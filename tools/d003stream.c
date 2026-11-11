// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// d003stream — first duplex isochronous test for Digidesign 003 Rack (IOFireWireLib + NuDCL).
// Sends silence to the 003 (required: the device only transmits after it receives packets),
// receives its 18 capture channels and prints peak meters for analog inputs 1-8.
//
// Protocol per Linux sound/firewire/digi00x (Sakamoto) + amdtp-dot (Gareus/Zammit).
// Build: clang -O2 -Wall -o d003stream d003stream.c -framework IOKit -framework CoreFoundation
// Usage: ./d003stream [seconds]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>

#define VENDOR_DIGIDESIGN 0x00a07e
#define BASE_HI 0xffff
#define BASE_LO 0xe0000000u
#define R_STREAMING_STATE 0x0000
#define R_STREAMING_SET   0x0004
#define R_ISOC_CHANNELS   0x0100
#define R_LOCAL_RATE      0x0110
#define R_CLOCK_SOURCE    0x0118

#define PCM_CH      18                  // 8 analog + 8 ADAT + 2 S/PDIF at 44.1/48k
#define DBS         (PCM_CH + 1)        // + MIDI conformant quadlet
#define BLOCKS      6                   // 48 kHz non-blocking: 6 data blocks every cycle
#define TX_BYTES    (8 + BLOCKS * DBS * 4)
#define RX_BYTES    1024
#define NPKT        256                 // ring length (32 ms); 256*6 % 256 == 0 keeps DBC continuous
#define CB_EVERY    32

static IOFireWireLibDeviceRef fw;
static io_object_t svc;
static UInt32 gen;

static IOReturn wr(UInt32 off, UInt32 v) {
    FWAddress a = { 0, BASE_HI, BASE_LO + off };
    return (*fw)->WriteQuadlet(fw, svc, &a, CFSwapInt32HostToBig(v), false, gen);
}
static IOReturn rd(UInt32 off, UInt32 *v) {
    FWAddress a = { 0, BASE_HI, BASE_LO + off };
    IOReturn r = (*fw)->ReadQuadlet(fw, svc, &a, v, false, gen);
    *v = CFSwapInt32BigToHost(*v);
    return r;
}

static void finish_session(void) {
    wr(R_STREAMING_SET, 3);
    wr(R_ISOC_CHANNELS, 0);
    usleep(50000);
}

// --- remote port (the 003 side of each channel) ---
static UInt32 chanTx = 0xffffffff;  // device -> host
static UInt32 chanRx = 0xffffffff;  // host -> device
static IOReturn rGetSupported(IOFireWireLibIsochPortRef p, IOFWSpeed *s, UInt64 *ch) {
    *s = kFWSpeed400MBit; *ch = ~0ULL; return kIOReturnSuccess;
}
static IOReturn rAllocate(IOFireWireLibIsochPortRef p, IOFWSpeed s, UInt32 chan) {
    UInt32 *dst = (UInt32 *)(*p)->GetRefCon(p);
    *dst = chan;
    printf("  canal isocrono %u asignado (%s)\n", chan, dst == &chanTx ? "003 -> Mac" : "Mac -> 003");
    return kIOReturnSuccess;
}
static IOReturn rNop(IOFireWireLibIsochPortRef p) { return kIOReturnSuccess; }

// --- receive metering ---
static UInt8 *rxBuf;
static volatile unsigned rxIndex;
static unsigned long pkts, emptyPkts, badPkts;
static int32_t peak[PCM_CH];
static UInt32 lastCIP0, lastCIP1, lastHdr;

static void parsePacket(const UInt8 *b) {
    UInt32 hdr = *(const UInt32 *)b;
    lastHdr = hdr;
    UInt32 len = hdr >> 16;                       // OHCI stores the iso header host-endian
    if (len > RX_BYTES - 4) len = CFSwapInt32(hdr) >> 16;
    const UInt32 *q = (const UInt32 *)(b + 4);
    if (len < 8) { badPkts++; return; }
    UInt32 c0 = CFSwapInt32BigToHost(q[0]), c1 = CFSwapInt32BigToHost(q[1]);
    lastCIP0 = c0; lastCIP1 = c1;
    unsigned dbs = (c0 >> 16) & 0xff;
    if (len == 8) { emptyPkts++; pkts++; return; }
    if (dbs != DBS) { badPkts++; return; }
    unsigned nblk = (len - 8) / (dbs * 4);
    const UInt32 *d = q + 2;
    for (unsigned i = 0; i < nblk; i++, d += dbs)
        for (int c = 0; c < PCM_CH; c++) {
            int32_t s = (int32_t)(CFSwapInt32BigToHost(d[1 + c]) << 8) >> 8;
            if (s < 0) s = -s;
            if (s > peak[c]) peak[c] = s;
        }
    pkts++;
}

static void rxCallback(void *refcon, NuDCLRef dcl) {
    unsigned end = (unsigned)(uintptr_t)refcon;   // index of the DCL that fired
    unsigned start = (end + NPKT - (CB_EVERY - 1)) % NPKT;
    for (unsigned k = 0; k < CB_EVERY; k++) parsePacket(rxBuf + ((start + k) % NPKT) * RX_BYTES);
}

static const char *db(int32_t p) {
    static char s[8][16]; static int i; i = (i + 1) & 7;
    if (p <= 0) snprintf(s[i], 16, "  -inf");
    else snprintf(s[i], 16, "%6.1f", 20 * log10((double)p / 8388608.0));
    return s[i];
}

static void meterTimer(CFRunLoopTimerRef t, void *info) {
    printf("pkts %6lu vacios %5lu malos %4lu | CIP %08x %08x | ", pkts, emptyPkts, badPkts, lastCIP0, lastCIP1);
    for (int c = 0; c < 8; c++) printf("%s ", db(peak[c]));
    printf("dBFS\n");
    memset(peak, 0, sizeof peak);
}

static void *pageAlloc(size_t n) {
    vm_address_t a = 0;
    if (vm_allocate(mach_task_self(), &a, n, VM_FLAGS_ANYWHERE)) return NULL;
    memset((void *)a, 0, n);
    return (void *)a;
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;

    io_iterator_t it; io_object_t d, dev = 0;
    IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOFireWireDevice"), &it);
    while ((d = IOIteratorNext(it))) {
        CFTypeRef r = IORegistryEntryCreateCFProperty(d, CFSTR("Vendor_ID"), kCFAllocatorDefault, 0);
        long v = 0; if (r) { CFNumberGetValue(r, kCFNumberLongType, &v); CFRelease(r); }
        if (v == VENDOR_DIGIDESIGN && !dev) dev = d; else IOObjectRelease(d);
    }
    IOObjectRelease(it);
    if (!dev) { fprintf(stderr, "No encuentro el 003 en el bus FireWire.\n"); return 1; }

    IOCFPlugInInterface **plug; SInt32 score;
    if (IOCreatePlugInInterfaceForService(dev, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score)) return 1;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID *)&fw);
    (*plug)->Release(plug);
    if (!fw || (*fw)->Open(fw)) { fprintf(stderr, "No pude abrir el 003.\n"); return 1; }
    svc = (*fw)->GetDevice(fw);
    (*fw)->GetBusGeneration(fw, &gen);
    (*fw)->AddCallbackDispatcherToRunLoop(fw, CFRunLoopGetCurrent());
    (*fw)->AddIsochCallbackDispatcherToRunLoop(fw, CFRunLoopGetCurrent());
    UInt16 localNode = 0; (*fw)->GetLocalNodeID(fw, &localNode);

    finish_session();
    wr(R_LOCAL_RATE, 1);        // 48 kHz
    wr(R_CLOCK_SOURCE, 0);      // internal
    UInt32 v; rd(R_LOCAL_RATE, &v); printf("LOCAL_RATE=%u  nodo local 0x%04x\n", v, localNode);

    // ---- TX ring: silence, AM824 / CIP, DBC continuous across the loop ----
    UInt8 *txBuf = pageAlloc(NPKT * TX_BYTES);
    for (unsigned i = 0; i < NPKT; i++) {
        UInt32 *q = (UInt32 *)(txBuf + i * TX_BYTES);
        q[0] = CFSwapInt32HostToBig(((localNode & 0x3f) << 24) | (DBS << 16) | ((i * BLOCKS) & 0xff));
        q[1] = CFSwapInt32HostToBig(0x80000000u | (0x10u << 24) | (0x02u << 16) | 0xffff); // FMT AM824, SFC 48k, SYT no-info
        UInt32 *blk = q + 2;
        for (int b = 0; b < BLOCKS; b++, blk += DBS) {
            blk[0] = CFSwapInt32HostToBig(0x80000000u);                       // MIDI slot, empty
            for (int c = 0; c < PCM_CH; c++) blk[1 + c] = CFSwapInt32HostToBig(0x40000000u); // MBLA silence
        }
    }
    IOFireWireLibNuDCLPoolRef txPool = (*fw)->CreateNuDCLPool(fw, NPKT, CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    (*txPool)->SetCurrentTagAndSync(txPool, 1, 0);
    NuDCLRef first = NULL, prev = NULL;
    for (unsigned i = 0; i < NPKT; i++) {
        IOVirtualRange rg = { (IOVirtualAddress)(txBuf + i * TX_BYTES), TX_BYTES };
        NuDCLRef dcl = (*txPool)->AllocateSendPacket(txPool, NULL, 1, &rg);
        if (!dcl) { fprintf(stderr, "AllocateSendPacket fallo\n"); return 1; }
        if (!first) first = dcl;
        prev = dcl;
    }
    (*txPool)->SetDCLBranch(prev, first);
    IOVirtualRange txAll = { (IOVirtualAddress)txBuf, NPKT * TX_BYTES };
    IOFireWireLibLocalIsochPortRef txPort = (*fw)->CreateLocalIsochPort(fw, true, (*txPool)->GetProgram(txPool),
        0, 0, 0, NULL, 0, &txAll, 1, CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!txPort) { fprintf(stderr, "CreateLocalIsochPort(tx) fallo\n"); return 1; }

    // ---- RX ring ----
    rxBuf = pageAlloc(NPKT * RX_BYTES);
    IOFireWireLibNuDCLPoolRef rxPool = (*fw)->CreateNuDCLPool(fw, NPKT, CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    first = prev = NULL;
    for (unsigned i = 0; i < NPKT; i++) {
        IOVirtualRange rg = { (IOVirtualAddress)(rxBuf + i * RX_BYTES), RX_BYTES };
        NuDCLRef dcl = (*rxPool)->AllocateReceivePacket(rxPool, NULL, 4, 1, &rg);
        if (!dcl) { fprintf(stderr, "AllocateReceivePacket fallo\n"); return 1; }
        if ((i % CB_EVERY) == CB_EVERY - 1) {
            (*rxPool)->SetDCLCallback(dcl, rxCallback);
            (*rxPool)->SetDCLRefcon(dcl, (void *)(uintptr_t)i);
            (*rxPool)->SetDCLFlags(dcl, kNuDCLUpdateBeforeCallback);
        }
        if (!first) first = dcl;
        prev = dcl;
    }
    (*rxPool)->SetDCLBranch(prev, first);
    IOVirtualRange rxAll = { (IOVirtualAddress)rxBuf, NPKT * RX_BYTES };
    IOFireWireLibLocalIsochPortRef rxPort = (*fw)->CreateLocalIsochPort(fw, false, (*rxPool)->GetProgram(rxPool),
        0, 0, 0, NULL, 0, &rxAll, 1, CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!rxPort) { fprintf(stderr, "CreateLocalIsochPort(rx) fallo\n"); return 1; }

    // ---- remote ports + channels ----
    IOFireWireLibRemoteIsochPortRef devListen = (*fw)->CreateRemoteIsochPort(fw, false, CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    IOFireWireLibRemoteIsochPortRef devTalk   = (*fw)->CreateRemoteIsochPort(fw, true,  CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    IOFireWireLibRemoteIsochPortRef rp[2] = { devListen, devTalk };
    for (int i = 0; i < 2; i++) {
        (*rp[i])->SetGetSupportedHandler(rp[i], rGetSupported);
        (*rp[i])->SetAllocatePortHandler(rp[i], rAllocate);
        (*rp[i])->SetReleasePortHandler(rp[i], rNop);
        (*rp[i])->SetStartHandler(rp[i], rNop);
        (*rp[i])->SetStopHandler(rp[i], rNop);
    }
    (*devListen)->SetRefCon((IOFireWireLibIsochPortRef)devListen, &chanRx);
    (*devTalk)->SetRefCon((IOFireWireLibIsochPortRef)devTalk, &chanTx);

    IOFireWireLibIsochChannelRef chOut = (*fw)->CreateIsochChannel(fw, true, TX_BYTES, kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    IOFireWireLibIsochChannelRef chIn  = (*fw)->CreateIsochChannel(fw, true, TX_BYTES, kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    (*chOut)->SetTalker(chOut, (IOFireWireLibIsochPortRef)txPort);
    (*chOut)->AddListener(chOut, (IOFireWireLibIsochPortRef)devListen);
    (*chIn)->SetTalker(chIn, (IOFireWireLibIsochPortRef)devTalk);
    (*chIn)->AddListener(chIn, (IOFireWireLibIsochPortRef)rxPort);

    IOReturn e;
    if ((e = (*chOut)->AllocateChannel(chOut))) { fprintf(stderr, "AllocateChannel(out) 0x%x\n", e); return 1; }
    if ((e = (*chIn)->AllocateChannel(chIn)))   { fprintf(stderr, "AllocateChannel(in) 0x%x\n", e); return 1; }

    // begin_session: register channels, step streaming state down to 1
    wr(R_ISOC_CHANNELS, (chanTx << 16) | chanRx);
    UInt32 curr = 0; rd(R_STREAMING_STATE, &curr);
    if (curr == 0) curr = 2;
    for (curr--; curr > 0; curr--) { wr(R_STREAMING_SET, curr); usleep(20000); }

    if ((e = (*chOut)->Start(chOut))) { fprintf(stderr, "Start(out) 0x%x\n", e); goto out; }
    if ((e = (*chIn)->Start(chIn)))   { fprintf(stderr, "Start(in) 0x%x\n", e); goto out; }
    rd(R_STREAMING_STATE, &curr);
    printf("Streaming iniciado (STREAMING_STATE=%u). Toca algo en las entradas 1-8...\n", curr);
    printf("                                                     in1    in2    in3    in4    in5    in6    in7    in8\n");

    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1, 0, 0, meterTimer, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    printf("ultimo header iso 0x%08x\n", lastHdr);

out:
    (*chIn)->Stop(chIn);
    (*chOut)->Stop(chOut);
    finish_session();
    (*chIn)->ReleaseChannel(chIn);
    (*chOut)->ReleaseChannel(chOut);
    (*fw)->Close(fw);
    return 0;
}
