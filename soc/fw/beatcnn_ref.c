// Bit-exact C implementation of the int8 BeatCNN (matches int_forward() in the notebook).
// Uses export/beatcnn_params.h. Works on PicoRV32 (RV32IM): link with -lgcc for the 64-bit shifts.
#include <stdint.h>
#include "beatcnn_params.h"

static inline int32_t rq(int64_t acc, int32_t M, int sh, int32_t lo, int32_t hi) {
    int64_t y = (acc * M + ((int64_t)1 << (sh - 1))) >> sh;      // arithmetic shift
    return y < lo ? lo : (y > hi ? hi : (int32_t)y);
}

// out[co][l] = clamp(rq(B[co] + sum_ci,k W[co][ci][k] * in[ci][l+k-pad])), then optional maxpool-2
static void conv_relu_pool(const int8_t *in, int cin, int len, const int8_t *W, const int32_t *B,
                           const int32_t *M, const int8_t *SH, int cout, int K, int8_t *out) {
    int pad = K / 2;
    for (int co = 0; co < cout; co++)
        for (int l = 0; l < len; l += 2) {
            int32_t best = 0;                                        // ReLU output >= 0
            for (int j = 0; j < 2; j++) {
                int64_t acc = B[co];
                for (int ci = 0; ci < cin; ci++)
                    for (int k = 0; k < K; k++) {
                        int p = l + j + k - pad;
                        if (p >= 0 && p < len) acc += W[(co * cin + ci) * K + k] * in[ci * len + p];
                    }
                int32_t v = rq(acc, M[co], SH[co], 0, 127);
                if (v > best) best = v;
            }
            out[co * (len / 2) + l / 2] = (int8_t)best;
        }
}

// window: 128 raw ADC samples (R at index BC_PRE)
// rr: pre*256/avg8, post*256/avg8, pre*256/post, pre*256/avg32 (averages over PREVIOUS RR intervals)
// logits (optional, may be NULL): BC_NCLS int16 values. Returns class index (binary: 0 = N, 1 = ABN).
int beatcnn_classify(const int16_t *window, const int32_t *rr, int32_t *logits) {
    static int8_t q0[BC_WIN], p1[8 * 64], p2[16 * 32], cat[32 * 16 + 4], h[32];
    int32_t sum = 0;
    for (int i = 0; i < BC_WIN; i++) sum += window[i];
    int32_t mean = sum >> 7;                                         // floor(sum/128)
    for (int i = 0; i < BC_WIN; i++) q0[i] = (int8_t)rq(window[i] - mean, BC_IN_M, BC_IN_SH, -128, 127);
    conv_relu_pool(q0, 1, 128, BC_CONV1_W, BC_CONV1_B, BC_CONV1_M, BC_CONV1_SH, 8, 7, p1);
    conv_relu_pool(p1, 8, 64, BC_CONV2_W, BC_CONV2_B, BC_CONV2_M, BC_CONV2_SH, 16, 5, p2);
    conv_relu_pool(p2, 16, 32, BC_CONV3_W, BC_CONV3_B, BC_CONV3_M, BC_CONV3_SH, 32, 5, cat);   // 512 values
    for (int i = 0; i < 4; i++) cat[512 + i] = (int8_t)rq(rr[i] - BC_RR_MU[i], BC_RR_M[i], BC_RR_SH[i], -128, 127);
    for (int o = 0; o < 32; o++) {
        int64_t acc = BC_FC1_B[o];
        for (int i = 0; i < 516; i++) acc += BC_FC1_W[o * 516 + i] * cat[i];
        h[o] = (int8_t)rq(acc, BC_FC1_M[o], BC_FC1_SH[o], 0, 127);
    }
    int best = 0; int32_t bestv = -32768;
    for (int o = 0; o < BC_NCLS; o++) {
        int64_t acc = BC_FC2_B[o];
        for (int i = 0; i < 32; i++) acc += BC_FC2_W[o * 32 + i] * h[i];
        int32_t v = rq(acc, BC_FC2_M[o], BC_FC2_SH[o], -32768, 32767);
        if (logits) logits[o] = v;
        if (v > bestv) { bestv = v; best = o; }                      // first max wins (= numpy argmax)
    }
    return best;
}
