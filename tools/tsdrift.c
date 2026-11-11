// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// tsdrift — runs IO on two devices and prints how their Core Audio timelines drift apart
// (sample time at the same host time). Locked devices with correct timestamps stay constant.
// Usage: ./tsdrift "<dev A>" "<dev B>" [seconds]
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static OSStatus proc(AudioObjectID d, const AudioTimeStamp *n, const AudioBufferList *i, const AudioTimeStamp *it,
                     AudioBufferList *o, const AudioTimeStamp *ot, void *c) {
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
    int secs = argc > 3 ? atoi(argv[3]) : 60;
    AudioObjectID d[2] = { find(argv[1]), find(argv[2]) };
    if (!d[0] || !d[1]) { fprintf(stderr, "dispositivo no encontrado\n"); return 1; }
    AudioDeviceIOProcID p[2];
    for (int k = 0; k < 2; k++) { AudioDeviceCreateIOProcID(d[k], proc, NULL, &p[k]); AudioDeviceStart(d[k], p[k]); }
    sleep(2);
    double base = 0;
    for (int t = 0; t <= secs; t += 5) {
        UInt64 h = mach_absolute_time();
        AudioTimeStamp in = { .mHostTime = h, .mFlags = kAudioTimeStampHostTimeValid }, oa = { .mFlags = kAudioTimeStampSampleTimeValid }, ob = oa;
        OSStatus ea = AudioDeviceTranslateTime(d[0], &in, &oa), eb = AudioDeviceTranslateTime(d[1], &in, &ob);
        double diff = ob.mSampleTime - oa.mSampleTime;
        if (t == 0) base = diff;
        printf("t=%3ds  B-A desplazamiento %+8.1f frames%s\n", t, diff - base, (ea || eb) ? " (error)" : "");
        fflush(stdout);
        sleep(5);
    }
    for (int k = 0; k < 2; k++) { AudioDeviceStop(d[k], p[k]); AudioDeviceDestroyIOProcID(d[k], p[k]); }
    return 0;
}
