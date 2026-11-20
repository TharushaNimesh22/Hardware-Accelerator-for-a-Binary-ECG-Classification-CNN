// ECG interpreter firmware for PicoRV32 on DE2-115
//  1. runs the Pan-Tompkins accelerator on the ECG record in shared RAM
//  2. runs the same algorithm in software (for verification + speed-up figure)
//  3. interprets the beats (HR, RR, HRV, rhythm flags) and reports over UART
//  Step 2.3: CNN self-test - runs the int8 CNN (beatcnn_ref.c) on the exported test
//            vectors and checks every logit against the Python integer model
#include <stdint.h>
#include "test_vectors.h"

// 1 = run only the CNN self-test, then print #END (fast simulation)
// 0 = run the CNN self-test, then the normal Pan-Tompkins flow
#define CNN_SELFTEST_ONLY 1

// CNN in software (beatcnn_ref.c). Do NOT include beatcnn_params.h here: its weight
// arrays are 'static', so including it twice would store the weights twice.
int beatcnn_classify(const int16_t *window, const int32_t *rr, int32_t *logits);

#define REG(a)      (*(volatile uint32_t *)(a))
#define ECG         ((volatile int32_t *)0x10000000)   // word0 = N, then samples
#define UART_DATA   REG(0x20000000)
#define UART_STAT   REG(0x20000004)
#define LEDR        REG(0x20000010)
#define LEDG        REG(0x20000014)
#define ACC_BASE    0x30000000
#define ACC_CTRL    REG(ACC_BASE + 0x00)
#define ACC_STATUS  REG(ACC_BASE + 0x04)
#define ACC_SRC     REG(ACC_BASE + 0x08)
#define ACC_LEN     REG(ACC_BASE + 0x0C)
#define ACC_NPEAKS  REG(ACC_BASE + 0x10)
#define ACC_CYCLES  REG(ACC_BASE + 0x14)
#define ACC_PEAK(i) REG(ACC_BASE + 0x1000 + 4 * (i))

#define FS        200
#define MAXPK     512
#define LEARN     400
#define REFRACT   40
#define DELAY     38

// UART helpers
static void uputc(char c) { while (UART_STAT & 1) { } UART_DATA = (uint8_t)c; }
static void uputs(const char *s) { while (*s) { if (*s == '\n') uputc('\r'); uputc(*s++); } }
static void uputu(uint32_t v) {
    char b[11]; int i = 0;
    do { b[i++] = '0' + v % 10; v /= 10; } while (v);
    while (i) uputc(b[--i]);
}
static void uputi(int32_t v) { if (v < 0) { uputc('-'); uputu((uint32_t)(-v)); } else uputu((uint32_t)v); }
static int ugetc(void) { while (!(UART_STAT & 2)) { } return UART_DATA & 0xff; }
static inline uint32_t rdcycle(void) { uint32_t c; __asm__ volatile ("rdcycle %0" : "=r"(c)); return c; }

//  software Pan-Tompkins
// Bit-exact with rtl/pan_tompkins_accel.v and tools/pt_golden.py
static int32_t xb[16], ub[64], wb[8], qb[64];
static int sw_peaks[MAXPK];

static int sw_pan_tompkins(const volatile int32_t *x, int len, int *peaks) {
    int32_t y1_d1 = 0, y1_d2 = 0, s = 0, qsum = 0;
    int32_t learn_max = 0, spki = 0, npki = 0, cur_max = 0, prev = 0;
    int cur_idx = 0, last_idx = 0, have_last = 0, rising = 0, np = 0;
    int rr_avg = 0, rr_valid = 0, cand_idx = 0, cand_valid = 0, pend = 0, pki_p = 0;
    int32_t cand_amp = 0, pk_p = 0;
    for (int i = 0; i < 16; i++) xb[i] = 0;
    for (int i = 0; i < 64; i++) { ub[i] = 0; qb[i] = 0; }
    for (int i = 0; i < 8;  i++) wb[i] = 0;

    for (int n = 0; n < len; n++) {
        xb[n & 15] = x[n];
        int32_t y1 = 2 * y1_d1 - y1_d2 + xb[n & 15] - 2 * xb[(n - 6) & 15] + xb[(n - 12) & 15];
        y1_d2 = y1_d1; y1_d1 = y1;

        ub[n & 63] = y1;
        s += y1 - ub[(n - 32) & 63];
        int32_t hp = (32 * ub[(n - 16) & 63] - s) >> 5;

        wb[n & 7] = hp;
        int32_t d = (2 * hp + wb[(n - 1) & 7] - wb[(n - 3) & 7] - 2 * wb[(n - 4) & 7]) >> 3;
        d >>= 2;
        if (d > 16383) d = 16383; else if (d < -16383) d = -16383;

        int32_t q = (d * d) >> 3;
        qb[n & 63] = q;
        qsum += q - qb[(n - 32) & 63];
        int32_t m = qsum >> 5;

        if (n < LEARN) {
            if (m > learn_max) learn_max = m;
            if (n == LEARN - 1) { spki = learn_max >> 1; npki = learn_max >> 3; }
        }
        int32_t thr = npki + ((spki - npki) >> 2);
        int rr_lim = rr_avg + (rr_avg >> 1) + (rr_avg >> 3);       // ~1.66 x RR average
        int sb = have_last && rr_valid && cand_valid && (n - last_idx > rr_lim) &&
                 (cand_amp > (thr >> 1)) && (cand_idx - last_idx > REFRACT);
        int acc = 0, a_idx = 0, a_sh = 3; int32_t a_amp = 0;
        if (sb) { acc = 1; a_idx = cand_idx; a_amp = cand_amp; a_sh = 2; }   // searchback
        else if (pend) {
            pend = 0;
            if (pk_p > thr) {
                if (!have_last || pki_p - last_idx > REFRACT) { acc = 1; a_idx = pki_p; a_amp = pk_p; }
            } else {
                npki += (pk_p - npki) >> 3;
                if (!cand_valid || pk_p > cand_amp) { cand_amp = pk_p; cand_idx = pki_p; cand_valid = 1; }
            }
        }
        if (acc) {
            if (np < MAXPK) peaks[np++] = a_idx - DELAY;
            if (have_last) {
                int rr = a_idx - last_idx;
                rr_avg = rr_valid ? rr_avg + ((rr - rr_avg) >> 3) : rr;
                rr_valid = 1;
            }
            last_idx = a_idx; have_last = 1; cand_valid = 0;
            spki += (a_amp - spki) >> a_sh;
        }
        // local-maximum tracking on the MWI signal
        if (rising) {
            if (m > cur_max) { cur_max = m; cur_idx = n; }
            else if (m < cur_max - (cur_max >> 2)) {
                rising = 0;
                if (n >= LEARN) { pend = 1; pk_p = cur_max; pki_p = cur_idx; }
            }
        } else if (m > prev) { rising = 1; cur_max = m; cur_idx = n; }
        prev = m;
    }
    return np;
}

//  interpretation
static int pk[MAXPK];

static void interpret(int np) {
    uputs("#BEATS\nbeat,sample,time_ms,rr_ms,hr_bpm,flag\n");
    uint32_t sum_rr = 0, min_hr = 999, max_hr = 0, n_rr = 0, n_irreg = 0;
    uint32_t sum_sq_diff = 0;               // fits: (1000 ms)^2 * 512 < 2^32
    int prev_rr = 0;
    for (int i = 0; i < np; i++) {
        int rr = (i > 0) ? (pk[i] - pk[i - 1]) * (1000 / FS) : 0;   // ms
        int hr = (rr > 0) ? 60000 / rr : 0;
        const char *flag = "N";
        if (i > 1) {
            int avg = (int)(sum_rr / n_rr);
            if (rr < (avg * 80) / 100)       { flag = "PREMATURE"; n_irreg++; }
            else if (rr > (avg * 150) / 100) { flag = "PAUSE";     n_irreg++; }
            int dd = rr - prev_rr; sum_sq_diff += (uint32_t)(dd * dd);
        }
        if (rr > 0) {
            sum_rr += rr; n_rr++;
            if ((uint32_t)hr < min_hr) min_hr = hr;
            if ((uint32_t)hr > max_hr) max_hr = hr;
            prev_rr = rr;
        }
        uputu(i); uputc(','); uputu(pk[i]); uputc(','); uputu(pk[i] * (1000 / FS)); uputc(',');
        uputu(rr); uputc(','); uputu(hr); uputc(','); uputs(flag); uputc('\n');
    }
    if (n_rr < 2) { uputs("#SUMMARY\nresult,too few beats detected\n"); return; }

    uint32_t mean_rr = sum_rr / n_rr;
    uint32_t mean_hr = 60000 / mean_rr;
    // RMSSD (ms): integer square root of mean squared successive difference
    uint32_t msd = sum_sq_diff / (n_rr - 1), r = 0;
    for (uint32_t b = 1u << 30; b; b >>= 2) { if (r + b <= msd) { msd -= r + b; r = (r >> 1) + b; } else r >>= 1; }
    uint32_t rmssd = r;

    const char *rhythm;
    if (rmssd * 100 > mean_rr * 12 && n_irreg * 5 > n_rr) rhythm = "IRREGULAR rhythm - possible atrial fibrillation";
    else if (n_irreg > 0)  rhythm = (mean_hr > 100) ? "Sinus tachycardia with ectopic beats" :
                                    (mean_hr < 60)  ? "Sinus bradycardia with ectopic beats" :
                                                      "Normal sinus rhythm with ectopic beats";
    else if (mean_hr > 100) rhythm = "Sinus tachycardia";
    else if (mean_hr < 60)  rhythm = "Sinus bradycardia";
    else                    rhythm = "Normal sinus rhythm";

    uputs("#SUMMARY\n");
    uputs("beats,");        uputu(np);      uputc('\n');
    uputs("mean_hr_bpm,");  uputu(mean_hr); uputc('\n');
    uputs("min_hr_bpm,");   uputu(min_hr);  uputc('\n');
    uputs("max_hr_bpm,");   uputu(max_hr);  uputc('\n');
    uputs("mean_rr_ms,");   uputu(mean_rr); uputc('\n');
    uputs("rmssd_ms,");     uputu(rmssd);   uputc('\n');
    uputs("irregular_beats,"); uputu(n_irreg); uputc('\n');
    uputs("rhythm,");       uputs(rhythm);  uputc('\n');
    LEDR = mean_hr;                          // heart rate in binary on red LEDs
}

// CNN self-test (step 2.3)
// Classifies each test beat with the C model on the PicoRV32 and compares the logits and the
// class bit-exactly with the Python integer model. Also measures the cycles per beat.
static int cnn_selftest(void) {
    uputs("\n#CNN_SELFTEST\n");
    uputs("vec,label,pred,exp_pred,");
    for (int o = 0; o < TV_NCLS; o++) { uputs("logit"); uputu(o); uputc(','); }
    for (int o = 0; o < TV_NCLS; o++) { uputs("exp"); uputu(o); uputc(','); }
    uputs("cycles,result\n");

    int pass = 0;
    uint32_t cyc_sum = 0, cyc_min = 0xffffffffu, cyc_max = 0;
    for (int n = 0; n < TV_N; n++) {
        int32_t lg[TV_NCLS];
        uint32_t t0 = rdcycle();
        int p = beatcnn_classify(&TV_WIN[n * 128], &TV_RR[n * 4], lg);
        uint32_t cyc = rdcycle() - t0;

        int ok = (p == TV_PRED[n]);
        for (int o = 0; o < TV_NCLS; o++) ok &= (lg[o] == TV_LOGITS[n * TV_NCLS + o]);
        pass += ok;
        cyc_sum += cyc;
        if (cyc < cyc_min) cyc_min = cyc;
        if (cyc > cyc_max) cyc_max = cyc;

        uputu(n); uputc(','); uputu(TV_LABEL[n]); uputc(','); uputu(p); uputc(','); uputu(TV_PRED[n]); uputc(',');
        for (int o = 0; o < TV_NCLS; o++) { uputi(lg[o]); uputc(','); }
        for (int o = 0; o < TV_NCLS; o++) { uputi(TV_LOGITS[n * TV_NCLS + o]); uputc(','); }
        uputu(cyc); uputc(','); uputs(ok ? "PASS" : "FAIL"); uputc('\n');
    }
    uputs("#CNN_SUMMARY\n");
    uputs("vectors,");          uputu(TV_N);          uputc('\n');
    uputs("passed,");           uputu(pass);          uputc('\n');
    uputs("sw_cycles_avg,");    uputu(cyc_sum / TV_N); uputc('\n');
    uputs("sw_cycles_min,");    uputu(cyc_min);       uputc('\n');
    uputs("sw_cycles_max,");    uputu(cyc_max);       uputc('\n');
    uputs("selftest,");         uputs(pass == TV_N ? "PASS" : "FAIL"); uputc('\n');
    LEDG = (pass == TV_N) ? 0x1F0 : 0x100;       // LEDG4..7 on = CNN self-test passed
    return pass == TV_N;
}

int main(void) {
    cnn_selftest();
#if CNN_SELFTEST_ONLY
    uputs("#END\n");
    for (;;) { }
#endif
    for (;;) {
        LEDG = 0;
        uputs("\n#ECG-ACCEL DE2-115 PicoRV32\n");
        int len = ECG[0];
        uputs("samples,"); uputu(len); uputs("\nfs_hz,200\n");

        //  hardware run
        ACC_SRC = 1; ACC_LEN = len;
        uint32_t t0 = rdcycle();
        ACC_CTRL = 1;
        while (ACC_STATUS & 1) { }
        uint32_t t1 = rdcycle();
        int hw_n = ACC_NPEAKS;
        for (int i = 0; i < hw_n; i++) pk[i] = ACC_PEAK(i);
        LEDG = 1;

        // software run (same algorithm on the CPU)
        uint32_t t2 = rdcycle();
        int sw_n = sw_pan_tompkins(ECG + 1, len, sw_peaks);
        uint32_t t3 = rdcycle();

        int mism = (hw_n != sw_n);
        for (int i = 0; i < hw_n && i < sw_n; i++) mism += (pk[i] != sw_peaks[i]);

        uputs("#PERF\n");
        uputs("hw_cycles,");    uputu(t1 - t0);        uputc('\n');
        uputs("hw_core_cycles,"); uputu(ACC_CYCLES);   uputc('\n');
        uputs("sw_cycles,");    uputu(t3 - t2);        uputc('\n');
        uputs("speedup_x,");    uputu((t3 - t2) / (t1 - t0)); uputc('\n');
        uputs("hw_sw_match,");  uputs(mism ? "NO" : "YES"); uputc('\n');
        if (!mism) LEDG = 3;

        interpret(hw_n);
        uputs("#END\nsend 'r' to run again\n");
        while (ugetc() != 'r');
    }
}
