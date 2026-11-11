// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// d003probe — read-only probe of a Digidesign 002/003 over FireWire (IOFireWireLib).
// Lists FireWire devices and, for Digidesign units (vendor 0x00a07e), dumps the
// control registers documented by the Linux snd-firewire-digi00x driver.
//
// Build: clang -O2 -Wall -o d003probe d003probe.c -framework IOKit -framework CoreFoundation

#include <stdio.h>
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>

#define VENDOR_DIGIDESIGN 0x00a07e
#define DG00X_ADDR_HI 0xffff
#define DG00X_ADDR_LO 0xe0000000u

static const struct { uint32_t off; const char *name; } regs[] = {
    {0x0000, "STREAMING_STATE"},
    {0x0004, "STREAMING_SET"},
    {0x0014, "MESSAGE_ADDR_HI"},
    {0x0018, "MESSAGE_ADDR_LO"},
    {0x0100, "ISOC_CHANNELS"},
    {0x0110, "LOCAL_RATE (0=44.1 1=48 2=88.2 3=96)"},
    {0x0114, "EXTERNAL_RATE"},
    {0x0118, "CLOCK_SOURCE (0=int 1=spdif 2=adat 3=word)"},
    {0x011c, "OPT_IFACE_MODE (0=adat 1=spdif)"},
    {0x0124, "MIXER_ON_OFF?"},
    {0x012c, "DETECT_EXTERNAL"},
};

static long numProp(io_object_t o, CFStringRef key) {
    long v = -1;
    CFTypeRef r = IORegistryEntryCreateCFProperty(o, key, kCFAllocatorDefault, 0);
    if (r) { if (CFGetTypeID(r) == CFNumberGetTypeID()) CFNumberGetValue(r, kCFNumberLongType, &v); CFRelease(r); }
    return v;
}

static void strProp(io_object_t o, CFStringRef key, char *buf, size_t n) {
    buf[0] = 0;
    CFTypeRef r = IORegistryEntryCreateCFProperty(o, key, kCFAllocatorDefault, 0);
    if (r) { if (CFGetTypeID(r) == CFStringGetTypeID()) CFStringGetCString(r, buf, n, kCFStringEncodingUTF8); CFRelease(r); }
}

static void probeDigi(io_object_t dev) {
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(dev, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (kr || !plug) { printf("   ! IOCreatePlugInInterfaceForService failed 0x%x\n", kr); return; }
    IOFireWireLibDeviceRef fw = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), (LPVOID *)&fw);
    (*plug)->Release(plug);
    if (!fw) { printf("   ! QueryInterface failed\n"); return; }
    kr = (*fw)->Open(fw);
    if (kr) { printf("   ! Open failed 0x%x (otro driver lo tiene abierto?)\n", kr); (*fw)->Release(fw); return; }

    io_object_t svc = (*fw)->GetDevice(fw);
    UInt32 gen = 0; (*fw)->GetBusGeneration(fw, &gen);
    printf("   bus generation %u\n", gen);
    for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        FWAddress a = { 0, DG00X_ADDR_HI, DG00X_ADDR_LO + regs[i].off };  // {nodeID, addressHi, addressLo}
        UInt32 v = 0;
        kr = (*fw)->ReadQuadlet(fw, svc, &a, &v, false, gen);
        if (kr) printf("   0x%04x %-44s  error 0x%x\n", regs[i].off, regs[i].name, kr);
        else    printf("   0x%04x %-44s  0x%08x\n", regs[i].off, regs[i].name, CFSwapInt32BigToHost(v));
    }
    (*fw)->Close(fw);
    (*fw)->Release(fw);
}

int main(void) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOFireWireDevice"), &it)) {
        fprintf(stderr, "IOServiceGetMatchingServices failed\n"); return 1;
    }
    int n = 0, digi = 0; io_object_t d;
    while ((d = IOIteratorNext(it))) {
        char vname[64], mname[64];
        long vid = numProp(d, CFSTR("Vendor_ID")), mid = numProp(d, CFSTR("Model_ID"));
        strProp(d, CFSTR("FireWire Vendor Name"), vname, sizeof vname);
        strProp(d, CFSTR("FireWire Product Name"), mname, sizeof mname);
        long guid = numProp(d, CFSTR("GUID"));
        printf("[%d] vendor 0x%06lx model 0x%06lx  %s / %s  GUID %016lx\n", n++, vid, mid, vname, mname, guid);
        if (vid == VENDOR_DIGIDESIGN) { digi++; probeDigi(d); }
        IOObjectRelease(d);
    }
    IOObjectRelease(it);
    if (n == 0) printf("No hay dispositivos FireWire. Revisa adaptadores, cable y que el 003 este encendido.\n");
    else if (!digi) printf("Ningun dispositivo Digidesign (vendor 0x00a07e) encontrado.\n");
    return 0;
}
