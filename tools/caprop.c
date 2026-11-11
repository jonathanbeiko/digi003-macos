// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// caprop — small Core Audio helper.
//   caprop list                      : devices with sample rate
//   caprop rate  "<name>" <hz>       : set nominal sample rate
//   caprop clock "<name>" <item>     : set clock-source selector (0 int,1 spdif,2 adat,3 word)
//   caprop adat  "<name>" on|off     : preamps 1-8 -> ADAT out (Digi 003 driver)
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static AudioObjectID find(const char *name, int list) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    for (UInt32 i = 0; i < sz / 4; i++) {
        CFStringRef n; UInt32 s = sizeof n; AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0 };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &n)) continue;
        char buf[128]; CFStringGetCString(n, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(n);
        if (list) {
            Float64 r = 0; UInt32 rs = 8; AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0 };
            AudioObjectGetPropertyData(ids[i], &ra, 0, NULL, &rs, &r);
            printf("%-32s %.0f Hz\n", buf, r);
        } else if (!strcmp(buf, name)) return ids[i];
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "list")) { find(NULL, 1); return 0; }
    if (argc < 4) return 1;
    AudioObjectID d = find(argv[2], 0);
    if (!d) { fprintf(stderr, "no encuentro %s\n", argv[2]); return 1; }
    if (!strcmp(argv[1], "rate")) {
        Float64 r = atof(argv[3]);
        AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0 };
        OSStatus e = AudioObjectSetPropertyData(d, &ra, 0, NULL, 8, &r);
        usleep(300000);
        Float64 now = 0; UInt32 s = 8; AudioObjectGetPropertyData(d, &ra, 0, NULL, &s, &now);
        printf("rate set -> %d, ahora %.0f Hz\n", (int)e, now);
        return e != 0;
    }
    if (!strcmp(argv[1], "clock")) {
        AudioObjectPropertyAddress ca = { kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, 0 };
        AudioObjectID ctl[8]; UInt32 s = sizeof ctl;
        if (AudioObjectGetPropertyData(d, &ca, 0, NULL, &s, ctl) || !s) { fprintf(stderr, "sin controles\n"); return 1; }
        UInt32 item = (UInt32)atoi(argv[3]);
        AudioObjectPropertyAddress ia = { kAudioSelectorControlPropertyCurrentItem, kAudioObjectPropertyScopeGlobal, 0 };
        OSStatus e = AudioObjectSetPropertyData(ctl[0], &ia, 0, NULL, 4, &item);
        UInt32 cur = 99; s = 4; AudioObjectGetPropertyData(ctl[0], &ia, 0, NULL, &s, &cur);
        printf("clock set -> %d, ahora item %u\n", (int)e, cur);
        return e != 0;
    }
    if (!strcmp(argv[1], "adat")) {
        AudioObjectPropertyAddress ca = { kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, 0 };
        AudioObjectID ctl[8]; UInt32 s = sizeof ctl;
        if (AudioObjectGetPropertyData(d, &ca, 0, NULL, &s, ctl) || s < 8) { fprintf(stderr, "sin control ADAT (driver viejo?)\n"); return 1; }
        UInt32 v = !strcmp(argv[3], "on");
        AudioObjectPropertyAddress va = { kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, 0 };
        OSStatus e = AudioObjectSetPropertyData(ctl[1], &va, 0, NULL, 4, &v);
        UInt32 cur = 9; s = 4; AudioObjectGetPropertyData(ctl[1], &va, 0, NULL, &s, &cur);
        printf("preamps -> ADAT: set -> %d, ahora %s\n", (int)e, cur ? "ON" : "OFF");
        return e != 0;
    }
    return 1;
}
