// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// chpeak — records N seconds from a Core Audio device and prints the peak of every input channel
// (with the device's own channel names). Usage: ./chpeak "<device>" [seconds]
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define MAXCH 128
static float peak[MAXCH];
static OSStatus proc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it,
                     AudioBufferList *o, const AudioTimeStamp *ot, void *ctx) {
    unsigned base = 0;
    for (UInt32 b = 0; in && b < in->mNumberBuffers; b++) {
        const AudioBuffer *B = &in->mBuffers[b]; const float *p = B->mData; unsigned ch = B->mNumberChannels;
        unsigned n = B->mDataByteSize / 4 / (ch ? ch : 1);
        for (unsigned i = 0; i < n; i++) for (unsigned c = 0; c < ch && base + c < MAXCH; c++) { float v = fabsf(p[i * ch + c]); if (v > peak[base + c]) peak[base + c] = v; }
        base += ch;
    }
    if (o) for (UInt32 k = 0; k < o->mNumberBuffers; k++) memset(o->mBuffers[k].mData, 0, o->mBuffers[k].mDataByteSize);
    return 0;
}
int main(int argc, char **argv) {
    int secs = argc > 2 ? atoi(argv[2]) : 2;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64], dev = 0; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    for (UInt32 i = 0; i < sz / 4; i++) {
        CFStringRef n; UInt32 s = sizeof n; AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0 };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &n)) continue;
        char b[128]; CFStringGetCString(n, b, sizeof b, kCFStringEncodingUTF8); CFRelease(n);
        if (!strcmp(b, argv[1])) dev = ids[i];
    }
    if (!dev) { fprintf(stderr, "no encuentro %s\n", argv[1]); return 1; }
    AudioDeviceIOProcID p; AudioDeviceCreateIOProcID(dev, proc, NULL, &p);
    AudioDeviceStart(dev, p); sleep(secs); AudioDeviceStop(dev, p); AudioDeviceDestroyIOProcID(dev, p);
    AudioObjectPropertyAddress ca = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, 0 };
    UInt32 csz = 0; AudioObjectGetPropertyDataSize(dev, &ca, 0, NULL, &csz);
    AudioBufferList *bl = malloc(csz); AudioObjectGetPropertyData(dev, &ca, 0, NULL, &csz, bl);
    unsigned total = 0; for (UInt32 b = 0; b < bl->mNumberBuffers; b++) total += bl->mBuffers[b].mNumberChannels;
    for (unsigned c = 0; c < total && c < MAXCH; c++) {
        char name[64] = ""; CFStringRef n = NULL; UInt32 s = sizeof n;
        AudioObjectPropertyAddress ea = { kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, c + 1 };
        if (!AudioObjectGetPropertyData(dev, &ea, 0, NULL, &s, &n) && n) { CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8); CFRelease(n); }
        printf("in %2u %-22s %s\n", c + 1, name, peak[c] > 0 ? (char[16]){0} : "  silencio digital");
        if (peak[c] > 0) printf("\033[1A\033[30C%7.1f dBFS\n", 20 * log10f(peak[c]));
    }
    return 0;
}
