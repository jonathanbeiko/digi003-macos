// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// clockcmp — runs IO on two devices and compares their real sample rates against the host clock.
// Usage: ./clockcmp "<dev A>" "<dev B>" [seconds]
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { Float64 s0, s1; UInt64 h0, h1; int have; } Meas;
static OSStatus proc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *i, const AudioTimeStamp *it,
                     AudioBufferList *o, const AudioTimeStamp *ot, void *ctx) {
    Meas *m = ctx;
    if (!m->have) { m->s0 = now->mSampleTime; m->h0 = now->mHostTime; m->have = 1; }
    m->s1 = now->mSampleTime; m->h1 = now->mHostTime;
    if (o) for (UInt32 k = 0; k < o->mNumberBuffers; k++) memset(o->mBuffers[k].mData, 0, o->mBuffers[k].mDataByteSize);
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
int main(int argc, char **argv) {
    int secs = argc > 3 ? atoi(argv[3]) : 20;
    AudioObjectID d[2] = { find(argv[1]), find(argv[2]) };
    if (!d[0] || !d[1]) { fprintf(stderr, "dispositivo no encontrado\n"); return 1; }
    Meas m[2] = {0}; AudioDeviceIOProcID p[2];
    for (int k = 0; k < 2; k++) { AudioDeviceCreateIOProcID(d[k], proc, &m[k], &p[k]); AudioDeviceStart(d[k], p[k]); }
    sleep(secs);
    for (int k = 0; k < 2; k++) { AudioDeviceStop(d[k], p[k]); AudioDeviceDestroyIOProcID(d[k], p[k]); }
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double r[2];
    for (int k = 0; k < 2; k++) {
        double dt = (double)(m[k].h1 - m[k].h0) * tb.numer / tb.denom / 1e9;
        r[k] = (m[k].s1 - m[k].s0) / dt;
        printf("%-30s %.3f Hz  (%.1f s medidos)\n", argv[k + 1], r[k], dt);
    }
    printf("diferencia: %+.2f ppm\n", (r[1] / r[0] - 1) * 1e6);
    return 0;
}
