// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// midimon — prints MIDI from "Digi 003 MIDI In" for N seconds; optionally plays a test arpeggio on "Digi 003 MIDI Out".
// Usage: ./midimon [seconds] [play]
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static MIDIEndpointRef byName(int src, const char *want) {
    ItemCount n = src ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef e = src ? MIDIGetSource(i) : MIDIGetDestination(i); CFStringRef s = NULL; char b[128] = "";
        MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s); if (s) { CFStringGetCString(s, b, 128, kCFStringEncodingUTF8); CFRelease(s); }
        if (!strcmp(b, want)) return e;
    }
    return 0;
}
static void readProc(const MIDIPacketList *pl, void *r, void *s) {
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; i++, p = MIDIPacketNext(p)) {
        if (p->length == 1 && p->data[0] >= 0xF8) continue;           // hide clock/active sensing
        printf("IN:"); for (int k = 0; k < p->length; k++) printf(" %02X", p->data[k]); printf("\n"); fflush(stdout);
    }
}
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    MIDIClientRef c; MIDIClientCreate(CFSTR("midimon"), NULL, NULL, &c);
    MIDIPortRef in, out; MIDIInputPortCreate(c, CFSTR("in"), readProc, NULL, &in); MIDIOutputPortCreate(c, CFSTR("out"), &out);
    MIDIEndpointRef src = byName(1, "Digi 003 MIDI In"), dst = byName(0, "Digi 003 MIDI Out");
    printf("fuente %s, destino %s\n", src ? "ok" : "NO", dst ? "ok" : "NO"); fflush(stdout);
    if (src) MIDIPortConnectSource(in, src, NULL);
    if (argc > 2 && dst) {
        int notes[] = { 60, 64, 67, 72, 67, 64, 60 };
        for (int i = 0; i < 7; i++) {
            Byte buf[64]; MIDIPacketList *pl = (MIDIPacketList *)buf; MIDIPacket *p = MIDIPacketListInit(pl);
            Byte on[3] = { 0x90, notes[i], 90 }; p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, on); MIDISend(out, dst, pl);
            usleep(250000);
            p = MIDIPacketListInit(pl); Byte off[3] = { 0x80, notes[i], 0 }; MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, off); MIDISend(out, dst, pl);
            usleep(50000);
        }
        printf("OUT: arpegio de prueba enviado (Do-Mi-Sol-Do, canal 1)\n"); fflush(stdout);
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs, false);
    printf("fin del monitor\n");
}
