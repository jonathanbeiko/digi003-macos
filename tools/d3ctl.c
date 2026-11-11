// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// d3ctl — reads/writes the Digi003 driver's custom properties.
//   d3ctl status | levels | midiin
//   d3ctl route 0|4|8 | lowlat 0|1 | optical 0|1 | restart | keepalive <secs>
//   d3ctl midiout <hex bytes...>      e.g. d3ctl midiout 90 3c 64
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AudioObjectID find(void) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    for (UInt32 i = 0; i < sz / 4; i++) {
        CFStringRef u; UInt32 s = sizeof u; AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0 };
        if (AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &s, &u)) continue;
        bool ok = CFStringHasPrefix(u, CFSTR("Digi003:")); CFRelease(u);
        if (ok) return ids[i];
    }
    return 0;
}
static CFPropertyListRef get(AudioObjectID d, UInt32 sel) {
    CFPropertyListRef p = NULL; UInt32 s = sizeof p; AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, 0 };
    OSStatus e = AudioObjectGetPropertyData(d, &a, 0, NULL, &s, &p);
    if (e) fprintf(stderr, "get error %d\n", (int)e);
    return p;
}
static int set(AudioObjectID d, UInt32 sel, CFPropertyListRef v) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, 0 };
    OSStatus e = AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof v, &v);
    printf("set '%c%c%c%c' -> %d\n", sel >> 24, sel >> 16, sel >> 8, sel, (int)e);
    return e != 0;
}
static CFNumberRef num(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

int main(int argc, char **argv) {
    AudioObjectID d = find();
    if (!d) { fprintf(stderr, "Digi 003 no encontrado\n"); return 1; }
    const char *c = argc > 1 ? argv[1] : "status";
    if (!strcmp(c, "status")) { CFPropertyListRef p = get(d, 'd3st'); if (p) { CFShow(p); CFRelease(p); } return 0; }
    if (!strcmp(c, "levels")) {
        CFDictionaryRef p = get(d, 'd3lv'); if (!p) return 1;
        CFArrayRef in = CFDictionaryGetValue(p, CFSTR("in"));
        for (CFIndex i = 0; i < CFArrayGetCount(in); i++) { double v; CFNumberGetValue(CFArrayGetValueAtIndex(in, i), kCFNumberDoubleType, &v); printf("%6.1f ", v); }
        printf("dBFS (in 1-18)\n");
        CFArrayRef out = CFDictionaryGetValue(p, CFSTR("out"));
        for (CFIndex i = 0; i < CFArrayGetCount(out); i++) { double v; CFNumberGetValue(CFArrayGetValueAtIndex(out, i), kCFNumberDoubleType, &v); printf("%6.1f ", v); }
        printf("dBFS (out 1-18)\n"); CFRelease(p); return 0;
    }
    if (!strcmp(c, "midiin")) {
        CFDataRef p = get(d, 'd3mi'); if (!p) return 1;
        printf("%ld bytes:", (long)CFDataGetLength(p)); for (CFIndex i = 0; i < CFDataGetLength(p); i++) printf(" %02x", CFDataGetBytePtr(p)[i]);
        printf("\n"); CFRelease(p); return 0;
    }
    if (!strcmp(c, "route") && argc > 2) return set(d, 'd3rt', num(atoi(argv[2])));
    if (!strcmp(c, "lowlat") && argc > 2) return set(d, 'd3ll', num(atoi(argv[2])));
    if (!strcmp(c, "optical") && argc > 2) return set(d, 'd3op', num(atoi(argv[2])));
    if (!strcmp(c, "restart")) return set(d, 'd3rs', num(1));
    if (!strcmp(c, "keepalive") && argc > 2) return set(d, 'd3ka', num(atoi(argv[2])));
    if (!strcmp(c, "midiout")) {
        uint8_t b[256]; int n = 0;
        for (int i = 2; i < argc && n < 256; i++) b[n++] = (uint8_t)strtol(argv[i], NULL, 16);
        return set(d, 'd3mo', CFDataCreate(NULL, b, n));
    }
    fprintf(stderr, "uso: d3ctl status|levels|midiin|route N|lowlat N|optical N|restart|keepalive S|midiout HEX...\n");
    return 1;
}
