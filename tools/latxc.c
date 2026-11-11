// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// latxc — loop-latency measurement by cross-correlation (robust at low SNR).
// Plays a 2048-sample pseudo-random burst on <outCh> every second and finds it on <inA> (and optionally <inB>)
// of the same device, relative to the device's HAL timeline.
// Usage: LATDEV="<device>" ./latxc <bursts> <outCh> <inA> [inB]     (1-based channels within the device)
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define BURST 2048
#define MAXB 64
#define REC (44100 * 80)
static float burst[BURST];
static unsigned outG, inA, inB, wantB, nb; static int haveB;
static double t0In = -1, emitted[MAXB], nextAt = -1;
static float *recA, *recB;

static float *chan(const AudioBufferList *bl, unsigned g, unsigned *stride, unsigned *frames) {
    for (UInt32 b = 0; b < bl->mNumberBuffers; b++) {
        unsigned n = bl->mBuffers[b].mNumberChannels;
        if (g < n) { *stride = n; *frames = bl->mBuffers[b].mDataByteSize / 4 / n; return (float *)bl->mBuffers[b].mData + g; }
        g -= n;
    }
    return NULL;
}
static OSStatus proc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it,
                     AudioBufferList *out, const AudioTimeStamp *ot, void *ctx) {
    for (UInt32 b = 0; b < out->mNumberBuffers; b++) memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);
    unsigned os, of; float *o = chan(out, outG, &os, &of);
    if (nextAt < 0) nextAt = ot->mSampleTime + 22050;
    for (unsigned i = 0; o && i < of; i++) {
        double t = ot->mSampleTime + i;
        if (nb < wantB && t >= nextAt && t < nextAt + BURST) {
            o[i * os] = burst[(unsigned)(t - nextAt)];
            if (getenv("DUAL")) o[i * os + 1] = burst[(unsigned)(t - nextAt)];   // same burst on the next output channel
            if (t == nextAt) emitted[nb] = t;
            if (t == nextAt + BURST - 1) { nb++; nextAt += 44100; }
        }
    }
    unsigned sa, fa, sb = 0, fb = 0; const float *xa = chan(in, inA, &sa, &fa), *xb = haveB ? chan(in, inB, &sb, &fb) : NULL;
    if (t0In < 0) t0In = it->mSampleTime;
    for (unsigned i = 0; xa && i < fa; i++) {
        long k = (long)(it->mSampleTime - t0In) + i;
        if (k >= 0 && k < REC) { recA[k] = xa[i * sa]; if (xb) recB[k] = xb[i * sb]; }
    }
    return 0;
}
static AudioObjectID find(const char *name) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    for (UInt32 i = 0; i < sz / 4; i++) {
        CFStringRef n; UInt32 s = sizeof n; AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0 };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &n)) continue;
        char b[128]; CFStringGetCString(n, b, sizeof b, kCFStringEncodingUTF8); CFRelease(n);
        if (!strcmp(b, name)) return ids[i];
    }
    return 0;
}
static double locate(const float *rec, double emitAt) {      // lag (frames) of the burst after emitAt, searching 0..8000
    long base = (long)(emitAt - t0In); double best = 0; long bestLag = -1, maxLag = 8000;
    for (long lag = 0; lag < maxLag; lag++) {
        long s = base + lag; if (s < 0 || s + BURST >= REC) continue;
        double acc = 0; for (int k = 0; k < BURST; k++) acc += burst[k] * rec[s + k];
        if (fabs(acc) > best) { best = fabs(acc); bestLag = lag; }
    }
    // confidence: peak vs average correlation magnitude
    return bestLag;
}
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: LATDEV=dev ./latxc bursts outCh inA [inB]\n"); return 1; }
    wantB = atoi(argv[1]); if (wantB > MAXB) wantB = MAXB;
    outG = atoi(argv[2]) - 1; inA = atoi(argv[3]) - 1; haveB = argc > 4; inB = haveB ? atoi(argv[4]) - 1 : 0;
    const char *dn = getenv("LATDEV") ? getenv("LATDEV") : "Digi 003 Rack";
    AudioObjectID dev = find(dn); if (!dev) { fprintf(stderr, "no encuentro %s\n", dn); return 1; }
    unsigned seed = 12345; for (int i = 0; i < BURST; i++) { seed = seed * 1103515245 + 12345; burst[i] = ((seed >> 16) & 1) ? 0.5f : -0.5f; }
    recA = calloc(REC, 4); recB = calloc(REC, 4);
    AudioDeviceIOProcID p; AudioDeviceCreateIOProcID(dev, proc, NULL, &p); AudioDeviceStart(dev, p);
    while (nb < wantB) usleep(100000);
    usleep(300000);
    AudioDeviceStop(dev, p); AudioDeviceDestroyIOProcID(dev, p);
    double sum = 0;
    for (unsigned i = 0; i < nb; i++) {
        double a = locate(recA, emitted[i]);
        printf("ráfaga %2u: A %5.0f frames", i + 1, a);
        if (haveB) { double b = locate(recB, emitted[i]); printf("   B %5.0f frames   A-B %5.0f", b, a - b); }
        printf("\n"); sum += a;
    }
    printf("promedio A: %.1f frames\n", sum / nb);
    return 0;
}
