// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// d003test — exercises d003engine: plays a 440 Hz tone on outputs 1-2 (-12 dBFS),
// records analog inputs 1-8 to a WAV and prints stream / clock statistics.
// Usage: ./d003test [seconds] [rate] [out.wav]
#include "../engine/d003engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <mach/mach_time.h>

static void wav_header(FILE *f, unsigned rate, unsigned ch, uint32_t frames) {
    uint32_t data = frames * ch * 4, riff = 36 + data, fmtLen = 16, br = rate * ch * 4;
    uint16_t fmt = 3, chn = ch, ba = ch * 4, bits = 32;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&chn, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    unsigned rate = argc > 2 ? (unsigned)atoi(argv[2]) : 48000;
    const char *path = argc > 3 ? argv[3] : "d003-in.wav";
    char err[256];
    D003Engine *e = d003_open(err, sizeof err);
    if (!e) { fprintf(stderr, "d003_open: %s\n", err); return 1; }
    if (d003_start(e, rate, getenv("D003_CLOCK") ? (D003Clock)atoi(getenv("D003_CLOCK")) : D003_CLOCK_INTERNAL)) { fprintf(stderr, "d003_start failed\n"); d003_close(e); return 1; }
    printf("streaming %u Hz, %u canales\n", rate, d003_channels(e));
    if (argc > 4) d003_set_adat_loopback(e, true);

    FILE *wf = fopen(path, "wb"); wav_header(wf, rate, 8, 0);
    float *in = d003_in_ring(e), *out = d003_out_ring(e);
    int64_t rec = 0, gen = -1; uint32_t recFrames = 0;
    double phase = 0, inc = 2 * M_PI * 440.0 / rate;
    float peak[8] = {0};
    int64_t endFrames = (int64_t)seconds * rate; int64_t nextReport = rate;

    while (d003_rx_frames(e) < endFrames) {
        usleep(2000);
        int64_t rx = d003_rx_frames(e), tx = d003_tx_frames(e);
        // tone generator: keep the out ring filled ~4096 frames ahead of the tx reader
        if (gen < tx) gen = tx;
        while (gen < tx + 4096) {
            float s = 0.25f * (float)sin(phase); phase += inc; if (phase > 2 * M_PI) phase -= 2 * M_PI;
            float *o = out + (size_t)(gen & (D003_RING_FRAMES - 1)) * D003_MAX_CH;
            o[0] = s; o[1] = s; gen++;
        }
        // recorder
        while (rec < rx) {
            float *s = in + (size_t)(rec & (D003_RING_FRAMES - 1)) * D003_MAX_CH;
            fwrite(s, sizeof(float), 8, wf); recFrames++;
            for (int c = 0; c < 8; c++) if (fabsf(s[c]) > peak[c]) peak[c] = fabsf(s[c]);
            rec++;
        }
        if (rx >= nextReport) {
            D003Stats st; d003_get_stats(e, &st);
            uint64_t ht; double tpf; bool ok = d003_host_time_for(e, (double)rx, &ht, &tpf);
            double aheadMs = ok ? ((double)(int64_t)(mach_absolute_time() - ht)) / tpf / rate * 1000 : 0;
            printf("t=%2llds rx %llu pkts (vacios %llu malos %llu dbcErr %llu overrun %llu) tx %llu | tx-rx %lld fr | reloj %+.1f ppm jitter %.0f us, ahora-modelo %.2f ms | ",
                   (long long)(rx / rate), st.rxPackets, st.rxEmpty, st.rxBad, st.rxDbcErrors, st.rxOverruns, st.txPackets,
                   (long long)(tx - rx), st.ppm, st.jitterUs, aheadMs);
            for (int c = 0; c < 8; c++) printf("%5.1f ", peak[c] > 0 ? 20 * log10f(peak[c]) : -199.f);
            printf("dB\n");
            memset(peak, 0, sizeof peak);
            nextReport += rate;
        }
    }
    if (argc > 4) printf("loopback analog->ADAT: retardo %lld frames (%.2f ms)\n", (long long)d003_loopback_latency_frames(e), d003_loopback_latency_frames(e) * 1000.0 / rate);
    d003_stop(e);
    fseek(wf, 0, SEEK_SET); wav_header(wf, rate, 8, recFrames); fclose(wf);
    printf("grabado %u frames en %s\n", recFrames, path);
    d003_close(e);
    return 0;
}
