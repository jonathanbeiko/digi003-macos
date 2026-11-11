// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// Protocol and the "double-oh-three" scrambling are derived from the Linux driver sound/firewire/digi00x:
//   Copyright (c) 2014-2015 Takashi Sakamoto
//   Copyright (C) 2012 Robin Gareus, Damien Zammit (amdtp-dot)
// d003engine — userspace FireWire streaming engine for Digidesign 003 / 003 Rack / 002 (Apple Silicon, IOFireWireLib).
// Protocol knowledge from Linux sound/firewire/digi00x (T. Sakamoto) and amdtp-dot (R. Gareus, D. Zammit).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define D003_MAX_CH       18          // 44.1/48k stream order: analog 1-8, S/PDIF L/R, ADAT 1-8
#define D003_CH_SPDIF     8
#define D003_CH_ADAT      10
#define D003_RING_FRAMES  16384       // power of two

typedef enum { D003_CLOCK_INTERNAL = 0, D003_CLOCK_SPDIF = 1, D003_CLOCK_ADAT = 2, D003_CLOCK_WORD = 3 } D003Clock;

typedef struct {
    uint64_t rxPackets, rxEmpty, rxBad, rxDbcErrors, rxOverruns;
    uint64_t txPackets, txLate;
    double   ppm;                     // measured device clock vs host clock
    double   jitterUs;                // callback latency spread
} D003Stats;

typedef struct D003Engine D003Engine;

// Opens the first Digidesign 00x on the bus. Returns NULL (and fills err) if none.
D003Engine *d003_open(char *err, size_t errlen);
void        d003_close(D003Engine *e);

int   d003_start(D003Engine *e, unsigned rate, D003Clock clock);   // 0 on success
void  d003_stop(D003Engine *e);
bool  d003_running(D003Engine *e);
bool  d003_realtime(D003Engine *e);              // engine thread got time-constraint scheduling
unsigned d003_rate(D003Engine *e);
unsigned d003_channels(D003Engine *e);          // PCM channels for the current rate

// Frame counters on a single timeline (sample time). rx = frames captured, tx = frames consumed for output.
int64_t d003_rx_frames(D003Engine *e);
int64_t d003_tx_frames(D003Engine *e);

// Interleaved rings, [D003_RING_FRAMES][D003_MAX_CH], indexed by sample time % D003_RING_FRAMES.
float *d003_in_ring(D003Engine *e);
float *d003_out_ring(D003Engine *e);            // engine zeroes each frame after sending it

// Host time (mach_absolute_time units) at which frame `sampleTime` was captured, by the clock model.
bool  d003_host_time_for(D003Engine *e, double sampleTime, uint64_t *hostTime, double *ticksPerFrame);

void  d003_get_stats(D003Engine *e, D003Stats *s);
unsigned d003_output_lead_frames(D003Engine *e); // how far ahead of rx the tx side reads the out ring

// Copies analog inputs 1-8 to ADAT outputs 1-8 inside the engine (preamp mode). Can be toggled while running.
void  d003_set_adat_loopback(D003Engine *e, bool on);
int64_t d003_loopback_latency_frames(D003Engine *e);   // -1 until the stream has settled
// ADAT routing: 0 = DAW owns ADAT outs, 4 = analog 1-4 -> ADAT 1-4 (DAW keeps 5-8), 8 = analog 1-8 -> ADAT 1-8.
void  d003_set_adat_route(D003Engine *e, int analogChannels);
// Applied at the next d003_start: smaller tx ring (~3 ms instead of 5 ms), optical port mode (0 ADAT, 1 S/PDIF).
void  d003_set_low_latency(D003Engine *e, bool on);
void  d003_set_optical_mode(D003Engine *e, unsigned mode);
// Peak |sample| per channel since the previous call (inputs as captured, outputs as sent).
void  d003_take_peaks(D003Engine *e, float in[D003_MAX_CH], float out[D003_MAX_CH]);
// MIDI IN/OUT of the 003 (carried inside the isochronous stream). Callback runs on the engine thread.
size_t d003_midi_read(D003Engine *e, uint8_t *buf, size_t max);
size_t d003_midi_write(D003Engine *e, const uint8_t *buf, size_t n);
void  d003_set_midi_in_callback(D003Engine *e, void (*cb)(void *), void *ctx);

// Diagnostics: 0 = mirror the device's rx cadence on tx (default), 1 = nominal cadence only.
void  d003_set_cadence_mode(D003Engine *e, int mode);
void  d003_debug(D003Engine *e, unsigned *cadLen, uint64_t *nominalUsed);

// Reads the external clock detector (valid when the device is idle or streaming).
bool  d003_read_external(D003Engine *e, bool *detected, unsigned *rate);

// Optional hook run on the engine thread after each rx batch (e.g. for ADAT loopback).
typedef void (*D003RxHook)(D003Engine *e, int64_t fromFrame, int64_t toFrame, void *ctx);
void  d003_set_rx_hook(D003Engine *e, D003RxHook hook, void *ctx);
