// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// Protocol and the "double-oh-three" scrambling are derived from the Linux driver sound/firewire/digi00x:
//   Copyright (c) 2014-2015 Takashi Sakamoto
//   Copyright (C) 2012 Robin Gareus, Damien Zammit (amdtp-dot)
// d003engine.c — see d003engine.h
#include "d003engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
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

#define NTX        40        // max tx ring (5 ms); runtime size e->ntx is 40 (safe) or 24 (low latency)
#define NNOM       80        // nominal cadence period (one full 44.1k pattern)
#define NRX        128       // rx ring (16 ms)
#define CB_EVERY   8         // callback every 8 packets (1 ms)
#define PKT_STRIDE 1024
#define RING_MASK  (D003_RING_FRAMES - 1)
#define WINDOWS    64        // clock model: window minima kept (64 * 125 ms = 8 s)
#define WINDOW_FRAMES_DIV 8  // window length = rate / 8 frames (125 ms)

#define BE(x) CFSwapInt32HostToBig(x)
#define FROMBE(x) CFSwapInt32BigToHost(x)

struct dot_state { uint8_t carry, idx; unsigned off; };

struct D003Engine {
    // FireWire objects (engine thread only)
    io_object_t dev;
    IOFireWireLibDeviceRef fw;
    io_object_t svc;
    CFRunLoopRef rl;
    pthread_t thread;
    dispatch_semaphore_t ready;

    IOFireWireLibNuDCLPoolRef txPool, rxPool;
    IOFireWireLibLocalIsochPortRef txPort, rxPort;
    IOFireWireLibRemoteIsochPortRef devListen, devTalk;
    IOFireWireLibIsochChannelRef chOut, chIn;
    UInt32 chanTx, chanRx;
    uint8_t *txBuf, *rxBuf;

    // stream state
    atomic_bool running;
    unsigned rate, pcm, dbs, sfc;
    uint8_t txBlocks[NNOM];         // nominal cadence (used until rx cadence is available)
    uint8_t txCur[NTX];             // current size (blocks) programmed in each tx DCL
    NuDCLRef txDcl[NTX];
    NuDCLRef rxDcl[NRX];
    uint8_t cad[1024]; unsigned cadHead, cadTail;   // rx cadence FIFO (blocks per packet), engine thread only
    int cadenceMode;                // 0 = mirror rx cadence, 1 = nominal only (diagnostic)
    uint64_t nominalUsed;
    unsigned nomIdx;
    uint8_t txDbc, rxDbc; bool rxDbcValid;
    unsigned rxNext;
    UInt16 sid;
    struct dot_state dot;

    _Atomic int64_t rxFrames, txFrames;
    float *inRing, *outRing;

    // clock model
    double nominalTpf;                 // mach ticks per frame (nominal)
    int64_t winStartFrame; double winMinOff; int64_t winMinFrame;
    double wFrame[WINDOWS], wOff[WINDOWS]; int wCount, wHead;
    pthread_mutex_t modelLock;
    double mTpf, mIntercept; bool mValid;   // host = mIntercept + frame * mTpf
    double lastLatSpread;

    D003Stats st;
    bool rtOK;
    uint64_t lastTxCb; double txSlackTicks;
    D003RxHook hook; void *hookCtx;

    // analog 1-8 -> ADAT out 1-8 loopback (done in the rx path, written ahead of the tx reader)
    _Atomic int loopCount;            // analog channels mirrored to ADAT (0, 4 or 8)
    unsigned ntx; bool lowLatency; unsigned optical;   // applied at start
    float peak[D003_MAX_CH];          // input peaks since last d003_take_peaks
    float outPeak[D003_MAX_CH];
    // MIDI (port 1 of the 003): single-producer/single-consumer byte rings
    uint8_t midiIn[1024];  _Atomic unsigned midiInHead, midiInTail;
    uint8_t midiOut[1024]; _Atomic unsigned midiOutHead, midiOutTail;
    double midiTokens;
    void (*midiInCb)(void *); void *midiInCtx; bool midiPending;
    int64_t loopLead;                 // frames between capture and ADAT output; fixed per session (-1 = not yet)
};

// ---------------------------------------------------------------- register access
static IOReturn wr(D003Engine *e, UInt32 off, UInt32 v) {
    UInt32 gen = 0; (*e->fw)->GetBusGeneration(e->fw, &gen);
    FWAddress a = { 0, BASE_HI, BASE_LO + off };
    return (*e->fw)->WriteQuadlet(e->fw, e->svc, &a, BE(v), false, gen);
}
static IOReturn rd(D003Engine *e, UInt32 off, UInt32 *v) {
    UInt32 gen = 0; (*e->fw)->GetBusGeneration(e->fw, &gen);
    FWAddress a = { 0, BASE_HI, BASE_LO + off };
    IOReturn r = (*e->fw)->ReadQuadlet(e->fw, e->svc, &a, v, false, gen);
    *v = FROMBE(*v);
    return r;
}

// ---------------------------------------------------------------- double-oh-three scrambling (output only)
static uint8_t dot_scrt(uint8_t idx, unsigned off) {
    static const uint8_t len[16] = {0, 1, 3, 5, 7, 9, 11, 13, 14, 12, 10, 8, 6, 4, 2, 0};
    static const uint8_t nib[15] = {0x8, 0x7, 0x9, 0x6, 0xa, 0x5, 0xb, 0x4, 0xc, 0x3, 0xd, 0x2, 0xe, 0x1, 0xf};
    static const uint8_t hir[15] = {0x0, 0x6, 0xf, 0x8, 0x7, 0x5, 0x3, 0x4, 0xc, 0xd, 0xe, 0x1, 0x2, 0xb, 0xa};
    static const uint8_t hio[16] = {0, 11, 12, 6, 7, 5, 1, 4, 3, 0x00, 14, 13, 8, 9, 10, 2};
    uint8_t ln = idx & 0xf, hn = (idx >> 4) & 0xf;
    uint8_t hr = (hn == 0x9) ? 0x9 : hir[(hio[hn] + off) % 15];
    if (len[ln] < off) return 0;
    return nib[14 + off - len[ln]] | (hr << 4);
}
static inline void dot_encode(struct dot_state *s, UInt32 *beWord) {
    uint8_t *d = (uint8_t *)beWord;
    if (d[2] != 0) { s->off = 0; s->idx = d[2] ^ s->carry; }
    d[2] ^= s->carry;
    s->carry = dot_scrt(s->idx, ++s->off);
}

// ---------------------------------------------------------------- clock model
static void model_feed(D003Engine *e, int64_t frames, uint64_t now) {
    if (frames <= 0) return;
    double off = (double)now - (double)frames * e->nominalTpf;
    int64_t winLen = e->rate / WINDOW_FRAMES_DIV;
    if (e->winStartFrame < 0) { e->winStartFrame = frames; e->winMinOff = off; e->winMinFrame = frames; }
    if (off < e->winMinOff) { e->winMinOff = off; e->winMinFrame = frames; }
    if (frames - e->winStartFrame < winLen) return;

    // close window: store its minimum (least-delayed callback)
    e->wFrame[e->wHead] = (double)e->winMinFrame; e->wOff[e->wHead] = e->winMinOff;
    e->wHead = (e->wHead + 1) % WINDOWS; if (e->wCount < WINDOWS) e->wCount++;
    e->winStartFrame = frames; e->winMinOff = off; e->winMinFrame = frames;

    // linear fit off = a + b*frame  ->  host = a + frame*(nominal + b)
    int n = e->wCount; double sx = 0, sy = 0, sxx = 0, sxy = 0, x0 = e->wFrame[(e->wHead + WINDOWS - n) % WINDOWS];
    for (int i = 0; i < n; i++) { int k = (e->wHead + WINDOWS - n + i) % WINDOWS; double x = e->wFrame[k] - x0, y = e->wOff[k];
        sx += x; sy += y; sxx += x * x; sxy += x * y; }
    double b = 0, a;
    if (n >= 4) { double den = n * sxx - sx * sx; if (den > 0) b = (n * sxy - sx * sy) / den; }
    a = (sy - b * sx) / n;
    // the fit passes through the middle of the minima; shift down so the line hugs the lower envelope
    double minResid = 0, maxResid = 0;
    for (int i = 0; i < n; i++) { int k = (e->wHead + WINDOWS - n + i) % WINDOWS; double r = e->wOff[k] - (a + b * (e->wFrame[k] - x0));
        if (r < minResid) minResid = r; if (r > maxResid) maxResid = r; }
    a += minResid;
    pthread_mutex_lock(&e->modelLock);
    e->mTpf = e->nominalTpf + b;
    e->mIntercept = a - b * x0;
    e->mValid = n >= 2;
    pthread_mutex_unlock(&e->modelLock);
    e->st.ppm = b / e->nominalTpf * 1e6;
    e->lastLatSpread = maxResid - minResid;
}

bool d003_host_time_for(D003Engine *e, double sampleTime, uint64_t *hostTime, double *tpf) {
    pthread_mutex_lock(&e->modelLock);
    bool ok = e->mValid;
    double t = e->mIntercept + sampleTime * e->mTpf, k = e->mTpf;
    pthread_mutex_unlock(&e->modelLock);
    if (!ok) return false;
    if (hostTime) *hostTime = (uint64_t)llround(t);
    if (tpf) *tpf = k;
    return true;
}

// ---------------------------------------------------------------- tx
static bool fill_tx_packet(D003Engine *e, unsigned i) {
    // mirror the device's own cadence so its media clock and ours never drift apart
    unsigned nb;
    if (e->cadenceMode == 0 && e->cadHead != e->cadTail) { nb = e->cad[e->cadTail]; e->cadTail = (e->cadTail + 1) & 1023; }
    else { nb = e->txBlocks[e->nomIdx]; e->nominalUsed++; }
    e->nomIdx = (e->nomIdx + 1) % NNOM;
    if (nb > 8) nb = 8;
    bool resized = false;
    if (nb != e->txCur[i] && e->txDcl[i]) {
        IOVirtualRange rg = { (IOVirtualAddress)(e->txBuf + i * PKT_STRIDE), 8 + nb * e->dbs * 4 };
        (*e->txPool)->SetDCLRanges(e->txDcl[i], 1, &rg);
        e->txCur[i] = nb; resized = true;
    }
    UInt32 *q = (UInt32 *)(e->txBuf + i * PKT_STRIDE);
    q[0] = BE(((UInt32)(e->sid & 0x3f) << 24) | (e->dbs << 16) | e->txDbc);
    q[1] = BE(0x80000000u | (0x10u << 24) | (e->sfc << 16) | 0xffff);
    e->txDbc = (uint8_t)(e->txDbc + nb);
    UInt32 *blk = q + 2;
    int64_t f = atomic_load_explicit(&e->txFrames, memory_order_relaxed);
    uint8_t blockDbc = (uint8_t)(e->txDbc - nb);
    for (unsigned b = 0; b < nb; b++, blk += e->dbs, f++, blockDbc++) {
        // MIDI out: the 003 reads port N from blocks whose DBC % 8 == N; rate-limit to the 31.25 kbaud wire
        uint8_t *m = (uint8_t *)&blk[0];
        m[0] = 0x80; m[1] = m[2] = m[3] = 0;
        if (e->midiTokens < 2.0) e->midiTokens += 3093.0 / e->rate;
        if ((blockDbc & 7) == 0) {
            unsigned t = atomic_load_explicit(&e->midiOutTail, memory_order_relaxed), n = 0;
            while (n < 2 && e->midiTokens >= 1.0 && t != atomic_load_explicit(&e->midiOutHead, memory_order_acquire)) {
                m[1 + n++] = e->midiOut[t]; t = (t + 1) & 1023; e->midiTokens -= 1.0;
            }
            atomic_store_explicit(&e->midiOutTail, t, memory_order_release);
            m[3] = (uint8_t)n;
        }
        float *src = e->outRing + (size_t)(f & RING_MASK) * D003_MAX_CH;
        for (unsigned c = 0; c < e->pcm; c++) {
            float x = src[c]; src[c] = 0.f;
            { float a = fabsf(x); if (a > e->outPeak[c]) e->outPeak[c] = a; }
            if (x > 1.f) x = 1.f; else if (x < -1.f) x = -1.f;
            int32_t s = (int32_t)lrintf(x * 8388607.f);
            UInt32 w = BE(((UInt32)s & 0x00ffffffu) | 0x40000000u);
            dot_encode(&e->dot, &w);
            blk[1 + c] = w;
        }
    }
    atomic_store_explicit(&e->txFrames, f, memory_order_release);
    e->st.txPackets++;
    return resized;
}

static void txCallback(void *refcon, NuDCLRef dcl) {
    D003Engine *e = (D003Engine *)((uintptr_t)refcon & ~(uintptr_t)0xff);
    unsigned k = (unsigned)((uintptr_t)refcon & 0xff);
    // callbacks can still be queued on the run loop after a stop (or from a previous session): ignore them
    if (!atomic_load(&e->running) || !e->txPool || k >= e->ntx) return;
    {   // the refilled packets go out again (ntx - CB_EVERY) cycles after this DCL; a gap longer than that means stale resends
        uint64_t now = mach_absolute_time();
        if (e->lastTxCb && (double)(now - e->lastTxCb) > e->txSlackTicks) e->st.txLate++;
        e->lastTxCb = now;
    }
    void *mod[CB_EVERY]; UInt32 nmod = 0;
    for (unsigned j = k + 1 - CB_EVERY; j <= k; j++) if (fill_tx_packet(e, j)) mod[nmod++] = e->txDcl[j];
    if (nmod) (*e->txPort)->Notify(e->txPort, kFWNuDCLModifyNotification, mod, nmod);
}

// ---------------------------------------------------------------- rx
static void process_rx_packet(D003Engine *e, const uint8_t *b) {
    UInt32 hdr = *(const UInt32 *)b;
    UInt32 len = hdr >> 16;
    if (len < 8 || len > PKT_STRIDE - 4) { e->st.rxBad++; return; }
    const UInt32 *q = (const UInt32 *)(b + 4);
    UInt32 c0 = FROMBE(q[0]);
    unsigned dbs = (c0 >> 16) & 0xff; uint8_t dbc = c0 & 0xff;
    e->st.rxPackets++;
    unsigned nextHead = (e->cadHead + 1) & 1023;
    if (len == 8) { e->st.rxEmpty++; if (nextHead != e->cadTail) { e->cad[e->cadHead] = 0; e->cadHead = nextHead; } return; }
    if (dbs != e->dbs) { e->st.rxBad++; return; }
    unsigned nb = (len - 8) / (dbs * 4);
    if (nextHead != e->cadTail) { e->cad[e->cadHead] = (uint8_t)nb; e->cadHead = nextHead; }
    if (e->rxDbcValid && dbc != e->rxDbc) e->st.rxDbcErrors++;
    e->rxDbc = (uint8_t)(dbc + nb); e->rxDbcValid = true;
    const UInt32 *d = q + 2;
    int64_t f = atomic_load_explicit(&e->rxFrames, memory_order_relaxed);
    int loopN = e->loopLead > 0 ? atomic_load_explicit(&e->loopCount, memory_order_relaxed) : 0;
    for (unsigned i = 0; i < nb; i++, d += dbs, f++) {
        // MIDI conformant quadlet: byte3 low nibble = length, high nibble = port (0 = MIDI IN of the 003)
        const uint8_t *m = (const uint8_t *)&d[0];
        unsigned mlen = m[3] & 0x0f;
        if (mlen > 0 && mlen <= 2 && (m[3] >> 4) == 0) {
            unsigned h = atomic_load_explicit(&e->midiInHead, memory_order_relaxed);
            for (unsigned k = 0; k < mlen; k++) {
                unsigned nh = (h + 1) & 1023;
                if (nh == atomic_load_explicit(&e->midiInTail, memory_order_acquire)) break;
                e->midiIn[h] = m[1 + k]; h = nh;
            }
            atomic_store_explicit(&e->midiInHead, h, memory_order_release);
            e->midiPending = true;
        }
        float *dst = e->inRing + (size_t)(f & RING_MASK) * D003_MAX_CH;
        for (unsigned c = 0; c < e->pcm; c++) {
            float v = (float)((int32_t)(FROMBE(d[1 + c]) << 8)) * (1.f / 2147483648.f);
            dst[c] = v;
            float a = fabsf(v); if (a > e->peak[c]) e->peak[c] = a;
        }
        if (loopN) {
            float *o = e->outRing + (size_t)((f + e->loopLead) & RING_MASK) * D003_MAX_CH;
            for (int c = 0; c < loopN; c++) o[D003_CH_ADAT + c] = dst[c];   // stream order: analog 1-8, S/PDIF L/R, ADAT 1-8
        }
    }
    atomic_store_explicit(&e->rxFrames, f, memory_order_release);
}

static void rxCallback(void *refcon, NuDCLRef dcl) {
    D003Engine *e = (D003Engine *)((uintptr_t)refcon & ~(uintptr_t)0xff);
    unsigned k = (unsigned)((uintptr_t)refcon & 0xff);
    if (!atomic_load(&e->running) || !e->rxPool || k >= NRX) return;
    uint64_t now = mach_absolute_time();
    int64_t before = atomic_load(&e->rxFrames);
    if (e->loopLead < 0 && before > 0) {
        // fix the loopback delay once: current tx lead plus one callback period of margin
        e->loopLead = atomic_load(&e->txFrames) - before + CB_EVERY * 6 + 16;
    }
    unsigned n = (k + NRX - e->rxNext) % NRX + 1;
    if (n > CB_EVERY) e->st.rxOverruns++;
    for (unsigned j = 0; j < n; j++) process_rx_packet(e, e->rxBuf + ((e->rxNext + j) % NRX) * PKT_STRIDE);
    e->rxNext = (k + 1) % NRX;
    int64_t after = atomic_load(&e->rxFrames);
    model_feed(e, after, now);
    e->st.jitterUs = e->lastLatSpread / e->nominalTpf / e->rate * 1e6;
    if (e->hook && after > before) e->hook(e, before, after, e->hookCtx);
    if (e->midiPending) { e->midiPending = false; if (e->midiInCb) e->midiInCb(e->midiInCtx); }
}

// ---------------------------------------------------------------- remote port handlers
static IOReturn rGetSupported(IOFireWireLibIsochPortRef p, IOFWSpeed *s, UInt64 *ch) { *s = kFWSpeed400MBit; *ch = ~0ULL; return kIOReturnSuccess; }
static IOReturn rAllocate(IOFireWireLibIsochPortRef p, IOFWSpeed s, UInt32 chan) { *(UInt32 *)(*p)->GetRefCon(p) = chan; return kIOReturnSuccess; }
static IOReturn rNop(IOFireWireLibIsochPortRef p) { return kIOReturnSuccess; }

static void *pageAlloc(size_t n) {
    vm_address_t a = 0;
    if (vm_allocate(mach_task_self(), &a, n, VM_FLAGS_ANYWHERE)) return NULL;
    return (void *)a;
}

// ---------------------------------------------------------------- engine thread
static void finish_session(D003Engine *e) {
    wr(e, R_STREAMING_SET, 3);
    wr(e, R_ISOC_CHANNELS, 0);
    usleep(50000);
}

static void teardown(D003Engine *e) {
    atomic_store(&e->running, false);           // first: makes late callbacks no-ops
    if (e->chIn)  { (*e->chIn)->Stop(e->chIn); }
    if (e->chOut) { (*e->chOut)->Stop(e->chOut); }
    finish_session(e);
    if (e->chIn)  { (*e->chIn)->ReleaseChannel(e->chIn);  (*e->chIn)->Release(e->chIn);  e->chIn = NULL; }
    if (e->chOut) { (*e->chOut)->ReleaseChannel(e->chOut); (*e->chOut)->Release(e->chOut); e->chOut = NULL; }
    if (e->txPort) { (*e->txPort)->Release(e->txPort); e->txPort = NULL; }
    if (e->rxPort) { (*e->rxPort)->Release(e->rxPort); e->rxPort = NULL; }
    if (e->devListen) { (*e->devListen)->Release(e->devListen); e->devListen = NULL; }
    if (e->devTalk)   { (*e->devTalk)->Release(e->devTalk); e->devTalk = NULL; }
    if (e->txPool) { (*e->txPool)->Release(e->txPool); e->txPool = NULL; }
    if (e->rxPool) { (*e->rxPool)->Release(e->rxPool); e->rxPool = NULL; }
    atomic_store(&e->running, false);
}

static int do_start(D003Engine *e, unsigned rate, D003Clock clock) {
    unsigned rateIdx;
    switch (rate) { case 44100: rateIdx = 0; e->sfc = 1; break; case 48000: rateIdx = 1; e->sfc = 2; break; default: return -1; }
    e->rate = rate; e->pcm = D003_MAX_CH; e->dbs = e->pcm + 1;
    for (unsigned i = 0; i < NNOM; i++)
        e->txBlocks[i] = rate == 48000 ? 6 : 5 + ((i & 1) ^ (i == 0 || i >= 40));

    finish_session(e);
    wr(e, R_LOCAL_RATE, rateIdx);
    wr(e, R_CLOCK_SOURCE, clock);
    {   // optical port: 0 = ADAT, 1 = S/PDIF — only write when it actually changes (as the Linux driver does)
        UInt32 cur = 0;
        if (!rd(e, 0x011c, &cur) && (cur & 1) != e->optical) wr(e, 0x011c, e->optical);
    }
    e->ntx = e->lowLatency ? 24 : NTX;
    { mach_timebase_info_data_t tb; mach_timebase_info(&tb); e->txSlackTicks = (e->ntx - CB_EVERY + CB_EVERY) * 125000.0 * tb.denom / tb.numer; }
    e->lastTxCb = 0;
    e->midiTokens = 0;
    memset(e->peak, 0, sizeof e->peak); memset(e->outPeak, 0, sizeof e->outPeak);
    (*e->fw)->GetLocalNodeID(e->fw, &e->sid);

    memset(e->outRing, 0, sizeof(float) * D003_RING_FRAMES * D003_MAX_CH);
    memset(e->inRing, 0, sizeof(float) * D003_RING_FRAMES * D003_MAX_CH);
    atomic_store(&e->rxFrames, 0); atomic_store(&e->txFrames, 0);
    e->txDbc = 0; e->rxDbcValid = false; e->rxNext = 0;
    e->cadHead = e->cadTail = 0; e->nomIdx = 0; e->nominalUsed = 0; e->loopLead = -1;
    memset(e->txDcl, 0, sizeof e->txDcl);
    memset(&e->dot, 0, sizeof e->dot);
    memset(&e->st, 0, sizeof e->st);
    e->nominalTpf = 0; { mach_timebase_info_data_t tb; mach_timebase_info(&tb); e->nominalTpf = 1e9 / rate * tb.denom / tb.numer; }
    e->winStartFrame = -1; e->wCount = e->wHead = 0; e->mValid = false;

    CFUUIDBytes poolID = CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID);
    CFUUIDBytes portID = CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID);

    // tx program
    e->txPool = (*e->fw)->CreateNuDCLPool(e->fw, e->ntx, poolID);
    if (!e->txPool) goto fail;
    (*e->txPool)->SetCurrentTagAndSync(e->txPool, 1, 0);
    NuDCLRef first = NULL, prev = NULL;
    for (unsigned i = 0; i < e->ntx; i++) {
        IOVirtualRange rg = { (IOVirtualAddress)(e->txBuf + i * PKT_STRIDE), 8 + e->txBlocks[i % NNOM] * e->dbs * 4 };
        NuDCLRef d = (*e->txPool)->AllocateSendPacket(e->txPool, NULL, 1, &rg);
        if (!d) goto fail;
        e->txCur[i] = e->txBlocks[i % NNOM];
        e->txDcl[i] = d;
        (*e->txPool)->SetDCLFlags(d, kNuDCLDynamic);
        if (i % CB_EVERY == CB_EVERY - 1) {
            (*e->txPool)->SetDCLCallback(d, txCallback);
            (*e->txPool)->SetDCLRefcon(d, (void *)((uintptr_t)e | i));
        }
        if (!first) first = d; prev = d;
    }
    (*e->txPool)->SetDCLBranch(prev, first);
    for (unsigned i = 0; i < e->ntx; i++) fill_tx_packet(e, i);     // prefill (silence, nominal cadence)
    IOVirtualRange txAll = { (IOVirtualAddress)e->txBuf, e->ntx * PKT_STRIDE };
    e->txPort = (*e->fw)->CreateLocalIsochPortWithOptions(e->fw, true, (*e->txPool)->GetProgram(e->txPool), 0, 0, 0,
                    NULL, 0, &txAll, 1, kFWIsochPortUseSeparateKernelThread, portID);
    if (!e->txPort) goto fail;

    // rx program
    e->rxPool = (*e->fw)->CreateNuDCLPool(e->fw, NRX, poolID);
    if (!e->rxPool) goto fail;
    first = prev = NULL;
    for (unsigned i = 0; i < NRX; i++) {
        IOVirtualRange rg = { (IOVirtualAddress)(e->rxBuf + i * PKT_STRIDE), PKT_STRIDE };
        NuDCLRef d = (*e->rxPool)->AllocateReceivePacket(e->rxPool, NULL, 4, 1, &rg);
        if (!d) goto fail;
        e->rxDcl[i] = d;
        if (i % CB_EVERY == CB_EVERY - 1) {
            (*e->rxPool)->SetDCLCallback(d, rxCallback);
            (*e->rxPool)->SetDCLRefcon(d, (void *)((uintptr_t)e | i));
            (*e->rxPool)->SetDCLFlags(d, kNuDCLUpdateBeforeCallback);
        }
        if (!first) first = d; prev = d;
    }
    (*e->rxPool)->SetDCLBranch(prev, first);
    IOVirtualRange rxAll = { (IOVirtualAddress)e->rxBuf, NRX * PKT_STRIDE };
    e->rxPort = (*e->fw)->CreateLocalIsochPortWithOptions(e->fw, false, (*e->rxPool)->GetProgram(e->rxPool), 0, 0, 0,
                    NULL, 0, &rxAll, 1, kFWIsochPortUseSeparateKernelThread, portID);
    if (!e->rxPort) goto fail;

    // remote ports & channels
    CFUUIDBytes rID = CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID);
    e->devListen = (*e->fw)->CreateRemoteIsochPort(e->fw, false, rID);
    e->devTalk   = (*e->fw)->CreateRemoteIsochPort(e->fw, true, rID);
    if (!e->devListen || !e->devTalk) goto fail;
    IOFireWireLibRemoteIsochPortRef rp[2] = { e->devListen, e->devTalk };
    for (int i = 0; i < 2; i++) {
        (*rp[i])->SetGetSupportedHandler(rp[i], rGetSupported);
        (*rp[i])->SetAllocatePortHandler(rp[i], rAllocate);
        (*rp[i])->SetReleasePortHandler(rp[i], rNop);
        (*rp[i])->SetStartHandler(rp[i], rNop);
        (*rp[i])->SetStopHandler(rp[i], rNop);
    }
    (*e->devListen)->SetRefCon((IOFireWireLibIsochPortRef)e->devListen, &e->chanRx);
    (*e->devTalk)->SetRefCon((IOFireWireLibIsochPortRef)e->devTalk, &e->chanTx);

    CFUUIDBytes cID = CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID);
    UInt32 maxPayload = 8 + 6 * e->dbs * 4;
    e->chOut = (*e->fw)->CreateIsochChannel(e->fw, true, maxPayload, kFWSpeed400MBit, cID);
    e->chIn  = (*e->fw)->CreateIsochChannel(e->fw, true, maxPayload, kFWSpeed400MBit, cID);
    if (!e->chOut || !e->chIn) goto fail;
    (*e->chOut)->SetTalker(e->chOut, (IOFireWireLibIsochPortRef)e->txPort);
    (*e->chOut)->AddListener(e->chOut, (IOFireWireLibIsochPortRef)e->devListen);
    (*e->chIn)->SetTalker(e->chIn, (IOFireWireLibIsochPortRef)e->devTalk);
    (*e->chIn)->AddListener(e->chIn, (IOFireWireLibIsochPortRef)e->rxPort);
    if ((*e->chOut)->AllocateChannel(e->chOut) || (*e->chIn)->AllocateChannel(e->chIn)) goto fail;

    // begin_session
    wr(e, R_ISOC_CHANNELS, (e->chanTx << 16) | e->chanRx);
    UInt32 curr = 0; rd(e, R_STREAMING_STATE, &curr);
    if (curr == 0) curr = 2;
    for (curr--; curr > 0; curr--) { wr(e, R_STREAMING_SET, curr); usleep(20000); }

    atomic_store(&e->running, true);
    if ((*e->chOut)->Start(e->chOut)) goto fail;
    if ((*e->chIn)->Start(e->chIn)) goto fail;
    return 0;
fail:
    teardown(e);
    return -1;
}

static void *engine_thread(void *arg) {
    D003Engine *e = arg;
    pthread_setname_np("Digi003 FireWire");
    // real-time (time-constraint) scheduling like Core Audio's own IO threads: the tx ring only has a few ms of slack,
    // and a late refill makes the hardware resend a stale packet, which the 003 buffers as extra audio
    {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        double msToAbs = 1e6 * tb.denom / tb.numer;
        thread_time_constraint_policy_data_t pol = {
            .period = (uint32_t)(1.0 * msToAbs), .computation = (uint32_t)(0.3 * msToAbs),
            .constraint = (uint32_t)(1.0 * msToAbs), .preemptible = 1 };
        kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        e->rtOK = kr == KERN_SUCCESS;
    }
    e->rl = CFRunLoopGetCurrent();
    CFRetain(e->rl);
    (*e->fw)->AddCallbackDispatcherToRunLoop(e->fw, e->rl);
    (*e->fw)->AddIsochCallbackDispatcherToRunLoop(e->fw, e->rl);
    // keep-alive source so the run loop never exits
    CFRunLoopTimerRef keep = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, NULL, NULL);
    CFRunLoopAddTimer(e->rl, keep, kCFRunLoopDefaultMode);
    dispatch_semaphore_signal(e->ready);
    CFRunLoopRun();
    return NULL;
}

static void on_engine(D003Engine *e, void (^block)(void)) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CFRunLoopPerformBlock(e->rl, kCFRunLoopDefaultMode, ^{ block(); dispatch_semaphore_signal(done); });
    CFRunLoopWakeUp(e->rl);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}

// ---------------------------------------------------------------- public API
D003Engine *d003_open(char *err, size_t errlen) {
    io_iterator_t it; io_object_t d, dev = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOFireWireDevice"), &it)) {
        snprintf(err, errlen, "IOServiceGetMatchingServices failed"); return NULL;
    }
    while ((d = IOIteratorNext(it))) {
        long v = 0; CFTypeRef r = IORegistryEntryCreateCFProperty(d, CFSTR("Vendor_ID"), kCFAllocatorDefault, 0);
        if (r) { CFNumberGetValue(r, kCFNumberLongType, &v); CFRelease(r); }
        if (v == VENDOR_DIGIDESIGN && !dev) dev = d; else IOObjectRelease(d);
    }
    IOObjectRelease(it);
    if (!dev) { snprintf(err, errlen, "no Digidesign 00x on the FireWire bus"); return NULL; }

    IOCFPlugInInterface **plug = NULL; SInt32 score; IOFireWireLibDeviceRef fw = NULL;
    kern_return_t kr = IOCreatePlugInInterfaceForService(dev, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (kr || !plug) { snprintf(err, errlen, "IOCreatePlugInInterfaceForService 0x%x", kr); IOObjectRelease(dev); return NULL; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9), (LPVOID *)&fw);
    IODestroyPlugInInterface(plug);
    if (!fw) { snprintf(err, errlen, "QueryInterface v9 failed"); IOObjectRelease(dev); return NULL; }
    if ((kr = (*fw)->Open(fw))) { snprintf(err, errlen, "Open 0x%x (device busy?)", kr); (*fw)->Release(fw); IOObjectRelease(dev); return NULL; }

    D003Engine *e = NULL;   // 256-byte aligned: DCL refcons pack (engine | packet index)
    if (posix_memalign((void **)&e, 256, sizeof *e)) { (*fw)->Close(fw); (*fw)->Release(fw); IOObjectRelease(dev); return NULL; }
    memset(e, 0, sizeof *e);
    e->dev = dev; e->fw = fw; e->svc = (*fw)->GetDevice(fw);
    e->txBuf = pageAlloc(NTX * PKT_STRIDE);
    e->rxBuf = pageAlloc(NRX * PKT_STRIDE);
    e->inRing = calloc((size_t)D003_RING_FRAMES * D003_MAX_CH, sizeof(float));
    e->outRing = calloc((size_t)D003_RING_FRAMES * D003_MAX_CH, sizeof(float));
    pthread_mutex_init(&e->modelLock, NULL);
    e->ready = dispatch_semaphore_create(0);
    pthread_attr_t at; pthread_attr_init(&at);
    struct sched_param sp = { .sched_priority = 47 };
    pthread_attr_setschedpolicy(&at, SCHED_RR); pthread_attr_setschedparam(&at, &sp);
    pthread_create(&e->thread, &at, engine_thread, e);
    dispatch_semaphore_wait(e->ready, DISPATCH_TIME_FOREVER);
    return e;
}

int d003_start(D003Engine *e, unsigned rate, D003Clock clock) {
    __block int r = 0;
    if (atomic_load(&e->running)) d003_stop(e);
    on_engine(e, ^{ r = do_start(e, rate, clock); });
    return r;
}

void d003_stop(D003Engine *e) {
    if (!atomic_load(&e->running)) return;
    on_engine(e, ^{ teardown(e); });
}

void d003_close(D003Engine *e) {
    if (!e) return;
    d003_stop(e);
    on_engine(e, ^{ (*e->fw)->Close(e->fw); CFRunLoopStop(e->rl); });
    pthread_join(e->thread, NULL);
    (*e->fw)->Release(e->fw);
    IOObjectRelease(e->dev);
    CFRelease(e->rl);
    vm_deallocate(mach_task_self(), (vm_address_t)e->txBuf, NTX * PKT_STRIDE);
    vm_deallocate(mach_task_self(), (vm_address_t)e->rxBuf, NRX * PKT_STRIDE);
    free(e->inRing); free(e->outRing);
    free(e);
}

bool d003_running(D003Engine *e) { return atomic_load(&e->running); }
bool d003_realtime(D003Engine *e) { return e->rtOK; }
unsigned d003_rate(D003Engine *e) { return e->rate; }
unsigned d003_channels(D003Engine *e) { return e->pcm ? e->pcm : D003_MAX_CH; }
int64_t d003_rx_frames(D003Engine *e) { return atomic_load_explicit(&e->rxFrames, memory_order_acquire); }
int64_t d003_tx_frames(D003Engine *e) { return atomic_load_explicit(&e->txFrames, memory_order_acquire); }
float *d003_in_ring(D003Engine *e) { return e->inRing; }
float *d003_out_ring(D003Engine *e) { return e->outRing; }
void d003_get_stats(D003Engine *e, D003Stats *s) { *s = e->st; }
unsigned d003_output_lead_frames(D003Engine *e) { return (e->lowLatency ? 24u : (unsigned)NTX) * 6 + 96; }
void d003_set_adat_loopback(D003Engine *e, bool on) { atomic_store(&e->loopCount, on ? 8 : 0); }
void d003_set_adat_route(D003Engine *e, int analogChannels) { atomic_store(&e->loopCount, analogChannels == 4 ? 4 : analogChannels ? 8 : 0); }
void d003_set_low_latency(D003Engine *e, bool on) { e->lowLatency = on; }
void d003_set_cadence_mode(D003Engine *e, int mode) { e->cadenceMode = mode; }
void d003_debug(D003Engine *e, unsigned *cadLen, uint64_t *nominalUsed) { *cadLen = (e->cadHead - e->cadTail) & 1023; *nominalUsed = e->nominalUsed; }
void d003_set_optical_mode(D003Engine *e, unsigned mode) { e->optical = mode ? 1 : 0; }
void d003_take_peaks(D003Engine *e, float in[D003_MAX_CH], float out[D003_MAX_CH]) {
    for (int c = 0; c < D003_MAX_CH; c++) { in[c] = e->peak[c]; e->peak[c] = 0; out[c] = e->outPeak[c]; e->outPeak[c] = 0; }
}
size_t d003_midi_read(D003Engine *e, uint8_t *buf, size_t max) {
    unsigned t = atomic_load_explicit(&e->midiInTail, memory_order_relaxed), h = atomic_load_explicit(&e->midiInHead, memory_order_acquire);
    size_t n = 0;
    while (t != h && n < max) { buf[n++] = e->midiIn[t]; t = (t + 1) & 1023; }
    atomic_store_explicit(&e->midiInTail, t, memory_order_release);
    return n;
}
size_t d003_midi_write(D003Engine *e, const uint8_t *buf, size_t n) {
    unsigned h = atomic_load_explicit(&e->midiOutHead, memory_order_relaxed);
    size_t i = 0;
    for (; i < n; i++) {
        unsigned nh = (h + 1) & 1023;
        if (nh == atomic_load_explicit(&e->midiOutTail, memory_order_acquire)) break;
        e->midiOut[h] = buf[i]; h = nh;
    }
    atomic_store_explicit(&e->midiOutHead, h, memory_order_release);
    return i;
}
void d003_set_midi_in_callback(D003Engine *e, void (*cb)(void *), void *ctx) { e->midiInCtx = ctx; e->midiInCb = cb; }
int64_t d003_loopback_latency_frames(D003Engine *e) { return e->loopLead; }
bool d003_read_external(D003Engine *e, bool *detected, unsigned *rate) {
    // synchronous FireWire reads block for ~1 ms: do them on the caller's thread, never on the real-time engine thread
    // (a time-constraint thread that blocks past its computation budget gets demoted, and then tx refills run late)
    UInt32 det = 0, er = 0;
    bool ok = !rd(e, 0x012c, &det) && !rd(e, 0x0114, &er);
    if (!ok) return false;
    static const unsigned rates[] = { 44100, 48000, 88200, 96000 };
    *detected = det != 0;
    *rate = (er & 0xf) < 4 ? rates[er & 0xf] : 0;
    return true;
}
void d003_set_rx_hook(D003Engine *e, D003RxHook h, void *ctx) { e->hookCtx = ctx; e->hook = h; }
