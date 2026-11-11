// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// mkaggregate — creates a public aggregate device "<master> + 003": the master interface is the clock source and
// the 003 is expected to follow it over ADAT (set the 003's clock to ADAT), so no drift correction is applied.
// Usage: ./mkaggregate "<master device name>"        e.g. ./mkaggregate "Universal Audio Thunderbolt"
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>

static CFStringRef uidOf(const char *name) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    for (UInt32 i = 0; i < sz / 4; i++) {
        CFStringRef n; UInt32 s = sizeof n; AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0 };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &n)) continue;
        char b[128]; CFStringGetCString(n, b, sizeof b, kCFStringEncodingUTF8); CFRelease(n);
        if (!strcmp(b, name)) {
            CFStringRef u; s = sizeof u; AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0 };
            if (!AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &s, &u)) return u;
        }
    }
    return NULL;
}
static CFDictionaryRef sub(CFStringRef uid) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceUIDKey), uid);
    int v = 0; CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceDriftCompensationKey), n); CFRelease(n);
    return d;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mkaggregate \"<master device name>\"\n"); return 1; }
    CFStringRef master = uidOf(argv[1]), digi = uidOf("Digi 003 Rack");
    if (!master || !digi) { fprintf(stderr, "device not found (%s / Digi 003 Rack)\n", argv[1]); return 1; }
    char name[256]; snprintf(name, sizeof name, "%s + 003", argv[1]);
    CFStringRef cname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), cname);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), CFSTR("io.github.jonathanbeiko.digi003.aggregate"));
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), master);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceClockDeviceKey), master);
    int zero = 0; CFNumberRef z = CFNumberCreate(NULL, kCFNumberIntType, &zero);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), z);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), z);
    CFDictionaryRef subs[2] = { sub(master), sub(digi) };
    CFArrayRef list = CFArrayCreate(NULL, (const void **)subs, 2, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), list);
    AudioObjectID agg = 0;
    OSStatus e = AudioHardwareCreateAggregateDevice(desc, &agg);
    printf("\"%s\": %s\n", name, e ? "error" : "created");
    return e != 0;
}
