// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// catest — opens "Digi 003 Rack" through Core Audio, runs IO for N seconds,
// reports input peaks (ch 1-8), IO cycles and overloads. Optional: tone on outputs 1-2 (-30 dBFS).
// Usage: ./catest [seconds] [tone]
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

static float peak[18]; static unsigned long cycles, overloads; static double phase; static int tone; static double rate = 48000;

static OSStatus ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inT,
                       AudioBufferList *out, const AudioTimeStamp *outT, void *ctx) {
    cycles++;
    if (in && in->mNumberBuffers) {
        const AudioBuffer *b = &in->mBuffers[0]; const float *p = b->mData; unsigned ch = b->mNumberChannels;
        unsigned n = b->mDataByteSize / 4 / ch;
        for (unsigned i = 0; i < n; i++) for (unsigned c = 0; c < ch && c < 18; c++) { float v = fabsf(p[i * ch + c]); if (v > peak[c]) peak[c] = v; }
    }
    if (out && out->mNumberBuffers) {
        AudioBuffer *b = &out->mBuffers[0]; float *p = b->mData; unsigned ch = b->mNumberChannels;
        unsigned n = b->mDataByteSize / 4 / ch;
        memset(p, 0, b->mDataByteSize);
        if (tone) for (unsigned i = 0; i < n; i++) { float s = (getenv("AMP") ? atof(getenv("AMP")) : 0.0316f) * sinf(phase); phase += 2 * M_PI * 440 / rate; p[i * ch] = s; p[i * ch + 1] = s; }
    }
    return 0;
}
static OSStatus overload(AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a, void *c) { overloads++; return 0; }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 3; tone = argc > 2;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    AudioObjectID dev = 0;
    for (UInt32 i = 0; i < sz / 4; i++) {
        CFStringRef n; UInt32 s = sizeof n; AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0 };
        if (!AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &n)) { if (CFEqual(n, CFSTR("Digi 003 Rack"))) dev = ids[i]; CFRelease(n); }
    }
    if (!dev) { printf("no encuentro Digi 003 Rack\n"); return 1; }
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0 };
    sz = 8; AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &rate);
    AudioObjectPropertyAddress oa = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, 0 };
    AudioObjectAddPropertyListener(dev, &oa, overload, NULL);
    AudioDeviceIOProcID pid;
    AudioDeviceCreateIOProcID(dev, ioproc, NULL, &pid);
    OSStatus r = AudioDeviceStart(dev, pid);
    printf("AudioDeviceStart -> %d  (%.0f Hz%s)\n", (int)r, rate, tone ? ", tono 440 Hz -30 dBFS en salidas 1-2" : "");
    if (r) return 1;
    for (int t = 0; t < secs; t++) {
        sleep(1);
        printf("t=%ds ciclos %lu overloads %lu | ", t + 1, cycles, overloads);
        for (int c = 0; c < 8; c++) { printf("%6.1f ", peak[c] > 0 ? 20 * log10f(peak[c]) : -199.f); peak[c] = 0; }
        printf("dBFS\n");
    }
    AudioDeviceStop(dev, pid);
    AudioDeviceDestroyIOProcID(dev, pid);
    return 0;
}
