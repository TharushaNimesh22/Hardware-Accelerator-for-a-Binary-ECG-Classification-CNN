// Verifies beatcnn_ref.c + beatcnn_params.h against the exported test vectors.
// Build (from the folder that contains beatcnn_ref.c):
//   gcc -O2 -I <export_dir> check_export.c beatcnn_ref.c -o check_export
// Run:
//   ./check_export <export_dir>/test_vectors
#include <stdio.h>
#include <stdint.h>
#include "beatcnn_params.h"

int beatcnn_classify(const int16_t *window, const int32_t *rr, int32_t *logits);

static int read_hex(const char *dir, const char *name, int bits, long *out, int max) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r"); if (!f) { printf("cannot open %s\n", path); return -1; }
    unsigned long v; int n = 0;
    while (n < max && fscanf(f, "%lx", &v) == 1) {
        long s = (long)v;
        if (bits < 64 && (v >> (bits - 1)) & 1) s -= (1L << bits);   // sign-extend
        out[n++] = s;
    }
    fclose(f); return n;
}

#define MAXV 256
static long W[MAXV * BC_WIN], RR[MAXV * 4], LG[MAXV * BC_NCLS], PR[MAXV];

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "test_vectors";
    int nw = read_hex(dir, "window.hex", 16, W, MAXV * BC_WIN);
    int nr = read_hex(dir, "rr.hex", 32, RR, MAXV * 4);
    int nl = read_hex(dir, "logits.hex", 16, LG, MAXV * BC_NCLS);
    int np = read_hex(dir, "pred.hex", 8, PR, MAXV);
    if (nw < 0 || nr < 0 || nl < 0 || np < 0) return 2;
    int nv = np, bad = 0;
    for (int n = 0; n < nv; n++) {
        int16_t w[BC_WIN]; int32_t rr[4], lg[BC_NCLS];
        for (int i = 0; i < BC_WIN; i++) w[i] = (int16_t)W[n * BC_WIN + i];
        for (int i = 0; i < 4; i++) rr[i] = (int32_t)RR[n * 4 + i];
        int p = beatcnn_classify(w, rr, lg);
        int ok = (p == PR[n]);
        for (int o = 0; o < BC_NCLS; o++) ok &= (lg[o] == LG[n * BC_NCLS + o]);
        if (!ok) { bad++; printf("vector %d MISMATCH: pred %d (exp %ld)\n", n, p, PR[n]); }
    }
    printf("%d test vectors, %d mismatches -> %s\n", nv, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
