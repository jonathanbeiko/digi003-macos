// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// Digi003Driver.c — AudioServerPlugIn (HAL driver) for Digidesign 003 / 003 Rack on Apple Silicon.
// Streaming is done in-process through d003engine (IOFireWireLib), no kernel extension.
//
// Objects: plug-in (1), device (2), input stream (3), output stream (4), clock-source selector (5),
//          "Preamps -> ADAT Out" boolean (6): analog 1-8 mirrored to ADAT out 1-8, runs even with no client.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <syslog.h>
#include <math.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#include "../engine/d003engine.h"

#include <os/log.h>
// os_log reaches the unified log from the sandboxed driver host (syslog does not). See: log stream --predicate 'subsystem == "io.github.jonathanbeiko.digi003"'
static os_log_t gLog;
#define LOG(fmt, ...) os_log(gLog ? gLog : OS_LOG_DEFAULT, "Digi003: " fmt, ##__VA_ARGS__)

enum { kObjPlugIn = kAudioObjectPlugInObject, kObjDevice = 2, kObjInStream = 3, kObjOutStream = 4, kObjClock = 5, kObjLoop = 6 };

#define kManufacturer     "Digidesign"
#define kDeviceName       "Digi 003 Rack"
#define kDeviceUIDPrefix  "Digi003:"
#define kModelUID         "Digi003Rack"
#define kZeroPeriod       8192
#define kInSafety         128
#define kOutSafety        384
#define kNumCh            D003_MAX_CH

static const Float64 kRates[] = { 44100.0, 48000.0 };
static const char *kClockNames[] = { "Internal", "S/PDIF", "ADAT", "Word Clock" };

// ------------------------------------------------------------------ state
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost;
static UInt32 gRefCount = 0;

static atomic_bool gPresent;            // device on the bus
static char gGUID[32] = "0";
static Float64 gRate = 48000.0;
static UInt32 gClock = D003_CLOCK_INTERNAL;
static UInt32 gIOCount = 0;
static D003Engine *gEngine;
static UInt64 gSeed = 1;
static Float64 gPendingRate;
static int gRoute = 0;                  // preamp mode: analog channels mirrored to ADAT out (0, 4, 8)
static int gLastRoute = 8;              // what the boolean switch turns back on
static bool gLowLatency = false;        // smaller output ring
static UInt32 gOptical = 0;             // optical port: 0 ADAT, 1 S/PDIF
static uint64_t gKeepAliveUntil;        // mach time until which the menu-bar app wants the stream (MIDI)
static dispatch_queue_t gNotifyQ;
#define gLoop (gRoute != 0)
static bool keepalive_active(void) { return gKeepAliveUntil > mach_absolute_time(); }
static bool gRateChangedByClock;        // set under lock, notified after unlock
static dispatch_source_t gWatchdog;
static int64_t gLastRx = -1; static int gStall;
static uint64_t gLastErr; static int gErrTicks; static int gUnlockTicks;
static bool gAsleep;                    // system sleeping: keep the engine down
static io_connect_t gRootPort; static IONotificationPortRef gPwrPort; static io_object_t gPwrNotifier;
#define kStatusSelector 'd3st'          // custom properties (CFPropertyList) used by the menu bar app:
#define kSelLevels      'd3lv'          //   get: {in:[dBFS x18], out:[dBFS x18]} peaks since last read
#define kSelRoute       'd3rt'          //   get/set: 0 / 4 / 8 analog channels -> ADAT out
#define kSelLowLatency  'd3ll'          //   get/set: 0/1
#define kSelOptical     'd3op'          //   get/set: 0 ADAT, 1 S/PDIF
#define kSelRestart     'd3rs'          //   set: restart the stream
#define kSelKeepAlive   'd3ka'          //   set: seconds the stream must stay up without HAL clients (MIDI)
#define kSelMidiIn      'd3mi'          //   get: CFData with pending MIDI IN bytes (change notified)
#define kSelMidiOut     'd3mo'          //   set: CFData to send to MIDI OUT
static const AudioObjectPropertySelector kCustomSels[] = { kStatusSelector, kSelLevels, kSelRoute, kSelLowLatency, kSelOptical, kSelRestart, kSelKeepAlive, kSelMidiIn, kSelMidiOut };
#define kNumCustom (sizeof kCustomSels / sizeof kCustomSels[0])
static bool is_custom(AudioObjectPropertySelector s) { for (unsigned i = 0; i < kNumCustom; i++) if (kCustomSels[i] == s) return true; return false; }
static bool is_custom_settable(AudioObjectPropertySelector s) { return s == kSelRoute || s == kSelLowLatency || s == kSelOptical || s == kSelRestart || s == kSelKeepAlive || s == kSelMidiOut; }

static IONotificationPortRef gNotifyPort;
static io_iterator_t gArriveIter, gLeaveIter;

// ------------------------------------------------------------------ helpers
static CFStringRef cfstr(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static void channel_name(UInt32 ch, char *buf, size_t n) {   // ch is 1-based
    if (ch >= 1 && ch <= 8)        snprintf(buf, n, "Analog %u", ch);
    else if (ch == 9)              snprintf(buf, n, "S/PDIF L");
    else if (ch == 10)             snprintf(buf, n, "S/PDIF R");
    else if (ch >= 11 && ch <= 18) snprintf(buf, n, "ADAT %u", ch - 10);
    else                          snprintf(buf, n, "%u", ch);
}

static void fill_asbd(AudioStreamBasicDescription *f, Float64 rate) {
    memset(f, 0, sizeof *f);
    f->mSampleRate = rate;
    f->mFormatID = kAudioFormatLinearPCM;
    f->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f->mBytesPerPacket = 4 * kNumCh; f->mFramesPerPacket = 1; f->mBytesPerFrame = 4 * kNumCh;
    f->mChannelsPerFrame = kNumCh; f->mBitsPerChannel = 32;
}

static void engine_update_locked(void);
static void midi_in_ready(void *ctx);
static void flush_notifications(void);

static void notify(AudioObjectID obj, AudioObjectPropertySelector sel) {
    if (!gHost) return;
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    gHost->PropertiesChanged(gHost, obj, 1, &a);
}

// ------------------------------------------------------------------ hot-plug
static void scan_guid(io_object_t d) {
    CFTypeRef r = IORegistryEntryCreateCFProperty(d, CFSTR("GUID"), kCFAllocatorDefault, 0);
    long long g = 0; if (r) { CFNumberGetValue(r, kCFNumberLongLongType, &g); CFRelease(r); }
    snprintf(gGUID, sizeof gGUID, "%016llx", g);
}
static void on_arrive(void *ctx, io_iterator_t it) {
    io_object_t d; bool any = false;
    while ((d = IOIteratorNext(it))) { scan_guid(d); IOObjectRelease(d); any = true; }
    if (any && !atomic_exchange(&gPresent, true)) {
        LOG("003 connected (GUID %{public}s)", gGUID);
        notify(kObjPlugIn, kAudioPlugInPropertyDeviceList);
        pthread_mutex_lock(&gLock); engine_update_locked(); pthread_mutex_unlock(&gLock);
        flush_notifications();
    }
}
static void on_leave(void *ctx, io_iterator_t it) {
    io_object_t d; bool any = false;
    while ((d = IOIteratorNext(it))) { IOObjectRelease(d); any = true; }
    if (!any || !atomic_exchange(&gPresent, false)) return;
    LOG("003 disconnected");
    pthread_mutex_lock(&gLock);
    if (gEngine) { d003_close(gEngine); gEngine = NULL; }
    gIOCount = 0;
    pthread_mutex_unlock(&gLock);
    notify(kObjDevice, kAudioDevicePropertyDeviceIsAlive);
    notify(kObjPlugIn, kAudioPlugInPropertyDeviceList);
}
static CFMutableDictionaryRef digi_match(void) {
    CFMutableDictionaryRef m = IOServiceMatching("IOFireWireDevice");
    int v = 0x00a07e; CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFDictionarySetValue(m, CFSTR("Vendor_ID"), n); CFRelease(n);
    return m;
}
static void start_hotplug(void) {
    gNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(gNotifyPort, dispatch_queue_create("Digi003.hotplug", NULL));
    IOServiceAddMatchingNotification(gNotifyPort, kIOFirstMatchNotification, digi_match(), on_arrive, NULL, &gArriveIter);
    on_arrive(NULL, gArriveIter);
    IOServiceAddMatchingNotification(gNotifyPort, kIOTerminatedNotification, digi_match(), on_leave, NULL, &gLeaveIter);
    on_leave(NULL, gLeaveIter);
}

// ------------------------------------------------------------------ prefs
// Settings live in the HAL's per-plug-in storage (the sandboxed driver process cannot use CFPreferences).
static bool storage_number(CFStringRef key, double *v) {
    CFPropertyListRef r = NULL;
    if (!gHost || gHost->CopyFromStorage(gHost, key, &r) || !r) return false;
    bool ok = CFGetTypeID(r) == CFNumberGetTypeID() && CFNumberGetValue(r, kCFNumberDoubleType, v);
    CFRelease(r);
    return ok;
}
static void load_prefs(void) {
    double v;
    if (storage_number(CFSTR("SampleRate"), &v) && (v == 44100 || v == 48000)) gRate = v;
    if (storage_number(CFSTR("ClockSource"), &v) && v >= 0 && v <= 3) gClock = (UInt32)v;
    if (storage_number(CFSTR("AdatLoopback"), &v)) gRoute = v == 4 ? 4 : v != 0 ? 8 : 0;   // old builds stored 1
    if (storage_number(CFSTR("AdatLastRoute"), &v) && (v == 4 || v == 8)) gLastRoute = (int)v;
    if (storage_number(CFSTR("LowLatency"), &v)) gLowLatency = v != 0;
    if (storage_number(CFSTR("Optical"), &v)) gOptical = v != 0;
    LOG("settings: %.0f Hz, clock %u, preamps->ADAT %d, low latency %d, optical %u", gRate, gClock, gRoute, gLowLatency, gOptical);
}
static void store_number(CFStringRef key, double v) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &v);
    gHost->WriteToStorage(gHost, key, n);
    CFRelease(n);
}
// Called with gLock held; the host call is dispatched so it never runs under our lock.
static void save_prefs(void) {
    double r = gRate, c = gClock, l = gRoute, lr = gLastRoute, ll = gLowLatency, op = gOptical;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        store_number(CFSTR("SampleRate"), r); store_number(CFSTR("ClockSource"), c); store_number(CFSTR("AdatLoopback"), l);
        store_number(CFSTR("AdatLastRoute"), lr); store_number(CFSTR("LowLatency"), ll); store_number(CFSTR("Optical"), op);
    });
}

// ------------------------------------------------------------------ engine control (gLock held)
static OSStatus engine_start_locked(void) {
    char err[256];
    if (!atomic_load(&gPresent)) return kAudioHardwareNotRunningError;
    if (!gEngine) {
        gEngine = d003_open(err, sizeof err);
        if (!gEngine) { LOG("open failed: %{public}s", err); return kAudioHardwareUnspecifiedError; }
    }
    if (gClock != D003_CLOCK_INTERNAL) {
        bool det = false; unsigned er = 0;
        if (d003_read_external(gEngine, &det, &er) && det && (er == 44100 || er == 48000) && er != (unsigned)gRate) {
            LOG("following external clock: %u Hz", er);
            gRate = er; save_prefs(); gRateChangedByClock = true;
        }
    }
    d003_set_low_latency(gEngine, gLowLatency);
    d003_set_optical_mode(gEngine, gOptical);
    d003_set_midi_in_callback(gEngine, midi_in_ready, NULL);
    if (d003_start(gEngine, (unsigned)gRate, (D003Clock)gClock)) {
        LOG("start failed"); d003_close(gEngine); gEngine = NULL; return kAudioHardwareUnspecifiedError;
    }
    // wait for the clock model (needs ~2 windows of 125 ms)
    for (int i = 0; i < 200; i++) { if (d003_host_time_for(gEngine, 0, NULL, NULL)) break; usleep(5000); }
    if (!d003_host_time_for(gEngine, 0, NULL, NULL)) {
        LOG("no data from device (clock source %{public}s locked?)", kClockNames[gClock]);
        d003_close(gEngine); gEngine = NULL; return kAudioHardwareUnspecifiedError;
    }
    gSeed++;
    d003_set_adat_route(gEngine, gOptical ? 0 : gRoute);
    gLastRx = -1; gStall = 0; gLastErr = 0; gErrTicks = 0; gUnlockTicks = 0;
    LOG("running at %.0f Hz, clock %{public}s%{public}s", gRate, kClockNames[gClock], gLoop ? ", preamps -> ADAT" : "");
    return 0;
}
static OSStatus engine_start_locked(void);
// Restart the stream on the same engine object: never free it while HAL IO threads may be inside DoIOOperation.
static void engine_restart_locked(void) {
    if (gEngine) d003_stop(gEngine);
    engine_start_locked();
}
static void engine_stop_locked(void) {
    if (gEngine) { d003_close(gEngine); gEngine = NULL; }
}
// Brings the engine to the wanted state: running if a HAL client is doing IO or preamp mode is on.
static void engine_update_locked(void) {
    bool want = !gAsleep && atomic_load(&gPresent) && (gIOCount > 0 || gLoop || keepalive_active());
    if (want && !gEngine) engine_start_locked();
    else if (!want && gEngine) engine_stop_locked();
    if (gEngine) d003_set_adat_route(gEngine, gOptical ? 0 : gRoute);
}
static void flush_notifications(void) {
    bool changed;
    pthread_mutex_lock(&gLock); changed = gRateChangedByClock; gRateChangedByClock = false; pthread_mutex_unlock(&gLock);
    if (changed) {
        notify(kObjDevice, kAudioDevicePropertyNominalSampleRate);
        notify(kObjInStream, kAudioStreamPropertyVirtualFormat); notify(kObjInStream, kAudioStreamPropertyPhysicalFormat);
        notify(kObjOutStream, kAudioStreamPropertyVirtualFormat); notify(kObjOutStream, kAudioStreamPropertyPhysicalFormat);
    }
}
// Preamp mode runs with no client: if the clock master changes rate or the ADAT link drops, the 003 stops
// sending; restart it (picking up the new external rate). Only when no HAL client owns the IO.
static void watchdog_tick(void) {
    pthread_mutex_lock(&gLock);
    if (gEngine && gIOCount == 0 && !gLoop && !keepalive_active()) engine_stop_locked();   // MIDI keep-alive expired
    bool requestRate = false;
    if ((gLoop || keepalive_active() || gIOCount > 0) && atomic_load(&gPresent) && !gAsleep) {
        if (!gEngine) engine_start_locked();     // also after a failed restart while clients are attached
        else {
            int64_t rx = d003_rx_frames(gEngine);
            gStall = (rx == gLastRx) ? gStall + 1 : 0;
            gLastRx = rx;
            // a session that keeps producing packet errors (e.g. locked while the master clock was still settling after wake)
            D003Stats st; d003_get_stats(gEngine, &st);
            uint64_t err = st.rxBad + st.rxDbcErrors;
            gErrTicks = err > gLastErr ? gErrTicks + 1 : 0;
            gLastErr = err;
            bool det = false; unsigned er = 0, restart = gStall >= 2;
            if (gErrTicks >= 5) { LOG("continuous packet errors"); restart = true; }
            if (!restart && gClock != D003_CLOCK_INTERNAL && d003_read_external(gEngine, &det, &er)) {
                if (det && (er == 44100 || er == 48000) && er != (unsigned)gRate) {
                    LOG("external clock now %u Hz", er);
                    if (gIOCount > 0) { gPendingRate = er; requestRate = true; }   // clients: let the HAL stop IO and reconfigure
                    else restart = true;
                }
                // a session started without the external clock (e.g. the clock master booted later) never re-locks by itself
                gUnlockTicks = det ? 0 : gUnlockTicks + 1;
                if (gUnlockTicks >= 5) { LOG("external clock not locked, retrying"); restart = true; }
            }
            if (restart) { LOG("restarting stream"); engine_restart_locked(); }
        }
    }
    pthread_mutex_unlock(&gLock);
    if (requestRate && gHost) gHost->RequestDeviceConfigurationChange(gHost, kObjDevice, 1, NULL);
    flush_notifications();
}

// ------------------------------------------------------------------ MIDI (bridged to Core MIDI by the menu-bar app)
static atomic_bool gMidiNotifyPending;
static void midi_in_ready(void *ctx) {        // engine thread: coalesce, never block it
    if (atomic_exchange(&gMidiNotifyPending, true)) return;
    dispatch_async(gNotifyQ, ^{ atomic_store(&gMidiNotifyPending, false); notify(kObjDevice, kSelMidiIn); });
}

// ------------------------------------------------------------------ sleep / wake
// FireWire powers down with the system: close the stream cleanly before sleep, bring it back after wake.
static void power_cb(void *ref, io_service_t svc, natural_t msg, void *arg) {
    switch (msg) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(gRootPort, (long)arg);
        break;
    case kIOMessageSystemWillSleep:
        LOG("system sleep: stopping stream");
        pthread_mutex_lock(&gLock); gAsleep = true; engine_stop_locked(); pthread_mutex_unlock(&gLock);
        IOAllowPowerChange(gRootPort, (long)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        LOG("system wake");
        // give the FireWire bus time to re-enumerate the 003
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            pthread_mutex_lock(&gLock); gAsleep = false; gStall = 0; gLastRx = -1; engine_update_locked(); pthread_mutex_unlock(&gLock);
            flush_notifications();
        });
        break;
    }
}
static void start_power(void) {
    gRootPort = IORegisterForSystemPower(NULL, &gPwrPort, power_cb, &gPwrNotifier);
    if (!gRootPort) { LOG("IORegisterForSystemPower failed"); return; }
    IONotificationPortSetDispatchQueue(gPwrPort, dispatch_queue_create("Digi003.power", NULL));
}

// ------------------------------------------------------------------ live status (custom property)
static CFDictionaryRef copy_status(void) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    #define SETB(k, v) CFDictionarySetValue(d, CFSTR(k), (v) ? kCFBooleanTrue : kCFBooleanFalse)
    #define SETN(k, v) do { double _v = (v); CFNumberRef _n = CFNumberCreate(NULL, kCFNumberDoubleType, &_v); CFDictionarySetValue(d, CFSTR(k), _n); CFRelease(_n); } while (0)
    pthread_mutex_lock(&gLock);
    SETB("present", atomic_load(&gPresent));
    SETB("streaming", gEngine != NULL && d003_running(gEngine));
    SETB("asleep", gAsleep);
    SETB("preamps", gLoop);
    SETN("route", gRoute);
    SETB("lowLatency", gLowLatency);
    SETN("optical", gOptical);
    SETB("midiKeepAlive", keepalive_active());
    SETN("clients", gIOCount);
    SETN("rate", gRate);
    SETN("clock", gClock);
    if (gEngine) {
        bool det = false; unsigned er = 0;
        if (d003_read_external(gEngine, &det, &er)) { SETB("extDetected", det); SETN("extRate", er); }
        D003Stats st; d003_get_stats(gEngine, &st);
        SETN("rxErrors", st.rxBad + st.rxDbcErrors); SETN("rxOverruns", st.rxOverruns); SETN("ppm", st.ppm);
        SETN("txLate", st.txLate); SETB("realtime", d003_realtime(gEngine));
        int64_t lead = d003_loopback_latency_frames(gEngine);
        if (lead > 0) SETN("loopMs", lead * 1000.0 / gRate);
    }
    pthread_mutex_unlock(&gLock);
    return d;
}

static CFDictionaryRef copy_levels(void) {
    float in[D003_MAX_CH] = {0}, out[D003_MAX_CH] = {0};
    pthread_mutex_lock(&gLock); if (gEngine) d003_take_peaks(gEngine, in, out); pthread_mutex_unlock(&gLock);
    CFMutableArrayRef ai = CFArrayCreateMutable(NULL, D003_MAX_CH, &kCFTypeArrayCallBacks), ao = CFArrayCreateMutable(NULL, D003_MAX_CH, &kCFTypeArrayCallBacks);
    for (int c = 0; c < D003_MAX_CH; c++) {
        double a = in[c] > 0 ? 20 * log10(in[c]) : -144, b = out[c] > 0 ? 20 * log10(out[c]) : -144;
        CFNumberRef na = CFNumberCreate(NULL, kCFNumberDoubleType, &a), nb = CFNumberCreate(NULL, kCFNumberDoubleType, &b);
        CFArrayAppendValue(ai, na); CFArrayAppendValue(ao, nb); CFRelease(na); CFRelease(nb);
    }
    const void *k[2] = { CFSTR("in"), CFSTR("out") }, *v[2] = { ai, ao };
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(ai); CFRelease(ao);
    return d;
}

// ------------------------------------------------------------------ COM boilerplate
static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out);
static ULONG AddRef(void *drv);
static ULONG Release(void *drv);
static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host);
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id) { return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) { return 0; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci) { return 0; }
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info);
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) { return 0; }
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *out);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inSize, UInt32 *outSize, void *out);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inSize, const void *in);
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client);
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed);
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace);
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci) { return 0; }
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary);
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci) { return 0; }

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release,
    Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange,
    HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

void *Digi003_Create(CFAllocatorRef alloc, CFUUIDRef type) {
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriverRef;
}

static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out) {
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gLock); gRefCount++; pthread_mutex_unlock(&gLock);
        *out = gDriverRef; r = S_OK;
    }
    CFRelease(req);
    return r;
}
static ULONG AddRef(void *drv) { pthread_mutex_lock(&gLock); ULONG r = ++gRefCount; pthread_mutex_unlock(&gLock); return r; }
static ULONG Release(void *drv) { pthread_mutex_lock(&gLock); ULONG r = gRefCount ? --gRefCount : 0; pthread_mutex_unlock(&gLock); return r; }

static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host) {
    gHost = host;
    gLog = os_log_create("io.github.jonathanbeiko.digi003", "driver");
    gNotifyQ = dispatch_queue_create("Digi003.notify", NULL);
    load_prefs();
    start_power();
    start_hotplug();
    gWatchdog = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_queue_create("Digi003.watchdog", NULL));
    dispatch_source_set_timer(gWatchdog, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(gWatchdog, ^{ watchdog_tick(); });
    dispatch_resume(gWatchdog);
    LOG("driver loaded, device %{public}s", atomic_load(&gPresent) ? "present" : "absent");
    return 0;
}

// ------------------------------------------------------------------ properties
static bool is_stream(AudioObjectID o) { return o == kObjInStream || o == kObjOutStream; }

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a) {
    UInt32 s = a->mSelector;
    switch (obj) {
    case kObjPlugIn:
        return s == kAudioObjectPropertyBaseClass || s == kAudioObjectPropertyClass || s == kAudioObjectPropertyOwner ||
               s == kAudioObjectPropertyManufacturer || s == kAudioObjectPropertyOwnedObjects ||
               s == kAudioPlugInPropertyDeviceList || s == kAudioPlugInPropertyTranslateUIDToDevice ||
               s == kAudioPlugInPropertyResourceBundle;
    case kObjDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate: case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyClockIsStable:
        case kAudioObjectPropertyCustomPropertyInfoList:
            return true;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo: case kAudioDevicePropertyPreferredChannelLayout:
            return a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeOutput;
        case kAudioObjectPropertyElementName:
            return (a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeOutput) &&
                   a->mElement >= 1 && a->mElement <= kNumCh;
        }
        return is_custom(s);
    case kObjInStream: case kObjOutStream:
        switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;
    case kObjLoop:
        switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioObjectPropertyName:
        case kAudioControlPropertyScope: case kAudioControlPropertyElement: case kAudioBooleanControlPropertyValue:
            return true;
        }
        return false;
    case kObjClock:
        switch (s) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioObjectPropertyName:
        case kAudioControlPropertyScope: case kAudioControlPropertyElement:
        case kAudioSelectorControlPropertyCurrentItem: case kAudioSelectorControlPropertyAvailableItems:
        case kAudioSelectorControlPropertyItemName: case kAudioSelectorControlPropertyItemKind:
            return true;
        }
        return false;
    }
    return false;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    if (!HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    *out = (obj == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
           (is_stream(obj) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) ||
           (obj == kObjClock && a->mSelector == kAudioSelectorControlPropertyCurrentItem) ||
           (obj == kObjLoop && a->mSelector == kAudioBooleanControlPropertyValue) ||
           (obj == kObjDevice && is_custom_settable(a->mSelector));
    return 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *out) {
    if (obj == kObjDevice && is_custom(a->mSelector)) { *out = sizeof(CFPropertyListRef); return 0; }   // never run getters with side effects
    UInt32 big = 4096; void *tmp = calloc(1, big);
    OSStatus r = GetPropertyData(d, obj, pid, a, qs, q, big, out, tmp);
    UInt32 sel = a->mSelector;
    bool isString = sel == kAudioObjectPropertyName || sel == kAudioObjectPropertyManufacturer ||
                    sel == kAudioDevicePropertyDeviceUID || sel == kAudioDevicePropertyModelUID ||
                    sel == kAudioObjectPropertyElementName || sel == kAudioSelectorControlPropertyItemName ||
                    sel == kStatusSelector || sel == kSelLevels || sel == kSelRoute || sel == kSelLowLatency ||
                    sel == kSelOptical || sel == kSelMidiIn || sel == kSelRestart || sel == kSelKeepAlive || sel == kSelMidiOut;
    if (!r && isString && *out == sizeof(CFStringRef) && *(CFStringRef *)tmp) CFRelease(*(CFStringRef *)tmp);
    free(tmp);
    return r;
}

#define PUT(T, v) do { if (inSize < sizeof(T)) return kAudioHardwareBadPropertySizeError; *(T *)out = (v); *outSize = sizeof(T); return 0; } while (0)
#define PUTSTR(cstr) PUT(CFStringRef, cfstr(cstr))

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 inSize, UInt32 *outSize, void *out) {
    UInt32 s = a->mSelector;
    bool present = atomic_load(&gPresent);
    switch (obj) {
    // ---------------- plug-in
    case kObjPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUTSTR("Digi 003 open driver");
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList:
            if (present && inSize >= sizeof(AudioObjectID)) { *(AudioObjectID *)out = kObjDevice; *outSize = sizeof(AudioObjectID); }
            else *outSize = 0;
            return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs == sizeof(CFStringRef) ? *(CFStringRef *)q : NULL;
            char want[64]; snprintf(want, sizeof want, kDeviceUIDPrefix "%s", gGUID);
            CFStringRef w = cfstr(want);
            AudioObjectID id = (present && uid && CFEqual(uid, w)) ? kObjDevice : kAudioObjectUnknown;
            CFRelease(w);
            PUT(AudioObjectID, id);
        }
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
        }
        break;

    // ---------------- device
    case kObjDevice: {
        AudioObjectPropertyScope sc = a->mScope;
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjPlugIn);
        case kAudioObjectPropertyName: PUTSTR(kDeviceName);
        case kAudioObjectPropertyManufacturer: PUTSTR(kManufacturer);
        case kAudioDevicePropertyDeviceUID: { char u[64]; snprintf(u, sizeof u, kDeviceUIDPrefix "%s", gGUID); PUTSTR(u); }
        case kAudioDevicePropertyModelUID: PUTSTR(kModelUID);
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeFireWire);
        case kAudioDevicePropertyRelatedDevices: PUT(AudioObjectID, kObjDevice);
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, present ? 1 : 0);
        case kAudioDevicePropertyDeviceIsRunning: { pthread_mutex_lock(&gLock); UInt32 r = gIOCount > 0; pthread_mutex_unlock(&gLock); PUT(UInt32, r); }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 1);
        case kAudioDevicePropertyLatency: PUT(UInt32, 24);               // converter latency estimate
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, sc == kAudioObjectPropertyScopeInput ? kInSafety : (gLowLatency ? 24u : 40u) * 6 + 144);
        case kAudioDevicePropertyClockIsStable: PUT(UInt32, 1);
        case kAudioObjectPropertyCustomPropertyInfoList: {
            UInt32 n = MIN((UInt32)kNumCustom, inSize / (UInt32)sizeof(AudioServerPlugInCustomPropertyInfo));
            AudioServerPlugInCustomPropertyInfo *ci = out;
            for (UInt32 i = 0; i < n; i++) {
                ci[i].mSelector = kCustomSels[i];
                ci[i].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                ci[i].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            }
            *outSize = n * sizeof *ci; return 0;
        }
        case kStatusSelector: PUT(CFPropertyListRef, copy_status());
        case kSelLevels: PUT(CFPropertyListRef, copy_levels());
        case kSelRoute: { int v = gRoute; PUT(CFPropertyListRef, CFNumberCreate(NULL, kCFNumberIntType, &v)); }
        case kSelLowLatency: { int v = gLowLatency; PUT(CFPropertyListRef, CFNumberCreate(NULL, kCFNumberIntType, &v)); }
        case kSelOptical: { int v = (int)gOptical; PUT(CFPropertyListRef, CFNumberCreate(NULL, kCFNumberIntType, &v)); }
        case kSelMidiIn: {
            uint8_t buf[1024]; size_t n = 0;
            pthread_mutex_lock(&gLock); if (gEngine) n = d003_midi_read(gEngine, buf, sizeof buf); pthread_mutex_unlock(&gLock);
            PUT(CFPropertyListRef, CFDataCreate(NULL, buf, (CFIndex)n));
        }
        case kSelRestart: case kSelKeepAlive: case kSelMidiOut: { int v = 0; PUT(CFPropertyListRef, CFNumberCreate(NULL, kCFNumberIntType, &v)); }
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, kZeroPeriod);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, gRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = 0; AudioValueRange *r = out;
            for (UInt32 i = 0; i < 2 && (n + 1) * sizeof(AudioValueRange) <= inSize; i++, n++) { r[i].mMinimum = r[i].mMaximum = kRates[i]; }
            *outSize = n * sizeof(AudioValueRange); return 0;
        }
        case kAudioObjectPropertyOwnedObjects: case kAudioDevicePropertyStreams: case kAudioObjectPropertyControlList: {
            AudioObjectID ids[4]; UInt32 n = 0;
            bool wantStreams = s != kAudioObjectPropertyControlList, wantCtl = s != kAudioDevicePropertyStreams;
            if (wantStreams && (sc == kAudioObjectPropertyScopeGlobal || sc == kAudioObjectPropertyScopeInput))  ids[n++] = kObjInStream;
            if (wantStreams && (sc == kAudioObjectPropertyScopeGlobal || sc == kAudioObjectPropertyScopeOutput)) ids[n++] = kObjOutStream;
            if (wantCtl && sc == kAudioObjectPropertyScopeGlobal) { ids[n++] = kObjClock; ids[n++] = kObjLoop; }
            if (s == kAudioObjectPropertyControlList) { n = 0; ids[n++] = kObjClock; ids[n++] = kObjLoop; }
            n = MIN(n, inSize / sizeof(AudioObjectID));
            memcpy(out, ids, n * sizeof(AudioObjectID)); *outSize = n * sizeof(AudioObjectID); return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inSize < 8) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)out)[0] = 1; ((UInt32 *)out)[1] = 2; *outSize = 8; return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 need = offsetof(AudioChannelLayout, mChannelDescriptions) + kNumCh * sizeof(AudioChannelDescription);
            if (inSize < need) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = out; memset(l, 0, need);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = kNumCh;
            for (UInt32 i = 0; i < kNumCh; i++)
                l->mChannelDescriptions[i].mChannelLabel = i == 0 ? kAudioChannelLabel_Left : i == 1 ? kAudioChannelLabel_Right : kAudioChannelLabel_Unknown;
            *outSize = need; return 0;
        }
        case kAudioObjectPropertyElementName: { char n[32]; channel_name(a->mElement, n, sizeof n); PUTSTR(n); }
        }
        break;
    }

    // ---------------- streams
    case kObjInStream: case kObjOutStream: {
        bool in = obj == kObjInStream;
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioObjectPropertyName: PUTSTR(in ? "003 In" : "003 Out");
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
        case kAudioStreamPropertyDirection: PUT(UInt32, in ? 1 : 0);
        case kAudioStreamPropertyTerminalType: PUT(UInt32, in ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency: PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: {
            if (inSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            fill_asbd(out, gRate); *outSize = sizeof(AudioStreamBasicDescription); return 0;
        }
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = 0; AudioStreamRangedDescription *r = out;
            for (UInt32 i = 0; i < 2 && (n + 1) * sizeof(*r) <= inSize; i++, n++) {
                fill_asbd(&r[i].mFormat, kRates[i]); r[i].mSampleRateRange.mMinimum = r[i].mSampleRateRange.mMaximum = kRates[i];
            }
            *outSize = n * sizeof(*r); return 0;
        }
        }
        break;
    }

    // ---------------- preamp mode switch
    case kObjLoop:
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioBooleanControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioObjectPropertyName: PUTSTR("Preamps -> ADAT Out");
        case kAudioControlPropertyScope: PUT(AudioObjectPropertyScope, kAudioObjectPropertyScopeGlobal);
        case kAudioControlPropertyElement: PUT(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue: PUT(UInt32, gLoop ? 1 : 0);
        }
        break;

    // ---------------- clock source selector
    case kObjClock:
        switch (s) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioSelectorControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioClockSourceControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: *outSize = 0; return 0;
        case kAudioObjectPropertyName: PUTSTR("Clock Source");
        case kAudioControlPropertyScope: PUT(AudioObjectPropertyScope, kAudioObjectPropertyScopeGlobal);
        case kAudioControlPropertyElement: PUT(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioSelectorControlPropertyCurrentItem: PUT(UInt32, gClock);
        case kAudioSelectorControlPropertyAvailableItems: {
            UInt32 n = MIN(4u, inSize / 4);
            for (UInt32 i = 0; i < n; i++) ((UInt32 *)out)[i] = i;
            *outSize = n * 4; return 0;
        }
        case kAudioSelectorControlPropertyItemName: {
            if (qs != 4) return kAudioHardwareBadPropertySizeError;
            UInt32 item = *(const UInt32 *)q; if (item > 3) return kAudioHardwareIllegalOperationError;
            PUTSTR(kClockNames[item]);
        }
        case kAudioSelectorControlPropertyItemKind: {
            UInt32 item = qs == 4 ? *(const UInt32 *)q : 0;
            PUT(UInt32, item == 0 ? kAudioClockSourceItemKindInternal : 0);
        }
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qs, const void *q, UInt32 inSize, const void *in) {
    Float64 want = 0;
    if (obj == kObjDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        want = *(const Float64 *)in;
    } else if (is_stream(obj) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = in;
        if (f->mChannelsPerFrame != kNumCh || f->mFormatID != kAudioFormatLinearPCM) return kAudioDeviceUnsupportedFormatError;
        want = f->mSampleRate;
    } else if (obj == kObjDevice && is_custom_settable(a->mSelector)) {
        if (inSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef pl = *(const CFPropertyListRef *)in;
        int num = 0;
        if (pl && CFGetTypeID(pl) == CFNumberGetTypeID()) CFNumberGetValue(pl, kCFNumberIntType, &num);
        bool restart = false, changedLoop = false;
        pthread_mutex_lock(&gLock);
        switch (a->mSelector) {
        case kSelRoute:
            num = num == 4 ? 4 : num ? 8 : 0;
            if (num != gRoute) { gRoute = num; if (num) gLastRoute = num; changedLoop = true; save_prefs(); engine_update_locked(); LOG("ADAT route %d", num); }
            break;
        case kSelLowLatency:
            if ((num != 0) != gLowLatency) { gLowLatency = num != 0; save_prefs(); restart = gEngine != NULL; }   // brief dropout for clients, applies now
            break;
        case kSelOptical:
            if ((UInt32)(num != 0) != gOptical) { gOptical = num != 0; save_prefs(); restart = gEngine != NULL; }
            break;
        case kSelRestart: if (gEngine) restart = true; else engine_update_locked(); break;
        case kSelKeepAlive: {
            mach_timebase_info_data_t tb; mach_timebase_info(&tb);
            uint64_t secs = num < 0 ? 0 : num > 60 ? 60 : num;
            gKeepAliveUntil = secs ? mach_absolute_time() + secs * 1000000000ull * tb.denom / tb.numer : 0;
            engine_update_locked();
            break;
        }
        case kSelMidiOut:
            if (pl && CFGetTypeID(pl) == CFDataGetTypeID()) {
                if (!gEngine) { gKeepAliveUntil = mach_absolute_time() + 10ull * NSEC_PER_SEC; engine_update_locked(); }
                if (gEngine) d003_midi_write(gEngine, CFDataGetBytePtr(pl), (size_t)CFDataGetLength(pl));
            }
            break;
        }
        if (restart) { LOG("restart requested"); engine_restart_locked(); }
        pthread_mutex_unlock(&gLock);
        flush_notifications();
        if (changedLoop) notify(kObjLoop, kAudioBooleanControlPropertyValue);
        if (a->mSelector == kSelLowLatency) notify(kObjDevice, kAudioDevicePropertySafetyOffset);
        return 0;
    } else if (obj == kObjLoop && a->mSelector == kAudioBooleanControlPropertyValue) {
        if (inSize < 4) return kAudioHardwareBadPropertySizeError;
        bool on = *(const UInt32 *)in != 0;
        pthread_mutex_lock(&gLock);
        if (on != gLoop) { gRoute = on ? gLastRoute : 0; save_prefs(); engine_update_locked(); LOG("preamps -> ADAT %d", gRoute); }
        pthread_mutex_unlock(&gLock);
        flush_notifications();
        notify(kObjLoop, kAudioBooleanControlPropertyValue);
        return 0;
    } else if (obj == kObjClock && a->mSelector == kAudioSelectorControlPropertyCurrentItem) {
        if (inSize < 4) return kAudioHardwareBadPropertySizeError;
        UInt32 c = *(const UInt32 *)in; if (c > 3) return kAudioHardwareIllegalOperationError;
        pthread_mutex_lock(&gLock);
        if (c != gClock) {
            gClock = c; save_prefs();
            if (gEngine) { engine_restart_locked(); }
        }
        pthread_mutex_unlock(&gLock);
        flush_notifications();
        notify(kObjClock, kAudioSelectorControlPropertyCurrentItem);
        return 0;
    } else return kAudioHardwareUnknownPropertyError;

    if (want != 44100.0 && want != 48000.0) return kAudioDeviceUnsupportedFormatError;
    if (want == gRate) return 0;
    gPendingRate = want;
    return gHost->RequestDeviceConfigurationChange(gHost, kObjDevice, 1, NULL);
}

static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info) {
    pthread_mutex_lock(&gLock);
    if (gPendingRate == 44100.0 || gPendingRate == 48000.0) {
        gRate = gPendingRate; save_prefs();
        if (gEngine) { engine_restart_locked(); }
    }
    pthread_mutex_unlock(&gLock);
    return 0;
}

// ------------------------------------------------------------------ IO
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    OSStatus r = 0;
    pthread_mutex_lock(&gLock);
    if (!gEngine) r = engine_start_locked();
    if (!r) gIOCount++;
    pthread_mutex_unlock(&gLock);
    flush_notifications();
    if (!r && gIOCount == 1) notify(kObjDevice, kAudioDevicePropertyDeviceIsRunning);
    return r;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client) {
    pthread_mutex_lock(&gLock);
    bool stopped = false;
    if (gIOCount > 0 && --gIOCount == 0) { engine_update_locked(); stopped = true; }
    pthread_mutex_unlock(&gLock);
    if (stopped) notify(kObjDevice, kAudioDevicePropertyDeviceIsRunning);
    return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed) {
    D003Engine *e = gEngine;
    if (!e) return kAudioHardwareNotRunningError;
    int64_t f = d003_rx_frames(e);
    int64_t z = (f / kZeroPeriod) * kZeroPeriod;
    uint64_t h; double tpf;
    if (!d003_host_time_for(e, (double)z, &h, &tpf)) return kAudioHardwareNotRunningError;
    *st = (Float64)z; *ht = h; *seed = gSeed;
    return 0;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    *will = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = true;
    return 0;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client, UInt32 op,
                              UInt32 n, const AudioServerPlugInIOCycleInfo *ci, void *mainBuf, void *secondary) {
    D003Engine *e = gEngine;
    if (!e) { if (op == kAudioServerPlugInIOOperationReadInput) memset(mainBuf, 0, n * kNumCh * 4); return 0; }
    const size_t frameBytes = sizeof(float) * kNumCh;
    if (op == kAudioServerPlugInIOOperationReadInput) {
        int64_t t = (int64_t)ci->mInputTime.mSampleTime;
        float *in = d003_in_ring(e), *dst = mainBuf;
        for (UInt32 i = 0; i < n; i++, t++)
            memcpy(dst + (size_t)i * kNumCh, in + (size_t)(t & (D003_RING_FRAMES - 1)) * D003_MAX_CH, frameBytes);
    } else if (op == kAudioServerPlugInIOOperationWriteMix) {
        int64_t t = (int64_t)ci->mOutputTime.mSampleTime;
        float *outRing = d003_out_ring(e); const float *src = mainBuf;
        int route = gOptical ? 0 : gRoute;
        if (!route) {
            for (UInt32 i = 0; i < n; i++, t++)
                memcpy(outRing + (size_t)(t & (D003_RING_FRAMES - 1)) * D003_MAX_CH, src + (size_t)i * kNumCh, frameBytes);
        } else {   // ADAT outs 1-8 belong to the preamp loopback
            for (UInt32 i = 0; i < n; i++, t++) {
                float *o = outRing + (size_t)(t & (D003_RING_FRAMES - 1)) * D003_MAX_CH; const float *x = src + (size_t)i * kNumCh;
                memcpy(o, x, D003_CH_ADAT * sizeof(float));   // analog 1-8 + S/PDIF
                for (int c = D003_CH_ADAT + route; c < kNumCh; c++) o[c] = x[c];   // ADAT channels not taken by the preamps
            }
        }
    }
    return 0;
}
