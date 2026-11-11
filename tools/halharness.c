// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// halharness — loads build/Digi003.driver like coreaudiod would and drives it for a few seconds.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

static AudioServerPlugInDriverRef drv;
static OSStatus hPropsChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a) {
    printf("  [host] PropertiesChanged obj %u sel '%.4s'\n", o, (char[]){ a->mSelector >> 24, a->mSelector >> 16, a->mSelector >> 8, a->mSelector });
    return 0;
}
static OSStatus hCopy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) { *d = NULL; return 0; }
static OSStatus hWrite(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { return 0; }
static OSStatus hDelete(AudioServerPlugInHostRef h, CFStringRef k) { return 0; }
static OSStatus hReqCfg(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 act, void *info) { return (*drv)->PerformDeviceConfigurationChange(drv, dev, act, info); }
static AudioServerPlugInHostInterface host = { hPropsChanged, hCopy, hWrite, hDelete, hReqCfg };

static void getstr(AudioObjectID o, UInt32 sel, UInt32 scope, UInt32 el, const char *label) {
    AudioObjectPropertyAddress a = { sel, scope, el }; CFStringRef s = NULL; UInt32 sz = sizeof s;
    OSStatus r = (*drv)->GetPropertyData(drv, o, 0, &a, 0, NULL, sz, &sz, &s);
    char buf[128] = "?"; if (!r && s) { CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(s); }
    printf("  %-22s %s\n", label, r ? "(error)" : buf);
}

int main(int argc, char **argv) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], strlen(argv[1]), true);
    CFPlugInRef plug = CFPlugInCreate(NULL, url);
    CFArrayRef f = CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plug);
    if (!f || !CFArrayGetCount(f)) { printf("no factory\n"); return 1; }
    IUnknownVTbl **iu = CFPlugInInstanceCreate(NULL, CFArrayGetValueAtIndex(f, 0), kAudioServerPlugInTypeUUID);
    (*iu)->QueryInterface(iu, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), (LPVOID *)&drv);
    AudioServerPlugInHostRef hr = &host;
    printf("Initialize -> %d\n", (*drv)->Initialize(drv, hr));

    AudioObjectPropertyAddress a = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID devs[4]; UInt32 sz = sizeof devs;
    (*drv)->GetPropertyData(drv, kAudioObjectPlugInObject, 0, &a, 0, NULL, sz, &sz, devs);
    printf("devices: %u\n", sz / 4);
    if (!sz) return 1;
    AudioObjectID dev = devs[0];
    getstr(dev, kAudioObjectPropertyName, 0x676c6f62, 0, "name");
    getstr(dev, kAudioDevicePropertyDeviceUID, 0x676c6f62, 0, "uid");
    getstr(dev, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 9, "input ch 9");
    getstr(dev, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeOutput, 17, "output ch 17");
    getstr(5, kAudioObjectPropertyName, 0x676c6f62, 0, "control");

    printf("StartIO -> %d\n", (*drv)->StartIO(drv, dev, 1));
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const UInt32 buf = 256; static float io[256 * 18];
    double rate = 48000, phase = 0;
    Float64 lastZ = -1; UInt64 lastH = 0, seed0 = 0; int zeroChanges = 0;
    double sampleTimeNow = 0;
    uint64_t t0 = mach_absolute_time(); float peak = 0;
    while (mach_absolute_time() - t0 < 4ull * 1000000000ull * tb.denom / tb.numer) {
        Float64 st; UInt64 ht, seed;
        if ((*drv)->GetZeroTimeStamp(drv, dev, 1, &st, &ht, &seed)) { usleep(1000); continue; }
        if (st != lastZ) { if (lastZ >= 0) { double dt = (double)(ht - lastH) * tb.numer / tb.denom / 1e9; printf("  zero ts %.0f  dt %.6f s -> %.2f Hz\n", st, dt, (st - lastZ) / dt); } lastZ = st; lastH = ht; seed0 = seed; zeroChanges++; }
        // current sample time from zero ts
        double tpf = 1e9 / rate * tb.denom / tb.numer;
        sampleTimeNow = st + (double)(mach_absolute_time() - ht) / tpf;
        AudioServerPlugInIOCycleInfo ci = {0};
        ci.mInputTime.mSampleTime = floor(sampleTimeNow) - buf - 128;
        ci.mOutputTime.mSampleTime = floor(sampleTimeNow) + 640 + buf;
        (*drv)->DoIOOperation(drv, dev, 3, 1, kAudioServerPlugInIOOperationReadInput, buf, &ci, io, NULL);
        for (UInt32 i = 0; i < buf * 18; i++) if (fabsf(io[i]) > peak) peak = fabsf(io[i]);
        for (UInt32 i = 0; i < buf; i++) { float s = 0.1f * sinf(phase); phase += 2 * M_PI * 440 / rate; for (int c = 0; c < 18; c++) io[i * 18 + c] = c < 2 ? s : 0; }
        (*drv)->DoIOOperation(drv, dev, 4, 1, kAudioServerPlugInIOOperationWriteMix, buf, &ci, io, NULL);
        usleep((useconds_t)(buf / rate * 1e6));
    }
    printf("input peak %.1f dBFS, zero-ts updates %d, seed %llu\n", 20 * log10(peak + 1e-12), zeroChanges, seed0);
    printf("StopIO -> %d\n", (*drv)->StopIO(drv, dev, 1));
    return 0;
}
