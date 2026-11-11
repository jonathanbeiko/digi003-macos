// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// d003loop — engine-level loop latency over time (cable 003 OUTPUT 1 -> INPUT 1).
// Every 2 s writes a pseudo-random burst into the out ring ahead of the tx reader and locates it in the in ring.
// Usage: ./d003loop <seconds> <rate> <clock 0|2> <cadence 0 mirror|1 nominal>
#include "../engine/d003engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#define BURST 2048
#define M (D003_RING_FRAMES - 1)
int main(int argc, char **argv) {
    int secs = atoi(argv[1]); unsigned rate = atoi(argv[2]); int clock = atoi(argv[3]), cad = atoi(argv[4]);
    char err[256]; D003Engine *e = d003_open(err, sizeof err); if (!e) { printf("%s\n", err); return 1; }
    d003_set_cadence_mode(e, cad);
    if (d003_start(e, rate, (D003Clock)clock)) { printf("start failed\n"); return 1; }
    float burst[BURST]; unsigned seed = 7; for (int i = 0; i < BURST; i++) { seed = seed * 1103515245 + 12345; burst[i] = ((seed >> 16) & 1) ? 0.5f : -0.5f; }
    float *in = d003_in_ring(e), *out = d003_out_ring(e);
    sleep(1);
    for (int t = 0; t < secs; t += 2) {
        int64_t F = d003_tx_frames(e) + 1024;                       // ahead of the tx reader
        for (int k = 0; k < BURST; k++) out[((F + k) & M) * D003_MAX_CH + 0] = burst[k];
        while (d003_rx_frames(e) < F + BURST + 8000) usleep(2000);
        double best = 0; long lag = -1;
        for (long L = 0; L < 7000; L++) { double acc = 0; for (int k = 0; k < BURST; k++) acc += burst[k] * in[((F + L + k) & M) * D003_MAX_CH + 0]; if (fabs(acc) > best) { best = fabs(acc); lag = L; } }
        float pk = 0, nz = 0;
        for (long k = 0; k < 9000; k++) { float v = fabsf(in[((F + k) & M) * D003_MAX_CH + 0]); if (v > pk) pk = v; }
        for (long k = -20000; k < -12000; k++) { float v = fabsf(in[((F + k) & M) * D003_MAX_CH + 0]); if (v > nz) nz = v; }
        double ratio = best / (BURST * (nz > 0 ? nz : 1e-9));
        printf("  [pico ventana %.1f dB, ruido %.1f dB, corr/ruido %.2f] ", 20*log10(pk+1e-12), 20*log10(nz+1e-12), ratio);
        unsigned cl; uint64_t nom; d003_debug(e, &cl, &nom); D003Stats st; d003_get_stats(e, &st);
        printf("t=%3ds lag %5ld frames | tx-rx %4lld | cadFIFO %3u | nominal %llu | rxErr %llu dbc %llu | txLate %llu\n", t, lag,
               (long long)(d003_tx_frames(e) - d003_rx_frames(e)), cl, nom, st.rxBad, st.rxDbcErrors, st.txLate);
        fflush(stdout);
        if (getenv("READX")) { for (int r = 0; r < 15; r++) { bool det; unsigned er; d003_read_external(e, &det, &er); usleep(100000); } }
        else usleep(1500000);
    }
    d003_close(e); return 0;
}
