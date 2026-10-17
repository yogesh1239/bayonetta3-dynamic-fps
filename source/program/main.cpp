// Bayonetta 3 1.2.0 (main build id B47CC79B88B7091EA514B2B725C50AE01C6F00DF) - dynamic frame rate.
// Design: ../../ARCHITECTURE.md (C1 clock, C2 presentation, C3 modes, C4 smart-tick counters).
#include "lib.hpp"
#include <arm_neon.h>
#include <bit>
#include <algorithm>
#include <cmath>
#define MULTI_EXPIRY 0   // IMPL-25: the site 0x8DD54 is always installed, 0 keeps all the others off. IMPL-18: 1 = IMPL-14/15/15b multi-expiry replay at the 18 sites of impl/reload_multi.inc; 0 = none of those hooks installed (one native expiry)
#define REPLAY_371814 1   // IMPL-17: 1 = owner 0x371814 runs d native units per update (replay); 0 = the old per-counter hooks (hooks_int.inc) and CrossSucc
#pragma GCC optimize("fp-contract=off")   // IMPL-14 step 5b: the phase-5 unit must round like the native fmul + fadd (no fused multiply-add)

namespace {

template <typename T>
T& At(void* p, ptrdiff_t off) { return *reinterpret_cast<T*>(static_cast<u8*>(p) + off); }

template <typename Fn>
Fn Game(ptrdiff_t off) { return reinterpret_cast<Fn>(exl::util::modules::GetTargetStart() + off); }

constexpr ptrdiff_t kFrameTimer     = 0xC1F600; // timer(x0 = timer object, s0 = fixed step, w1 = 1 measured / 0 fixed)
constexpr ptrdiff_t kGetSystemTick  = 0x1321430;
constexpr ptrdiff_t kStepOverride   = 0xBA3300; // returns float, > 0 = sound-sync override active
constexpr ptrdiff_t kGotTickFreq    = 0x24ABBB0; // GOT slot -> s64 tick frequency

constexpr float kMaxStep = 4.0f;   // per-update step cap (IMPL-8 item 3): stock mode 2 already feeds consumers a fixed step of 4.0 (0x670C18 -> 0xC1F698)
constexpr float kMaxAdmit = 1.0f; // IMPL-44: user decision: one update advances at most ONE whole 60 Hz unit, the rest of the real time is dropped (slow motion below 60 FPS)
constexpr float kStallTicks = 15.0f; // raw > 15 ticks (250 ms) is a stall (loading hitch): keep only kMaxStep of it and clear the carries

// Clock state. Written only by the frame timer (main logic thread).
float g_step = 1.0f;       // this update, in 60 Hz ticks
float g_e0 = 1.0f;         // the step stored in timer+0xE0 this update (raw x override, IMPL-14 step 3)
u64 g_stepFull = 0;        // IMPL-55: step table full, oldest slot evicted
u64 g_stepZeroUnk = 0;      // IMPL-57: zero-length writes with no getter record (stale read keeps the stored value)
float g_dimBud[5]; bool g_dimBudOk = false, g_budConst = false;   // IMPL-57: D_k = budget_k / e0 of the global timer; g_budConst: budgets are the literal 1.0 of a reset
u8 g_getMode = 0; float g_getDim = 0.0f; u64 g_getN = 0; u32 g_getSeq = 0, g_getSeqSeen = 0;   // IMPL-57 getter record: 0 unknown, 1 scaled by e0 (dim d), 2 const
u32 g_stepStale = 0;       // IMPL-54: times the consumer 0x442FE0 replaced a stale cached step [obj+0x17A40]
// Raw-time queue (review round 10): pieces {raw left, override of the update that produced it}, oldest first. No ceiling; a stall clears it.
struct Piece { float raw, ov; };
constexpr int kQueue = 1024;
Piece g_q[kQueue]; int g_qn = 0;
u32 g_qDrop = 0;           // debug counter: pieces dropped because the FIFO was full (only far below 15 FPS)
// IMPL-12 item 2 (fix review 1): a zero (or NaN) piece is never stored; only the ADJACENT newest piece with an EQUAL override is merged; different overrides are
// never averaged. Full with no equal neighbour: the newest piece's raw is dropped and counted (inside the accepted slow-down boundary below 15 FPS).
void QPush(float raw, float ov) {
    if (!(raw > 0.0f)) return;
    if (g_qn > 0 && g_q[g_qn - 1].ov == ov) { g_q[g_qn - 1].raw += raw; return; }
    if (g_qn == kQueue) { g_qDrop++; return; }
    g_q[g_qn++] = {raw, ov};
}
// Admit up to cap raw from the front (split a piece if needed) -> admitted raw; e0 = sum(admitted x override).
Piece g_adm[kQueue + 1]; int g_admN = 0;   // IMPL-12 item 3: the pieces admitted in this update (raw, override), oldest first; TokenHook splits on them
float g_accBefore = 0.0f;                  // G accumulator before this update's raw is added (unit boundaries lie at k - g_accBefore in raw)
float QAdmit(float cap, float& e0) {
    float got = 0.0f; e0 = 0.0f; int spent = 0; g_admN = 0;
    while (spent < g_qn && got < cap) {
        Piece& p = g_q[spent];
        float take = std::min(p.raw, cap - got);
        if (take > 0.0f) g_adm[g_admN++] = {take, p.ov};
        got += take; e0 += take * p.ov; p.raw -= take;
        if (p.raw > 0.0f) break;           // split: the rest stays at the front
        spent++;
    }
    for (int k = spent; k < g_qn; k++) g_q[k - spent] = g_q[k];
    g_qn -= spent;
    return got;
}
float g_acc  = 0.0f;       // fraction of a tick not yet handed out
u64   g_dGt1 = 0;        // IMPL-44 diagnostic: g_T > 1 / tail ticks > 1 (before the clamp) or Credit de > 1; nothing reads it yet
int   g_T    = 1;          // whole ticks that ended in this update
u64   g_G    = 0;          // total ticks so far
u64   g_Gp   = 0, g_Gtp = 0; // IMPL-32: G / tail G before this update (G - Gp = whole units this update crossed)
float g_accTBefore = 0.0f;   // IMPL-32: tail accumulator before this update
u32 g_cntEvictLive = 0, g_genEvictLive = 0, g_velEvictLive = 0, g_pickEvictLive = 0, g_phEvictLive = 0, g_thrOverflow = 0;   // IMPL-52 diagnostics: running totals since boot, relaxed, printed as [B3DYN]
u64   g_N    = 0;          // update number
// D tail clock: hooks inside first-pass-only code (closure of tools/tail-closure.txt) count on this clock. Stock runs the logic pass twice
// in an update when "paired" (flags of 0x670C04), and the first-pass-only roots then run once per two passes, so the tail clock moves at half rate.
bool  g_paired = false;    // set at 0x670C1C by PairHook (w21 == 0)
float g_ft   = 1.0f;       // tail step of this update
float g_accT = 0.0f;
u64   g_Gt   = 0;          // tail ticks so far
constexpr uintptr_t kTail[] = {
#include "tail.inc"
    0 };
constexpr bool IsTail(uintptr_t a) { for (uintptr_t t : kTail) if (t == a) return true; return false; }
// IMPL-10 step 1: level-triggered zero-tick sites (event branch b.gt/b.ge/b.lt/b.le: native fires on every update while the value is past the limit).
// Credit<> keeps d = d_eff (0 allowed) there: the value stays and the native branch decides from it. Generated list: impl/level.inc.
constexpr uintptr_t kLevel[] = {
#include "level.inc"
    0 };
constexpr bool IsLevel(uintptr_t a) { for (uintptr_t t : kLevel) if (t == a) return true; return false; }
template <bool Tail> inline u64 Now() { return Tail ? g_Gt : g_G; }

inline u64 ThreadId() { u64 v; asm volatile("mrs %0, tpidrro_el0" : "=r"(v)); return v; }   // Switch thread pointer
u64 g_logicThread = 0;     // thread id of the frame timer caller (set in FrameTimer); template flag MainOnly compares against it
int g_segOn = 0;           // IMPL-28: number of open cloth segment scopes (StepFactor looks at the thread context only when non-zero)
int g_capOn = 0;           // number of owner calls on the logic thread that have a credit cap set (IMPL-14 step 4); Credit looks at the thread context only when it is non-zero
// ---- Per-thread context (IMPL-8 items 7, 8). The mod has no TLS segment, so a small table keyed by tpidrro_el0 (the Switch thread pointer) is used.
// IMPL-12 item 4: a captured async context also carries the paired flag of the submitter's update; while it is installed (hasPaired) the getter hooks use it, not g_paired.
// IMPL-14 step 3: it also carries the submitter's published step values (e0 = timer+0xE0, raw = admitted raw step); they are valid while hasPaired is set (an installed async context).
// ovOn/ov/ovRaw = the scoped step override of IMPL-14 step 4; credCap1 = credit cap + 1 (0 = none).
struct ThreadCtx { u64 thr; bool tail; bool paired; bool hasPaired; float e0, raw; bool ovOn; float ov, ovRaw; int credCap1; bool saved[4]; bool savedPaired[4]; bool savedHas[4]; float savedE0[4], savedRaw[4]; int depth; u64 stOwner; u64 stN; float stF, stG0, stT, stFac; u64 yOwner, yN;
    // IMPL-25: oOwner/oOld = old timer value seen before the 0x56DBDC subtract; xOwner/xK = expiries due at 0xB01C (consumed at 0xB054); forceOne = HandlerGate forced to n = 1 (site 0x8DD54);
    // cN/cG/cGt/cFt = clock of an installed async context (valid while hasPaired): update number, G, tail G, tail step; saved* = the stack of the outer values
    u64 oOwner; float oOld; u64 xOwner; int xK; int forceOne; u64 cN, cG, cGt; float cFt; u64 savedN[4], savedG[4], savedGt[4]; float savedFt[4];
    // IMPL-26: snap = getter snapshot of the installed async context (-1 none; ownSnap[d] = the context at depth d holds one reference, released by TailPop);
    // rp* = state of the random-picker pair replay (0x55A400..0x55A410: outer override / cap saved) and of the picker n == 0 cache (rpMode, cached draws, actor)
    int snap; int savedSnap[4]; bool ownSnap[4]; int rpActive; bool rpOvOn; float rpOv, rpOvRaw; int rpCap1; u32 rpMode, rpR1, rpR2; u64 rpActor; u64 aOwner, aN; float aAmp0, aDs;
    // IMPL-28: cloth segment scope (0x813154): segOn/segF = raw length of the segment (ramp factor at 0x814228 / 0x814260); cl* = stack of the outer values (obj = x19 of the call)
    // IMPL-33: cGp/cGtp/cAcc = G before the update, tail G before, phase before of the installed context (own copy: a 'clock live' record has no matching node); live = the record's token/C4 values come from a stale node
    u64 cGp, cGtp, savedGp[4], savedGtp[4]; float cAcc, savedAcc[4]; bool live, savedLive[4]; float cAccA, cAccTA, savedAccA[4], savedAccTA[4], cAccTB, savedAccTB[4];   // IMPL-35: phase AFTER the update (acc, accT)
    bool pArmed, pGuard2, pStepped; u64 pOwner; float pB2;   /* IMPL-41: plan of a 2-unit update at 0x10AE6C */ bool subOn;   /* IMPL-38 item 2: d = 0 at 0x10AEB8, skip the submit at 0x10AF04 */ bool segOn; float segF; int clDepth; u64 clObj[4]; bool clSegOn[4]; float clSegF[4]; int clCap1[4]; u64 lastSite, lastField, lastN; s64 lastDe; s32 lastW; int chain;   /* IMPL-49: armed chain id (0 = none) */   /* IMPL-43: last Credit of this thread (site, key, final de) */ };   // IMPL-23: stOwner/stN/stF/stG0 = gain-move stash (0x2BD90 -> 0x2BDAC), stT = target of 0x1158AC
constexpr int kThreads = 64;
ThreadCtx g_thr[kThreads];
ThreadCtx& Thr() {
    const u64 id = ThreadId();
    for (int i = 0; i < kThreads; i++) {
        ThreadCtx& t = g_thr[(id / 16 + i) % kThreads];
        u64 cur = __atomic_load_n(&t.thr, __ATOMIC_ACQUIRE);
        if (cur == id) return t;
        if (cur == 0 && __atomic_compare_exchange_n(&t.thr, &cur, id, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return t;
        if (cur == id) return t;
    }
    __atomic_add_fetch(&g_thrOverflow, 1u, __ATOMIC_RELAXED);   // IMPL-52: thrOverflow
    return g_thr[0];   // ponytail: more than 64 threads share slot 0
}
// IMPL-25 item 5: the clock a counter hook sees. Inside an installed async context (hasPaired) it is the clock of the SUBMITTING update (N, G or tail G, step or tail step),
// otherwise the live globals. n == 0: no clock yet (stock behaviour).
struct Clk { u64 n, now; float step; };
template <bool Tail> inline Clk ClockNow() {
    const ThreadCtx& t = Thr();
    if (t.hasPaired) return {t.cN, Tail ? t.cGt : t.cG, Tail ? t.cFt : t.raw};
    return {g_N, Now<Tail>(), Tail ? g_ft : g_step};
}

// C4 smart tick: one record per (site, field address).
struct Counter { u64 site, field, S, U; s64 carry; s64 left; u64 aux; };   // left = ticks the floor cut in the LAST visit (leftover after an expiry; IMPL-13 RELOAD-INT) // carry = ticks owed to / by the next credit of this key
constexpr int kCounterCount = 8192; // ponytail: fixed table, 8 probes; on a full run the entry with the oldest U is overwritten
constexpr int kProbes = 8;
Counter g_counters[kCounterCount];

Counter& FindCounter(u64 site, u64 field, u64 now) {
    u64 h = (site * 0x9E3779B97F4A7C15ull) ^ (field * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29;
    Counter* oldest = nullptr;
    for (int i = 0; i < kProbes; i++) {
        Counter& c = g_counters[(h + i) % kCounterCount];
        if (c.site == site && c.field == field) return c;
        if (c.site == 0) { oldest = &c; break; }
        if (oldest == nullptr || c.U < oldest->U) oldest = &c;
    }
    if (oldest->site != 0 && oldest->U != ~0ull && oldest->U + 1 >= g_N) g_cntEvictLive++;   // IMPL-52: cntEvictLive (entry live in this or the previous update)
    *oldest = {site, field, now, ~0ull, 0, 0, 0}; // U = never: first call counts 1
    return *oldest;
}

Counter* LookCounter(u64 site, u64 field) {   // find only (no insert)
    u64 h = (site * 0x9E3779B97F4A7C15ull) ^ (field * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29;
    for (int i = 0; i < kProbes; i++) { Counter& c = g_counters[(h + i) % kCounterCount]; if (c.site == site && c.field == field) return &c; }
    return nullptr;
}

// IMPL-26 item 0(a): inside an installed async context the clock can be OLDER than the counter (a callback captured at N 7 consumed at N 11 meets a counter the main thread
// visited at N 10). The visit is then computed on a copy: S, U, carry and left of the newer state are never moved backwards. U == ~0 is "never visited".
inline Counter& CounterFor(u64 site, u64 field, u64 n, u64 now, Counter& tmp) {
    Counter& r = FindCounter(site, field, now);
    if (r.U != ~0ull && n < r.U) { tmp = r; return tmp; }
    return r;
}

// Credit the missing (d - 1) ticks of a counter held in W[Reg].
// Sign -1 (countdown) uses the v3.2 floor rule: the extra never takes the counter below 0, the stock step still runs.
//   Before the op (After = false): extra = min((d-1)*K, max(W - K, 0)).  After the op (W already stepped): extra = min((d-1)*K, max(W, 0)).
// d = 0 guard (120 FPS, a held counter runs in an update with no tick): d = 0 would cancel the stock step, so a counter parked on its
//   trigger value fires again next update. If d_eff == 0 and the counter before the op (Wb = After ? W - Sign*K : W) is at the trigger
//   (Sign -1: Wb == 0; Sign +1: ExactL >= 0 and Wb == ExactL), use d = 1 (no debt).
// IMPL-13 step 1: flags of the 32-bit subtraction a - b as NZCV (bits 31..28 of the saved word), and the AArch64 condition test (cond 0 = eq .. 13 = le).
inline u64 NzcvSub(u32 a, u32 b) {
    u32 r = a - b;
    u64 n = r >> 31, z = r == 0, c = a >= b, v = (((a ^ b) & (a ^ r)) >> 31) & 1;
    return (n << 31) | (z << 30) | (c << 29) | (v << 28);
}
inline bool CondHolds(int cond, u64 f) {
    bool n = f >> 31 & 1, z = f >> 30 & 1, c = f >> 29 & 1, v = f >> 28 & 1;
    switch (cond) {
        case 0: return z; case 1: return !z; case 2: return c; case 3: return !c; case 4: return n; case 5: return !n; case 6: return v; case 7: return !v;
        case 8: return c && !z; case 9: return !c || z; case 10: return n == v; case 11: return n != v; case 12: return !z && n == v; default: return z || n != v;
    }
}
constexpr u32 kFlagBig = 1u << 20;   // a stepped value far from the limit: its flags select the non-event side of a countdown test

// GKind (>= 0) = generalised zero-tick flag fix (tools/zero_tick_edge.py): the hook sits behind the FINAL flag producer (GKind 0: the subs of this counter, GImm = K;
// GKind 1: `cmp wD,#GImm`), GCond = the event branch's condition. On a d_eff = 0 visit the old value is stored and the flags are the real flags of that value,
// unless they would take the event side: then the flags of a far-from-limit value are used (the branch takes its NON-event side). With de > 0 the flags are
// those the native producer would give for the value now stored.
template <int Reg, int K, int Sign, bool After, int Base, ptrdiff_t Off, int ExactL, bool Tail, bool ZFix = false, int ZImm = 0, bool Level = false, int GKind = -1, int GCond = 0, bool Rec = false>
inline void Credit(exl::hook::InlineCtx* ctx, uintptr_t site) {
    const Clk clk = ClockNow<Tail>();
    const u64 fld = ctx->X[Base] + Off;
    // IMPL-45: Rec = remember (site, final de) of this visit for the gates below (LastDe); -1 = native / unknown
    auto rec = [&](s64 d) { if constexpr (Rec) { ThreadCtx& t = Thr(); t.lastSite = site; t.lastField = fld; t.lastN = clk.n; t.lastDe = d; t.lastW = static_cast<s32>(ctx->W[Reg]) + (After ? 0 : Sign * K); t.chain = 0; } };   // IMPL-51: lastW = the value the hooked add/sub writes
    if (clk.n == 0) { rec(-1); return; } // before the first timer call: stock behaviour
    const u64 now = clk.now;
    Counter tmp;
    Counter& c = CounterFor(site, fld, clk.n, now, tmp);
    c.left = 0;
    bool held = c.U == clk.n - 1;
    u64 dt = held ? now - c.S : 1;
    if (!held) c.carry = 0; // a gap forgives old debt (new entry = stock)
    // ponytail: workers share the table with no lock; the clamp bounds a raced entry to 4 ticks. Upgrade: per-thread tables or atomics.
    if (dt > 4) dt = 4;
    s32 w = static_cast<s32>(ctx->W[Reg]);
    s32 wb = After ? w - Sign * K : w; // value before the op
    s64 de = static_cast<s64>(dt) + c.carry; // d_eff
    c.carry = 0;
    if (de > 8) { c.carry = de - 8; de = 8; } // d_eff in [0, 8]
    // IMPL-14 step 4: inside the owner 0x1B1A24 after its countdown states handed units to the successor, only the units left (credCap) may be charged here
    if (g_capOn != 0 && ThreadId() == g_logicThread) { const int cap1 = Thr().credCap1; if (cap1 > 0 && de > cap1 - 1) de = cap1 - 1; }
    // crossing: a counter must land exactly on its trigger L, the rest of the credit goes to the next visit
    if (Sign > 0 && ExactL >= 0 && wb < ExactL && wb + de * K > ExactL && (ExactL - wb) % K == 0) {
        s64 dn = (ExactL - wb) / K;
        c.carry += de - dn;
        de = dn;
    }
    if (de > 1) g_dGt1++;   // IMPL-44 diagnostic (after carry / crossing)
    // zero-tick rule (IMPL-8 item 4, tools/zero_tick.py): d_eff = 0 would leave a counter parked on its trigger and the stock branch would fire the
    // event again. At the event value use d = 1 (the native step, which is inert there), with no debt and no negative carry.
    // Shape (b) store-before-branch sites should divert to the no-event continuation instead; that needs a PC diversion the bridge cannot do,
    // so they are listed in impl/zero-tick-manual.tsv and get the same d = 1 meanwhile.
    const bool atEvent = (Sign < 0 && wb == 0) || (Sign > 0 && ExactL >= 0 && wb == ExactL);
    auto gflags = [&](u32 v) { return GKind == 0 ? NzcvSub(v + K, K) : NzcvSub(v, ZImm); };
    if (GKind >= 0 && de == 0) {
        ctx->W[Reg] = static_cast<u32>(wb);
        u64 real = gflags(static_cast<u32>(wb)), good = gflags(kFlagBig);
        ctx->NZCV = (ctx->NZCV & ~0xF0000000ull) | (CondHolds(GCond, real) == CondHolds(GCond, good) ? real : good);
        c.S = now; c.U = clk.n;
        rec(de);
        return;
    }
    if (ZFix && de == 0) {
        // IMPL-9 item 3 (review round 13): after-hook behind the flag producer. A zero-tick visit leaves the old value and the flags of a value that is
        // NOT the event: Z = (old == imm), forced to 0 at the event value, so the native event branch is not taken and the store writes the old value.
        ctx->W[Reg] = static_cast<u32>(wb);
        ctx->NZCV = (ctx->NZCV & ~(1ull << 30)) | ((wb == ZImm && !atEvent) ? (1ull << 30) : 0);
        c.S = now; c.U = clk.n;
        rec(de);
        return;
    }
    if (!ZFix && GKind < 0 && !Level && de == 0 && atEvent) de = 1;   // Level sites (IMPL-10): d = d_eff, 0 allowed
    s64 extra = (de - 1) * K;
    if (Sign < 0 && extra > 0) {
        s64 room = static_cast<s64>(w) - (After ? 0 : K);
        if (room < 0) room = 0;
        if (extra > room) { c.left = (extra - room) / K; c.carry += c.left; extra = room; } // the part removed by the floor is owed, not lost
    }
    // ponytail: carry limited to 8 ticks so a counter pinned on its floor cannot store an unbounded credit
    if (c.carry > 8) c.carry = 8;
    ctx->W[Reg] += Sign * static_cast<int>(extra);
    if (ZFix) ctx->NZCV = (ctx->NZCV & ~(1ull << 30)) | (static_cast<s32>(ctx->W[Reg]) == ZImm ? (1ull << 30) : 0);   // Z matches the value now stored
    if (GKind >= 0) ctx->NZCV = (ctx->NZCV & ~0xF0000000ull) | gflags(ctx->W[Reg]);
    c.S = now;
    c.U = clk.n;
    rec(de);
}

// IMPL-45: LastDe = final de of the last Rec Credit on this thread if it was this site, else -1 (unknown -> native)
// IMPL-46: only if the record is of THIS update (n = clock number of the counter's Tail) and of this object (field; 0 = not known, site + n only). A bypass visit meets an old record -> -1.
// IMPL-47: the record is consumed by the gate read (match or not): one record serves one gate read, a visit that bypasses the counter never sees an older record. A later Rec hook overwrites it (nesting -> mismatch -> native).
template <bool Tail> inline s64 LastDePeek(u64 site, u64 field) {
    const ThreadCtx& t = Thr();
    const u64 n = ClockNow<Tail>().n;
    return (n != 0 && t.lastSite == site && t.lastN == n && (field == 0 || t.lastField == field)) ? t.lastDe : -1;
}
template <bool Tail> inline s64 LastDe(u64 site, u64 field) {
    const s64 d = LastDePeek<Tail>(site, field);
    Thr().lastSite = 0;
    return d;
}

// IMPL-43: unit gates (round 25). PeekUnits = the d_eff Credit WOULD give this key now, no change to the table (Credit's held / dt / carry / caps; ExactL = -1 and Sign +1 sites, so no crossing or atEvent rule).
// Not held, no entry, never visited and an async-older context (n < U: Credit works on a copy of the same values) all give 1 (U != n - 1).
template <bool Tail> inline s64 PeekUnits(uintptr_t site, u64 field) {
    const Clk clk = ClockNow<Tail>();
    if (clk.n == 0) return 1;
    const Counter* c = LookCounter(site, field);
    s64 de = 1;
    if (c != nullptr && c->U == clk.n - 1) {   // held
        u64 dt = clk.now - c->S;
        if (dt > 4) dt = 4;
        de = static_cast<s64>(dt) + c->carry;
        if (de > 8) de = 8;
    }
    if (g_capOn != 0 && ThreadId() == g_logicThread) { const int cap1 = Thr().credCap1; if (cap1 > 0 && de > cap1 - 1) de = cap1 - 1; }
    return de;
}
extern "C" u64 GateNop() { return 0; }   // call gate: x0 = w0 = 0
u64 g_gateShort = 0;   // IMPL-43: visits at the two gates with units >= 2 (30..60 FPS): native, one case per visit, a missed unit (no replay)
// a skipped gated visit is a visit of 0 units: what Credit does at d = 0 (S = now, U = n), else the next update meets a stale entry (not held -> 1 unit) and the gate fires only once per unit
template <bool Tail> inline void SkipVisit(uintptr_t site, u64 field) {
    const Clk clk = ClockNow<Tail>();
    Counter* c = clk.n == 0 ? nullptr : LookCounter(site, field);
    if (c != nullptr && c->U != ~0ull && clk.n >= c->U) { c->S = clk.now; c->U = clk.n; }
}
// 0x19E538 (`blr x10`, parity motion select, counter x27+0xA3C): 0 units -> the dispatch is a no-op, the hook at 0x19E540 then credits 0 and the parity holds
struct Gate19E538 : exl::hook::impl::InlineHook<Gate19E538> {
    static void Callback(exl::hook::InlineCtx* ctx) {
        const s64 u = PeekUnits<IsTail(0x19E540)>(0x19E540, ctx->X[27] + 0xA3C);
        if (u == 0) ctx->X[10] = reinterpret_cast<u64>(&GateNop);
        else if (u >= 2) g_gateShort++;
    }
};
// IMPL-51: 0x47574 (`b.lt 0x47A5C` after `cmp w8,#1`, counter [x28+0x518]): native not taken (w8 >= 1) and 0 units -> force taken (N = 1, V = 0), the whole spawn transaction is skipped
struct GateSpawn47574 : exl::hook::impl::InlineHook<GateSpawn47574> {
    static void Callback(exl::hook::InlineCtx* ctx) {
        if (CondHolds(11, ctx->NZCV)) return;
        if (PeekUnits<IsTail(0x47680)>(0x47680, ctx->X[28] + 0x518) != 0) return;
        ctx->NZCV = (ctx->NZCV & ~(1ull << 28)) | (1ull << 31);
        SkipVisit<IsTail(0x47680)>(0x47680, ctx->X[28] + 0x518);
    }
};
template <bool Tail> inline u64 WholeCrossed();   // IMPL-53: forward declaration (defined below)
// IMPL-53 gate B: 0x3CEEE0 (`b.ge 0x3CF140` after `cmp w8,#1`, counter [x19+0x1D98] decremented LATER at 0x3CF474): native taken (w8 >= 1) and 0 units -> force not taken (N = 1, V = 0), no vtable+0x1C0 post.
// No SkipVisit: 0x3CF474 is still reached and credits 0 itself.
struct GateB3CEEE0 : exl::hook::impl::InlineHook<GateB3CEEE0> {
    static void Callback(exl::hook::InlineCtx* ctx) {
        if (!CondHolds(10, ctx->NZCV)) return;
        if (PeekUnits<IsTail(0x3CF474)>(0x3CF474, ctx->X[19] + 0x1D98) != 0) return;
        ctx->NZCV = (ctx->NZCV & ~(1ull << 28)) | (1ull << 31);
    }
};
// IMPL-54: gate C (0xD4198) removed: its tally (stat 0x7138A8 id 0x23050) is an occurrence posted once natively by the one-shot caller 0x836F8.
// IMPL-55 (v2) stale cached step: writer 0x4423A0 (`str s0,[x21]`, x21 = obj+0x17A40) records {addr, n, factor = stored / g_e0, bits of stored}; consumer 0x442FE0 (after `ldr s0,[x21,#0xC]`)
// uses the value as is when written this update, or when its bits differ from the record (changed elsewhere); else factor x g_e0. 512 slots, linear probing on the full address, never emptied; full: evict smallest n (stepFull).
// IMPL-57 getter record (TailGetter, same update): scaled (token/budget factor d) -> at e0 == 0 f = d (stale = d x e0); const (step does not scale with e0) -> c, stale read = v;
// unknown at e0 == 0 -> z: the stale read keeps the stored value (IMPL-56, counted in stepZeroUnk). e0 > 0 and no const record: f = v / e0.
struct StepEnt { u64 addr; u64 n; float f; u32 vbits; bool z, c; };
StepEnt g_stepTab[512];
inline unsigned StepIdx(u64 a) { return static_cast<unsigned>((a >> 4) ^ (a >> 13) ^ (a >> 22)) & 511u; }
inline u32 StepBits(float v) { u32 b; __builtin_memcpy(&b, &v, 4); return b; }
inline void StepWrite(u64 a, float v) {
    unsigned i = StepIdx(a); StepEnt* e = nullptr;
    for (int k = 0; k < 512; k++, i = (i + 1) & 511u) {
        StepEnt& c = g_stepTab[i];
        if (c.addr == a || c.addr == 0) { e = &c; break; }
    }
    if (!e) {
        e = &g_stepTab[0];
        for (int k = 1; k < 512; k++) if (g_stepTab[k].n < e->n) e = &g_stepTab[k];
        g_stepFull++;
    }
    u8 gm = 0; float gd = 0.0f;
    if (g_getSeq != g_getSeqSeen && g_getN == g_N) { gm = g_getMode; gd = g_getDim; }
    g_getSeqSeen = g_getSeq;
    e->z = false; e->c = false;
    if (gm == 2) { e->c = true; e->f = 1.0f; }
    else if (g_e0 > 0.0f) e->f = v / g_e0;
    else if (gm == 1) e->f = gd;
    else { if (e->addr != a) e->f = 1.0f; e->z = true; g_stepZeroUnk++; }
    e->addr = a; e->n = g_N; e->vbits = StepBits(v);
}
inline float StepRead(u64 a, float v) {
    unsigned i = StepIdx(a);
    for (int k = 0; k < 512; k++, i = (i + 1) & 511u) {
        const StepEnt& e = g_stepTab[i];
        if (e.addr == 0) return v;
        if (e.addr != a) continue;
        if (e.n == g_N || StepBits(v) != e.vbits || e.z || e.c) return v;
        g_stepStale++;
        return e.f * g_e0;
    }
    return v;
}
struct StepWriter4423A0 : exl::hook::impl::InlineHook<StepWriter4423A0> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (ThreadId() == g_logicThread) StepWrite(ctx->X[21], ctx->S[0]); }
};
// IMPL-57: budgets of the global timer (the table the getter 0xC1F0C0 reads). 0xC1F794 (after the last budget store of 0xC1F72C, x0 = timer): D_k = prod_{j<=k} f_j x g_k.
// 0xC1F35C (timer init 0xC1F2F0, x19 = timer) and 0xC1F4D0 (reset 0xC1F49C, x0 = timer) store the literal 1.0 into the budgets: const, not scaled by e0.
inline bool IsGlobalTbl(u64 x) { u8* const* pp = *reinterpret_cast<u8* const* const*>(exl::util::modules::GetTargetStart() + 0x24ABBA8); return pp != nullptr && x != 0 && x == reinterpret_cast<u64>(*pp); }
struct BudFill : exl::hook::impl::InlineHook<BudFill> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (!IsGlobalTbl(ctx->X[0])) return;
        const u8* t = reinterpret_cast<const u8*>(ctx->X[0]); float p = 1.0f;
        for (int k = 0; k < 5; k++) { p *= *reinterpret_cast<const float*>(t + 0x88 + 16 * k); g_dimBud[k] = p * *reinterpret_cast<const float*>(t + 0x8C + 16 * k); }
        g_dimBudOk = true; g_budConst = false;
    }
};
inline void BudOne(u64 x) { if (!IsGlobalTbl(x)) return; for (int k = 0; k < 5; k++) g_dimBud[k] = 1.0f; g_dimBudOk = true; g_budConst = true; }
struct BudReset35C : exl::hook::impl::InlineHook<BudReset35C> { static void Callback(exl::hook::InlineFloatCtx* ctx) { BudOne(ctx->X[19]); } };
struct BudReset4D0 : exl::hook::impl::InlineHook<BudReset4D0> { static void Callback(exl::hook::InlineFloatCtx* ctx) { BudOne(ctx->X[0]); } };
struct StepReader442FE0 : exl::hook::impl::InlineHook<StepReader442FE0> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (ThreadId() == g_logicThread) ctx->S[0] = StepRead(ctx->X[21] + 0xC, ctx->S[0]); }
};
// 0xD6540 (`b.hi 0xD5DEC` after `cmp w8,#4`, stage machine [x21]): stages 0..4 (W[8] <= 4) with 0 units -> force the branch taken (C = 1, Z = 0), the whole case is skipped
struct GateD6540 : exl::hook::impl::InlineHook<GateD6540> {
    static void Callback(exl::hook::InlineCtx* ctx) {
        if (static_cast<u32>(ctx->W[8]) > 4) return;   // IMPL-44: every case 0..4 shares the hooked +1 at 0xD6EB0, gate them all
        const s64 u = PeekUnits<IsTail(0xD6EB0)>(0xD6EB0, ctx->X[21]);
        if (u == 0) { ctx->NZCV = (ctx->NZCV & ~(1ull << 30)) | (1ull << 29); SkipVisit<IsTail(0xD6EB0)>(0xD6EB0, ctx->X[21]); }
        else {
            if (u >= 2) g_gateShort++;
            if (ctx->W[8] == 4) SkipVisit<IsTail(0xD6EB0)>(0xD6EB0, ctx->X[21]);   // IMPL-45: stage 4 executes and resets at 0xD6EFC without Credit: record the visit (S = now, U = n) like Credit, else stage 0 runs one update early
        }
    }
};

// IMPL-45 gates: skip the per-unit action after a Rec counter that gave d = 0 (held, no whole unit). d = 1 native.
// FLAGS: at the b.cond after the counter, force the skip side (b.lt taken: N = 1, V = 0; b.ne taken: Z = 0); other bits stay
template <u64 Site, bool Lt, int Base, ptrdiff_t Off> struct FlagGate : exl::hook::impl::InlineHook<FlagGate<Site, Lt, Base, Off>> {
    static void Callback(exl::hook::InlineCtx* ctx) {
        if (LastDe<IsTail(Site)>(Site, ctx->X[Base] + Off) != 0) return;
        if (Lt) ctx->NZCV = (ctx->NZCV & ~(1ull << 28)) | (1ull << 31); else ctx->NZCV &= ~(1ull << 30);
    }
};
// IMPL-49 chain gates (finding-21): all gates of one visit of a shared key on a d = 0 visit. The FIRST gate reads LastDe (consuming) and arms Id; a later gate (Acc = bit mask of
// accepted ids) forces its branch only while armed; the LAST gate clears the flag. Any Rec Credit clears it too (a new record = a new visit). Hi: b.hi taken (C = 1, Z = 0), else N = 1, V = 0 (b.lt taken / b.ge not taken).
template <u64 Key, int Base, ptrdiff_t Off, int Id, u32 Acc, bool First, bool Last, bool Hi> struct ChainGate : exl::hook::impl::InlineHook<ChainGate<Key, Base, Off, Id, Acc, First, Last, Hi>> {
    static void Callback(exl::hook::InlineCtx* ctx) {
        ThreadCtx& t = Thr();
        if (First) {
            // IMPL-51: arm only when (a) the real flags take the ACTION side (we change the branch) and (b) the compared w8 is the value the counter hook stored (no independent clamp write in between)
            if (LastDe<IsTail(Key)>(Key, ctx->X[Base] + Off) != 0) return;
            if (CondHolds(Hi ? 8 : 11, ctx->NZCV)) return;
            if (static_cast<s32>(ctx->W[8]) != t.lastW) return;
            t.chain = Id;
        }
        else if (!(Acc >> t.chain & 1)) return;
        if (Hi) ctx->NZCV = (ctx->NZCV & ~(1ull << 30)) | (1ull << 29); else ctx->NZCV = (ctx->NZCV & ~(1ull << 28)) | (1ull << 31);
        if (Last) t.chain = 0;
    }
};
// CALL 0xCD86C `blr x8`: the counter that ran on this path is 0xCD7D8 or 0xCD828 (lastSite says which)
struct CallGateCD86C : exl::hook::impl::InlineHook<CallGateCD86C> {
    static void Callback(exl::hook::InlineCtx* ctx) {
        const bool z = LastDePeek<IsTail(0xCD7D8)>(0xCD7D8, ctx->X[22]) == 0 || LastDePeek<IsTail(0xCD828)>(0xCD828, ctx->X[22]) == 0;
        Thr().lastSite = 0;
        if (z) ctx->X[8] = reinterpret_cast<u64>(&GateNop);
    }
};
// REG 0x5DD530 `cbnz x0`: x0 = 0 falls through; 0x786794 `cbz w0`: w0 = 0 is taken
struct RegGate5DD530 : exl::hook::impl::InlineHook<RegGate5DD530> {
    static void Callback(exl::hook::InlineCtx* ctx) { if (LastDe<IsTail(0x5DD50C)>(0x5DD50C, ctx->X[19] + 7708) == 0) ctx->X[0] = 0; }
};

// IMPL-39: 0x10AEB8 (counter [x21+0xA3C], float guard timer [x21+0xA14], debit [x21+0x44]). Native unit: guard timer > 0, timer -= debit, counter >= 1, counter -= 1, ONE submit (bl 0x98AAB0 at 0x10AF04), terminal 0x98AB50 if counter <= 0.
// 0x98AAB0 writes ONE mailbox (consumer 0x990B34..0x990B84 reads and clears it between native units), so a second submit in the same update is dropped.
// d = 0: the gate on 0x98AAB0 skips the submit. d = 1: native. d >= 2: unit 1 native (timer already debited at 0x10AE78); unit 2 runs only if the timer is still > 0 and the counter is still >= 1:
// IMPL-40: split debit (see inside), one submit with the last unit's arguments.
inline int UnitBudgets(u8* owner, float* b, float* r);
// IMPL-42: d of this update at 0x10AEB8 = the whole units the scoped clock crossed in THIS update (WholeCrossed: G - Gp, or the pinned context's cG - cGp), never the counter's visit history
// (a previous update that bypassed 0x10AEB8 left a stale counter entry). Capped at 8 and by the credit cap as Credit does. The counter table is not touched.
template <bool Tail> inline u64 WholeCrossed();
inline s64 PeekD(u64) {
    const Clk clk = ClockNow<IsTail(0x10AEB8)>();
    if (clk.n == 0) return 1;
    s64 de = static_cast<s64>(WholeCrossed<IsTail(0x10AEB8)>());
    if (de > 8) de = 8;
    if (g_capOn != 0 && ThreadId() == g_logicThread) { const int cap1 = Thr().credCap1; if (cap1 > 0 && de > cap1 - 1) de = cap1 - 1; }
    return de;
}
// IMPL-42: the false-predicate path (0x10B108, reached from 0x10AE8C / 0x10AE9C / 0x10AEAC) submits a fallback packet at 0x10B14C with the UNSTEPPED counter and no step; it is the same call 0x98AAB0.
// Unit 2 on that path would submit the identical packet again (single mailbox: the last request wins, same content), so nothing extra is done for unit 2 there; only the timer debit applies.
// IMPL-41 hook A, BEFORE the native fsub at 0x10AE6C (s0 = timer, s1 = whole-update step S = [x21+0x44]); the guard at 0x10AE5C..64 already passed (unit 1 is running).
// d >= 2: the native fsub subtracts ONLY unit 1's budget b1. Budgets: UnitBudgets(x19) (the actor the float hooks use; x21 is not proven to be a UnitBudgets owner), b1 = S * ub[0] / sum(ub),
// S/2 when no usable budget list. Unit 2's debit b2 = S - b1 is applied at the exit 0x10B150 (every path), its timer guard is decided here: (T - b1) > 0, as native.
// RESIDUAL: 0x74D490 calls vtable +0xF8 (0x74D558); it is not proven not to read [x21+0xA14]; unit 2's predicates (0x74D490 result, 0x55BD94) reuse unit 1's results.
struct TimerPlan : exl::hook::impl::InlineHook<TimerPlan> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        t.pArmed = false;
        const float T = ctx->S[0], S = ctx->S[1];
        if (!(S > 0.0f) || PeekD(ctx->X[21] + 0xA3C) < 2) return;
        float ub[17], ur[17]; const int n = UnitBudgets(reinterpret_cast<u8*>(ctx->X[19]), ub, ur);
        float tot = 0.0f; for (int i = 0; i < n; i++) tot += ub[i];
        float b1 = (n >= 1 && tot > 0.0f && ub[0] >= 0.0f) ? S * (ub[0] / tot) : S * 0.5f;
        if (!(b1 >= 0.0f)) b1 = S * 0.5f;
        if (b1 > S) b1 = S;
        ctx->S[1] = b1;
        t.pArmed = true; t.pStepped = false; t.pOwner = ctx->X[21]; t.pB2 = S - b1; t.pGuard2 = (T - b1) > 0.0f;
    }
};
// IMPL-41 exit hook at 0x10B150 (the common exit of 0x10AE68's function: counter < 1, the 0x74D490 / 0x55BD94 bypasses, the normal path and the terminal all arrive here).
struct TimerExit : exl::hook::impl::InlineHook<TimerExit> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        if (!t.pArmed || t.pOwner != ctx->X[21]) return;
        t.pArmed = false;
        if (t.pGuard2) *reinterpret_cast<float*>(ctx->X[21] + 0xA14) -= t.pB2;
    }
};
// IMPL-41 at 0x10AEB8 (after `sub w9,w8,#1`, w8 = counter, w9 = counter - 1). Armed 2-unit update: unit 1 credits exactly 1 (the counter table is touched, no extra); if unit 2 is eligible
// (timer guard decided at hook A, counter - 1 >= 1) the counter steps twice and the ONE submit carries unit 2's request (w8 = old - 1 feeds 0x10AEC0/0x10AEC8). d = 0: the gate skips the submit.
int g_submitDropped = 0;
template <bool Tail>
inline void SubmitNote(exl::hook::InlineCtx* ctx) {
    ThreadCtx& t = Thr();
    const s32 old = static_cast<s32>(ctx->W[8]);
    if (t.pArmed && t.pOwner == ctx->X[21]) {
        t.subOn = false; t.pStepped = true;
        if (t.pGuard2 && old - 1 >= 1) { ctx->W[9] = static_cast<u32>(old - 2); ctx->W[8] = static_cast<u32>(old - 1); g_submitDropped++; }
        return;
    }
    t.subOn = old - static_cast<s32>(ctx->W[9]) == 0;
}

// IMPL-48: keyed variant of IntHook (Rec on): hook at Addr, counter key / Rec site = KeySite (an alternate writer of a counter that a hook at KeySite also steps).
template <uintptr_t Addr, uintptr_t KeySite, int Reg, int K, int Sign, int Base, ptrdiff_t Off, int ExactL = -1, bool Rec = true>
struct IntHookKey : exl::hook::impl::InlineHook<IntHookKey<Addr, KeySite, Reg, K, Sign, Base, Off, ExactL, Rec>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        Credit<Reg, K, Sign, false, Base, Off, ExactL, IsTail(KeySite), false, 0, IsLevel(KeySite), -1, 0, Rec>(ctx, KeySite);   // IMPL-50: Rec = false for HOOK_INT_KEY (no gate reads it)
    }
};

// Inline hooks use the float-saving bridge (InlineFloatCtx): the C++ callback may use SIMD registers the game still needs.
// Inline hook BEFORE `add/sub/subs wD, wReg, #K` (D == Reg, or D != Reg when wReg is not read again); the field is [X[Base] + Off].
// MainOnly (IMPL-8 item 8): on any thread other than the logic thread the hook does nothing, i.e. the native unit runs.
template <uintptr_t Addr, int Reg, int K, int Sign, int Base, ptrdiff_t Off, int ExactL = -1, bool MainOnly = false, bool Rec = false>
struct IntHook : exl::hook::impl::InlineHook<IntHook<Addr, Reg, K, Sign, Base, Off, ExactL, MainOnly, Rec>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (MainOnly && ThreadId() != g_logicThread) return;
        // IMPL-38 item 1: 0x56140 counts up to 100, and 100 -> 101 is a one-time STATE change (reader 0x55FE4: 101 goes to the timer, other values above 100 skip it). At >= 100 the native +1 runs.
        if constexpr (Addr == 0x56140) { if (static_cast<s32>(ctx->W[Reg]) >= 100) return; }
        Credit<Reg, K, Sign, false, Base, Off, ExactL, IsTail(Addr), false, 0, IsLevel(Addr), -1, 0, Rec>(ctx, Addr);
        if constexpr (Addr == 0x56140) { if (static_cast<s32>(ctx->W[Reg]) > 99) ctx->W[Reg] = 99; }   // the result (W + 1) never goes past exactly 100
    }
};

// Inline hook on the instruction AFTER `add/sub/subs wReg, wS, #K` (D != S): wReg already holds the stepped value.
template <uintptr_t Addr, int Reg, int K, int Sign, int Base, ptrdiff_t Off, int ExactL = -1, bool Rec = false>
struct IntHookAfter : exl::hook::impl::InlineHook<IntHookAfter<Addr, Reg, K, Sign, Base, Off, ExactL, Rec>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if constexpr (Addr == 0x10AEB8) {   // IMPL-41: armed update = unit 1 credits exactly 1
            const ThreadCtx& t = Thr();
            if (t.pArmed && t.pOwner == ctx->X[21]) {
                const Clk clk = ClockNow<IsTail(Addr)>(); Counter tmp;
                Counter& c = CounterFor(Addr, ctx->X[Base] + Off, clk.n, clk.now, tmp);
                c.left = 0; c.carry = 0; c.S = clk.now; c.U = clk.n;   // ponytail: units above 2 are dropped, no carry
                SubmitNote<IsTail(Addr)>(ctx);
                return;
            }
        }
        Credit<Reg, K, Sign, true, Base, Off, ExactL, IsTail(Addr), false, 0, IsLevel(Addr), -1, 0, Rec>(ctx, Addr);
        if constexpr (Addr == 0x10AEB8) SubmitNote<IsTail(Addr)>(ctx);
    }
};

// IMPL-9 item 3: zero-tick fix for store-before-branch counters (tools/zero_tick_z.py). Hook at Addr = the instruction after the final flag producer
// (the relocated instruction is the store); Reg holds the stepped value, NZCV in the context is changed (bridge restores it from the context).
template <uintptr_t Addr, int Reg, int K, int Sign, int Base, ptrdiff_t Off, int ExactL, int ZImm>
struct IntHookZ : exl::hook::impl::InlineHook<IntHookZ<Addr, Reg, K, Sign, Base, Off, ExactL, ZImm>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { Credit<Reg, K, Sign, true, Base, Off, ExactL, IsTail(Addr), true, ZImm>(ctx, Addr); }
};

template <uintptr_t Addr, int Reg, int K, int Sign, int Base, ptrdiff_t Off, int ExactL, int PKind, int PImm, int Cond>
struct IntHookG : exl::hook::impl::InlineHook<IntHookG<Addr, Reg, K, Sign, Base, Off, ExactL, PKind, PImm, Cond>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { Credit<Reg, K, Sign, true, Base, Off, ExactL, IsTail(Addr), false, PImm, false, PKind, Cond>(ctx, Addr); }
};

// C6 smart float (ARCHITECTURE C6 v3.2). f = g_step if this site also ran in the previous update (held), else 1.0 (event = stock).
// f is not clamped: g_step is already in [0, kMaxStep]. Same table and key style as the integer hooks (site = hook address).
template <bool Tail>
inline float StepFactor(u64 site, u64 field) {
    const Clk clk = ClockNow<Tail>();
    if (clk.n == 0) return 1.0f;   // captured before the first timer call
    Counter tmp;
    Counter& c = CounterFor(site, field, clk.n, clk.now, tmp);
    float f = (c.U == clk.n - 1) ? clk.step : 1.0f;
    c.S = clk.now;
    c.U = clk.n;
    { const ThreadCtx& t = Thr(); if (t.ovOn) f = t.ovRaw; }   // IMPL-29 item 4: inside a scoped replay unit the float hooks use that unit's raw budget (as RawStepGetter does)
    if (g_segOn != 0 && (site == 0x814228 || site == 0x814260) && ThreadId() == g_logicThread) { const ThreadCtx& t = Thr(); if (t.segOn) f = t.segF; }   // IMPL-28: the segment's raw length
    return f;
}

// IMPL-23: the factor StepFactor WOULD give for (site, field), without creating or changing the counter (a read that runs before the site's own update hook).
template <bool Tail>
inline float PeekStep(u64 site, u64 field) {
    Counter* c = LookCounter(site, field);
    const Clk clk = ClockNow<Tail>();
    { const ThreadCtx& t = Thr(); if (t.ovOn) return t.ovRaw; }   // IMPL-29 item 4
    return (c != nullptr && clk.n != 0 && c->U == clk.n - 1) ? clk.step : 1.0f;
}

// Shape A: hook AFTER `fadd/fsub sD, sN, sC` (Addr = the next instruction). S[D] += Sign * c * (f - 1). Field = [X[Base] + Off].
template <uintptr_t Addr, int D, u32 CBits, int Sign, int Base, ptrdiff_t Off>
struct FAddHook : exl::hook::impl::InlineHook<FAddHook<Addr, D, CBits, Sign, Base, Off>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        float f = StepFactor<IsTail(Addr)>(Addr, ctx->X[Base] + Off);
        ctx->S[D] += Sign * std::bit_cast<float>(CBits) * (f - 1.0f);
    }
};

// Shape B: hook BEFORE the multiply; the factor register S[Reg] (value a) becomes 1-(1-a)^f (Kind 0) or a^f (Kind 1).
template <uintptr_t Addr, int Reg, u32 ABits, int Kind, int Base, ptrdiff_t Off>
struct FSmoothHook : exl::hook::impl::InlineHook<FSmoothHook<Addr, Reg, ABits, Kind, Base, Off>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        float f = StepFactor<IsTail(Addr)>(Addr, ctx->X[Base] + Off);
        float a = std::bit_cast<float>(ABits);
        ctx->S[Reg] = Kind == 0 ? 1.0f - __builtin_powf(1.0f - a, f) : __builtin_powf(a, f);
    }
};

// IMPL-21 (fix review 2). Closed form of a per-call smoothing x' = A + (x-A)*a (native a, f = StepFactor): x' = A + (x-A)*a^f.
// Hook BEFORE the final `fadd` of the native step. x is [X[Base]+Off] (FromMem) or the register S[RX]. The result goes to S[RX], S[RZ] becomes 0,
// so the native fadd stores result + 0. f == 1 (stock or event): native, bit exact. f == 0: x unchanged.
template <uintptr_t Addr, int RX, int RZ, u32 ABits, u32 TBits, int Base, ptrdiff_t Off, bool FromMem>
struct FClosed : exl::hook::impl::InlineHook<FClosed<Addr, RX, RZ, ABits, TBits, Base, Off, FromMem>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const u64 fld = ctx->X[Base] + Off;
        float f = StepFactor<IsTail(Addr)>(Addr, fld);
        if (f == 1.0f) return;
        const float a = std::bit_cast<float>(ABits), A = std::bit_cast<float>(TBits);
        const float x = FromMem ? *reinterpret_cast<float*>(fld) : ctx->S[RX];
        ctx->S[RX] = A + (x - A) * __builtin_powf(a, f);
        ctx->S[RZ] = 0.0f;
    }
};

// IMPL-27 shared pieces, IMPL-30 fractional-unit framework. (1) Anchor store: one table keyed (site, owner) with a few floats and the update number of the last touch.
// An anchor = the state at the last whole-unit boundary + the progress u in [0,1) + what the next unit needs. The lookup probes the WHOLE table (linear, 4096 slots); only a stale entry
// (not written for >= 2 updates, a.n + 2 <= g_N) or a free slot may be replaced. Truly full: AnchorOpen counts it (g_anchorFull) and gives the caller a scratch anchor that is not
// stored. IMPL-32 item 1: that visit is a stateless WHOLE-UNIT visit (Anchor::whole): the owner commits exactly the whole units the global clock crossed this frame (WholeCrossed), shows no
// partial. Speed stays right (the budgets are scaled to the crossed units), only the smoothness is lost. A slot that frees later is taken at the next visit with anchor = the current state
// and u = the global phase AFTER the visit (IMPL-34 item 1: LateStart / PhaseAfter, g_anchorLate).
// ponytail: 4096 entries, no lock (workers race at worst into a re-anchor); upgrade: per-site tables.
struct Anchor { u64 site, owner, n; float v[10]; bool active; bool whole; };   // whole = the table was full: this visit is a stateless WHOLE-UNIT visit (IMPL-32 item 1)
constexpr int kAnchorCount = 4096;
Anchor g_anchor[kAnchorCount];
u32 g_anchorFull = 0, g_anchorLate = 0;
template <bool Tail> inline u64 WholeCrossed();      // defined after the publication nodes
template <bool Tail> inline double PhaseBefore();    // idem
inline u64 AnchorHash(u64 site, u64 owner) { u64 h = (site * 0x9E3779B97F4A7C15ull) ^ (owner * 0xC2B2AE3D27D4EB4Full); return h ^ (h >> 29); }
Anchor* AnchorGet(u64 site, u64 owner) {
    const u64 h = AnchorHash(site, owner);
    Anchor* pick = nullptr;
    for (int i = 0; i < kAnchorCount; i++) {
        Anchor& a = g_anchor[(h + i) % kAnchorCount];
        if (a.site == site && a.owner == owner) return &a;
        if (a.site == 0) { if (pick == nullptr || pick->site != 0) pick = &a; break; }
        if (a.n + 2 <= g_N && (pick == nullptr || a.n < pick->n)) pick = &a;   // stale only
    }
    if (pick == nullptr) { g_anchorFull++; return nullptr; }
    *pick = {};
    pick->site = site; pick->owner = owner;
    return pick;
}
Anchor* AnchorLook(u64 site, u64 owner) {
    const u64 h = AnchorHash(site, owner);
    for (int i = 0; i < kAnchorCount; i++) { Anchor& a = g_anchor[(h + i) % kAnchorCount]; if (a.site == site && a.owner == owner) return &a; if (a.site == 0) break; }
    return nullptr;
}
// The anchor of this visit: the stored one, or `spill` (zeroed, not stored) when the table is truly full.
inline Anchor* AnchorOpen(u64 site, u64 owner, Anchor& spill) {
    Anchor* st = AnchorGet(site, owner);
    if (st != nullptr) return st;
    spill = {}; spill.site = site; spill.owner = owner; spill.whole = true;
    return &spill;
}
// IMPL-34 item 1 (fix review 10): an owner with NO valid anchor that gets a slot after the table was full (a "late start") first commits the whole units the clock crossed this visit
// (WholeCrossed, exactly as the table-full path), THEN the anchor starts at that state with u = the global phase AFTER the visit (PhaseAfter) and shows the partial of u. The partial before
// the last boundary was never shown and is not owed. Each user: whole walk of W units (budget scaled W / f), then a second walk of the piece {PhaseAfter} (g_anchorLate counts).
template <bool Tail> inline double PhaseAfter();   // IMPL-35 item 3: defined after the publication nodes (an installed context uses its captured phase after)
inline bool LateStart(bool valid, bool whole, float f) {
    if (valid || whole || g_anchorFull == 0 || f == 1.0f) return false;
    g_anchorLate++; return true;
}
// (3) The walker. Progress u in [0,1) of the unit in progress and bAcc = the budget the unit in progress has consumed. Each piece {len = progress length, bud = budget} is added in order;
// every time u reaches 1 (within 1e-6) the unit is committed: onUnit(bAcc) with the budget the whole unit consumed (parts from earlier updates included), then u = 0, bAcc = 0.
// Committed work is never walked again: a change of an input only reaches the pieces that come after it.
template <class OnUnit>
inline void FracWalk(double& u, double& bAcc, const double* len, const double* bud, int n, OnUnit onUnit) {
    for (int i = 0; i < n; i++) {
        double left = len[i];
        if (!(left > 0.0)) { bAcc += bud[i]; continue; }   // no progress: the budget joins the unit in progress
        const double bpl = bud[i] / len[i];
        for (int guard = 0; left > 0.0 && guard < 64; guard++) {
            const double room = 1.0 - u, take = left < room ? left : room;
            bAcc += bpl * take; u += take; left -= take;
            if (u >= 1.0 - 1.0e-6) { onUnit(bAcc); u = 0.0; bAcc = 0.0; }
        }
    }
}
inline bool SameBits(float a, float b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
// (2) UnitBudgets: the ordered per-unit effective budgets b[] and raw durations r[] of this update for `owner` (whole units, then the fractional rest; ComputeUnitSteps u[k], r[k]);
// units with no time and no budget are left out. Returns the count (<= 17). Defined after ComputeUnitSteps.
inline int UnitBudgets(u8* owner, float* b, float* r);

// IMPL-23 item 1 (fix review 3) / IMPL-30 item 1 (fix review 6): 0x2BD90 gain g' = 0.85 g + 0.15 and the movement at 0x2BDAC..0x2BDB8 (pos += V dt g'; dt already carries f).
// Fractional-unit framework, anchor per (0x2BD90, x19): v[0] = gain ga at the last whole-unit boundary, v[1] = progress u, v[2] = the gain reported to the field, v[3] = displacement
// (in gain x budget) applied for the unit in progress minus the committed work not yet applied, v[4] = budget bAcc the unit in progress has consumed. Each update walks its pieces
// (UnitBudgets: budget b, raw r; r scaled so that they add up to f) from the anchor: every whole unit crossed commits g_unit = 0.85 ga + 0.15 (native expression) with that unit's own
// budget; the displacement of a unit is g_unit x budget. The gain reported for the rest is 1 + (ga - 1) 0.85^u (from the anchor, never stacked); the displacement of the rest is bAcc x that
// gain. GainClosed computes all this and stashes the movement factor delta / sum(b) (delta = committed + new partial - old partial); GainMove (before `fmul v1,v2,v1.s[0]` at 0x2BDAC)
// replaces the gain factor by it, so V dt factor = V delta. f == 1 with u == 0, or g_N == 0: native. Table full: the owner is anchored at its field (u = 0) for this visit.
template <uintptr_t Addr>
struct GainClosed : exl::hook::impl::InlineHook<GainClosed<Addr>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const u64 fld = ctx->X[19] + 0x1D04;
        ThreadCtx& t = Thr();
        t.stN = 0;
        const float f = StepFactor<IsTail(Addr)>(Addr, fld);
        const float x = *reinterpret_cast<float*>(fld);
        Anchor spill; Anchor* st = AnchorOpen(Addr, ctx->X[19], spill);
        const bool valid = st->n != 0 && st->n == g_N - 1 && SameBits(st->v[2], x);
        float ga = valid ? st->v[0] : x;
        double u = valid ? st->v[1] : 0.0, bAcc = valid ? st->v[4] : 0.0;
        const bool late = LateStart(valid, st->whole, f);
        const double pOld = valid ? st->v[3] : 0.0;
        // IMPL-31 item 4 (review doubt on IMPL-30): an update with zero budget leaves the displacement of a committed unit OWED (kept = pOld - committed != 0). The native early return is
        // taken only when nothing is owed and no fraction is active; otherwise this update pays the owed amount (committed + pNew - pOld).
        if (f == 1.0f && u == 0.0 && pOld == 0.0) { st->active = false; st->n = 0; return; }
        constexpr float kA = std::bit_cast<float>(0x3f59999au), kB = std::bit_cast<float>(0x3e199998u);
        // IMPL-32 item 2 (fix review 8): the budgets are in the MOVEMENT domain: dt = the movement slot [x19+0x1748] the native fmul uses; each piece gets dt x its share of this update
        // (UnitBudgets b_j / sum b, else its raw share), so the displacement of a unit = g_unit x (the movement budgets of ALL its visits), and the factor handed to the native multiply is delta / dt
        // (native applies V dt fac = V delta). The previous partial was applied in the same domain, so it is subtracted exactly. IMPL-32 item 1: a whole-unit visit walks W units (budgets scaled W / f).
        const float dt = *reinterpret_cast<float*>(ctx->X[19] + 0x1748);
        const bool wh = st->whole || late;
        const double fw = wh ? static_cast<double>(WholeCrossed<IsTail(Addr)>()) : static_cast<double>(f), sf = wh ? (f > 0.0f ? fw / f : 0.0) : 1.0;
        float ub[17], ur[17]; const int n = UnitBudgets(reinterpret_cast<u8*>(ctx->X[19]), ub, ur);
        double len[18], bud[18]; int m = 0; double sr = 0.0, sb = 0.0;
        for (int j = 0; j < n; j++) { sr += ur[j]; sb += ub[j]; }
        if (n > 0 && sr > 0.0) { for (int j = 0; j < n; j++) { len[m] = ur[j] * (fw / sr); bud[m] = static_cast<double>(dt) * (sb > 0.0 ? ub[j] / sb : ur[j] / sr) * sf; m++; } }
        else { len[0] = fw; bud[0] = static_cast<double>(dt) * sf; m = 1; }
        double committed = 0.0;
        auto onUnit = [&](double B) { ga = ga * kA + kB; committed += static_cast<double>(ga) * B; };
        FracWalk(u, bAcc, len, bud, m, onUnit);
        if (late) { const double l2 = PhaseAfter<IsTail(Addr)>(), b2 = static_cast<double>(dt) * (f > 0.0f ? l2 / f : 0.0); FracWalk(u, bAcc, &l2, &b2, 1, onUnit); }   // IMPL-34 item 1
        const double gpart = u > 0.0 ? 1.0 + (static_cast<double>(ga) - 1.0) * __builtin_pow(static_cast<double>(kA), u) : static_cast<double>(ga);
        const float rep = u > 0.0 ? static_cast<float>(gpart) : ga;
        const double pNew = u > 0.0 ? bAcc * gpart : 0.0;
        float fac = 0.0f; double kept = pNew;
        if (dt > 0.0f) fac = static_cast<float>((committed + pNew - pOld) / dt); else kept = pOld - committed;   // no movement budget this update: the work stays owed
        st->v[0] = ga; st->v[1] = static_cast<float>(u); st->v[2] = rep; st->v[3] = static_cast<float>(kept); st->v[4] = static_cast<float>(bAcc); st->n = g_N; st->active = u > 0.0;
        t.stOwner = ctx->X[19]; t.stN = g_N; t.stF = f; t.stFac = fac;
        ctx->S[0] = rep;
        ctx->S[1] = 0.0f;
    }
};
template <uintptr_t Addr>
struct GainMove : exl::hook::impl::InlineHook<GainMove<Addr>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        ThreadCtx& t = Thr();
        if (t.stN != g_N || t.stOwner != ctx->X[19]) return;
        t.stN = 0;
        ctx->S[1] = t.stFac;
    }
};
// IMPL-27 item 2: 0x385E44 reads the gain BEFORE the update at 0x385F68 in the same call, so the native output uses the gain at the start of the unit in progress. Anchor per
// (0x385F68, x22): v[0] = gain at the last whole-unit boundary (ga), v[1] = fractional progress u, v[2] = the gain reported to the field. The update (GainAnchor) advances t = u + f
// units from ga and reports 1 + (ga-1) 0.95^t. The output (GainPrev, a peek) uses the gain after k units when a fraction is in progress (uu > 0), after k-1 units when t is whole
// (k = floor t), never below the anchor: f == 1, u == 0 is the native read. State is valid only if the previous update wrote it and the field still holds the reported value.
constexpr uintptr_t kGainPrevSite = 0x385F68;
// IMPL-34 item 2: true when this (site, owner) has no anchor and the table has no slot for it (the update at 0x385F68 runs in WHOLE-UNIT mode): the outputs then stay native (field only).
inline bool AnchorWouldBeFull(u64 site, u64 owner) {
    if (g_anchorFull == 0 || AnchorLook(site, owner) != nullptr) return false;
    const u64 h = AnchorHash(site, owner);
    for (int i = 0; i < kAnchorCount; i++) { const Anchor& a = g_anchor[(h + i) % kAnchorCount]; if (a.site == 0 || a.n + 2 <= g_N) return false; }
    return true;
}
inline bool GainAnchorRead(u64 owner, float field, float& ga, float& u) {
    const Anchor* st = AnchorLook(kGainPrevSite, owner);
    if (st != nullptr && st->n != 0 && st->n == g_N - 1 && SameBits(st->v[2], field)) { ga = st->v[0]; u = st->v[1]; return true; }
    ga = field; u = 0.0f; return false;
}
// IMPL-30 item 2: the second reader `ldp s1,s2,[x22,#-56]` at 0x385E60 (gain in s2, used by `fmul s1,s0,s2` at 0x385E68, stored at 0x385E70) reads the gain too, so both readers use
// the anchored output gain: Reg = 1 for 0x385E48, Reg = 2 for 0x385E68. (example: gain 0, two half ticks -> 0x385E70 stores 1.0, not 1.0253206; the test harness sets [sp+0x24] = 0
// only to stop the LATER overwrite at 0x386324 from hiding that store; the module does not touch the stack.)
template <uintptr_t Addr, uintptr_t KeySite, int Reg = 1>
struct GainPrev : exl::hook::impl::InlineHook<GainPrev<Addr, KeySite, Reg>> {   // before `fmul s3,s0,s1` at 0x385E48 / `fmul s1,s0,s2` at 0x385E68 (S[Reg] = gain, x22 as at 0x385F68)
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const float f = PeekStep<IsTail(KeySite)>(KeySite, ctx->X[22] - 0x34);
        if (AnchorWouldBeFull(kGainPrevSite, ctx->X[22])) return;   // IMPL-34 item 2: whole-unit mode (table full): native output for the committed units
        float ga, u; GainAnchorRead(ctx->X[22], ctx->S[Reg], ga, u);
        if (f == 1.0f && u == 0.0f) return;
        const float t = u + f, k = __builtin_floorf(t);
        const float kk = (t - k) > 0.0f ? k : (k > 1.0f ? k - 1.0f : 0.0f);
        ctx->S[Reg] = 1.0f + (ga - 1.0f) * __builtin_powf(0.949999988f, kk);
    }
};
template <uintptr_t Addr>
struct GainAnchor : exl::hook::impl::InlineHook<GainAnchor<Addr>> {   // before `fadd s0,s1,s0` at 0x385F68: s1 = gain x, s0 = 0.05 (1-x); x22 = owner, field [x22-0x34]
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const float f = StepFactor<IsTail(Addr)>(Addr, ctx->X[22] - 0x34);
        Anchor spill; Anchor* st = AnchorOpen(Addr, ctx->X[22], spill);
        float ga, u; GainAnchorRead(ctx->X[22], ctx->S[1], ga, u);
        if (f == 1.0f && u == 0.0f) { st->active = false; st->n = 0; return; }
        if (st->whole) {   // IMPL-32 item 1: table full: commit the W whole units the clock crossed, no partial
            const u64 W = WholeCrossed<IsTail(Addr)>();
            if (W != 0) { ctx->S[1] = 1.0f + (ga - 1.0f) * __builtin_powf(0.949999988f, static_cast<float>(W)); ctx->S[0] = 0.0f; }
            else { ctx->S[0] = 0.0f; }
            return;
        }
        const float t = u + f, k = __builtin_floorf(t), uu = t - k;
        const float rep = 1.0f + (ga - 1.0f) * __builtin_powf(0.949999988f, t);
        st->v[0] = 1.0f + (ga - 1.0f) * __builtin_powf(0.949999988f, k); st->v[1] = uu; st->v[2] = rep; st->n = g_N; st->active = uu > 0.0f;
        ctx->S[1] = rep; ctx->S[0] = 0.0f;
    }
};

// IMPL-27 item 4 / IMPL-30 item 4 (0x4E824C, hook BEFORE `fsub s0,s1,s0`: s1 = old, s0 = the branch-selected q): unit replay from an anchor per (0x4E824C, x20): v[0] = x at the
// last whole-unit boundary (xa), v[1] = progress u, v[2] = the value reported to the field, v[3] = c = the decrement the unit in progress has consumed. Each update advances the
// progress by f (FracWalk, budget q per unit of progress): a unit crossed commits xa -= (c + q (1 - u)) = what the unit consumed so far plus the REST at the q selected now (the first unit
// was already admitted by the native gate; each later one re-tests the eligibility x/d >= 0.3, float divide as native), then the fractional rest consumes q u more. Consumed work is never
// recomputed: a new q reaches only the part of the unit that is still to come (example: x 0.4, d 1, q 0.3, half a tick -> 0.25; a zero-length update with q 0.6 -> still 0.25).
// FSubGate (before the native `fcmp s0,s1` at 0x4E8074) replaces the ratio by anchor/d while a fraction is active, so the unit an eligible state started finishes.
// A unit that reaches <= 0 ends the replay. f == 1 with no fraction: native. Table full: anchored at the field (u = 0) for this visit.
constexpr uintptr_t kFSubSite = 0x4E824C;
template <uintptr_t Addr, int Base, ptrdiff_t Off>
struct FSubUnits : exl::hook::impl::InlineHook<FSubUnits<Addr, Base, Off>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const u64 fld = ctx->X[Base] + Off;
        const float f = StepFactor<IsTail(Addr)>(Addr, fld);
        Anchor spill; Anchor* st = AnchorOpen(Addr, ctx->X[Base], spill);
        const float d = *reinterpret_cast<float*>(fld + 4), q = ctx->S[0];
        float xa = ctx->S[1];
        double u = 0.0, bAcc = 0.0;
        const bool valid = st->n != 0 && st->n == g_N - 1 && SameBits(st->v[2], xa);
        if (valid) { xa = st->v[0]; u = st->v[1]; bAcc = st->v[3]; }
        const bool late = LateStart(valid, st->whole, f);
        if (f == 1.0f && u == 0.0) { st->active = false; st->n = 0; return; }
        bool live = true; int units = 0;
        const double fw = (st->whole || late) ? static_cast<double>(WholeCrossed<IsTail(Addr)>()) : static_cast<double>(f);   // IMPL-32 item 1
        const double len = fw, bud = static_cast<double>(q) * fw;
        auto onUnit = [&](double B) {
            if (!live) return;
            if (units > 0 && (xa / d) < 0.300000012f) { live = false; return; }
            units++;
            xa = xa - static_cast<float>(B);
            if (!(xa > 0.0f)) live = false;
        };
        FracWalk(u, bAcc, &len, &bud, 1, onUnit);
        if (late) { const double l2 = PhaseAfter<IsTail(Addr)>(), b2 = static_cast<double>(q) * l2; FracWalk(u, bAcc, &l2, &b2, 1, onUnit); }   // IMPL-34 item 1
        float rep = xa;
        const bool frac = live && u > 0.0 && (units == 0 || !((xa / d) < 0.300000012f));
        if (frac) rep = xa - static_cast<float>(bAcc);
        st->v[0] = xa; st->v[1] = frac ? static_cast<float>(u) : 0.0f; st->v[2] = rep; st->v[3] = frac ? static_cast<float>(bAcc) : 0.0f; st->n = g_N; st->active = frac;
        ctx->S[1] = rep;
        ctx->S[0] = 0.0f;
    }
};
struct FSubGate : exl::hook::impl::InlineHook<FSubGate> {   // 0x4E8074, before `fcmp s0,s1` (s0 = x130 / x134, s1 = 0.3); x20 = owner
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const Anchor* st = AnchorLook(kFSubSite, ctx->X[20]);
        if (st == nullptr || !st->active || st->n == 0 || st->n != g_N - 1) return;
        if (!SameBits(st->v[2], *reinterpret_cast<float*>(ctx->X[20] + 0x130))) return;
        ctx->S[0] = st->v[0] / *reinterpret_cast<float*>(ctx->X[20] + 0x134);
    }
};

// IMPL-23 item 3 / IMPL-27 item 3: 0x94378 / 0x943A8 (one actor, x22): p = min(1, p+0.1), y' = y + p*min(0.3*(18-y), 0.5) for y < 18. Per-owner state in the anchor store (site 0x9437C,
// owner x22): v = {ya, pa, u, lastY, lastP, yrep} = the anchor (ya, pa) at the last whole-unit boundary, the fractional progress u in [0,1), what was reported to the fields, and
// the y reported to 0x943BC. Each update: progress t = u + f; the whole units crossed run from the anchor in order (native expression, bit exact); the reported y / p for the
// fractional rest is the anchor plus u of the NEXT unit's increment (linear), never stacked partial ladders. The state is valid only if the previous update wrote it and the
// fields still hold what it wrote (else re-anchor from the fields). f == 1 with u == 0: native. An entry with u > 0 is active and survives collisions (AnchorGet).
constexpr uintptr_t kRampSite = 0x9437C;
inline float RampG(float y, float p) {   // one whole native tick (same operations as 0x943A8..0x943BC)
    if (y >= 18.0f) return y;
    return y + p * __builtin_fminf((18.0f - y) * 0.300000012f, 0.5f);
}
// IMPL-30 item 3: on the framework (FracWalk, AnchorOpen). Table truly full: the owner is anchored at its fields (u = 0) for this visit and the y for 0x943BC goes through g_rampSpill, so
// the visit is never plain native while a fraction was active (example: p 0.2, y 0, two half ticks -> p 0.3, y 0.15 also when 33 owners share the table).
struct RampSpill { u64 owner, n; float y; };
RampSpill g_rampSpill = {};
template <uintptr_t Addr, int Base, ptrdiff_t Off>
struct RampP : exl::hook::impl::InlineHook<RampP<Addr, Base, Off>> {   // at 0x9437C (behind `fadd s0,s1,s8`; s1 = old p, s8 = 0.1)
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const float f = StepFactor<IsTail(Addr)>(Addr, ctx->X[Base] + Off);
        const float yF = *reinterpret_cast<float*>(ctx->X[Base] + 0x8B8), pF = ctx->S[1];
        Anchor spill; Anchor* st = AnchorOpen(kRampSite, ctx->X[Base], spill);
        const bool valid = st->n != 0 && st->n == g_N - 1 && SameBits(st->v[3], yF) && SameBits(st->v[4], pF);
        if (!valid) { st->v[0] = yF; st->v[1] = pF; st->v[2] = 0.0f; }
        const bool late = LateStart(valid, st->whole, f);
        if (f == 1.0f && st->v[2] == 0.0f) { st->n = 0; st->active = false; return; }
        double u = st->v[2], bAcc = 0.0; const double len = (st->whole || late) ? static_cast<double>(WholeCrossed<IsTail(Addr)>()) : static_cast<double>(f), bud = 0.0;
        float ya = st->v[0], pa = st->v[1];
        auto onUnit = [&](double) { pa = pa + 0.100000001f; if (pa > 1.0f) pa = 1.0f; ya = RampG(ya, pa); };
        FracWalk(u, bAcc, &len, &bud, 1, onUnit);
        if (late) { const double l2 = PhaseAfter<IsTail(Addr)>(); FracWalk(u, bAcc, &l2, &bud, 1, onUnit); }   // IMPL-34 item 1
        float yr = ya, pr = pa;
        if (u > 0.0) {
            const float uu = static_cast<float>(u);
            float pn = pa + 0.100000001f; if (pn > 1.0f) pn = 1.0f;
            yr = ya + uu * (RampG(ya, pn) - ya);
            pr = pa + uu * (pn - pa); if (pr > 1.0f) pr = 1.0f;
        }
        st->v[0] = ya; st->v[1] = pa; st->v[2] = static_cast<float>(u); st->v[3] = yr; st->v[4] = pr; st->v[5] = yr; st->n = g_N; st->active = u > 0.0;
        g_rampSpill = {st == &spill ? ctx->X[Base] : 0, g_N, yr};
        ctx->S[0] = pr;
    }
};
template <uintptr_t Addr>
struct RampY : exl::hook::impl::InlineHook<RampY<Addr>> {   // at 0x943BC, before `fadd s1,s2,s0` (s2 = y, s0 = p*d)
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const Anchor* st = AnchorLook(kRampSite, ctx->X[22]);
        if (g_rampSpill.owner == ctx->X[22] && g_rampSpill.n == g_N) { ctx->S[2] = g_rampSpill.y; ctx->S[0] = 0.0f; g_rampSpill.owner = 0; return; }
        if (st == nullptr || st->n != g_N) return;
        ctx->S[2] = st->v[5];
        ctx->S[0] = 0.0f;
    }
};

// IMPL-32 items 1+3 change this: v[1] is now the CURRENT amplitude (each piece applies its share of the ramp in order and clamps; no summed signed budget, no dsAcc), and a table-full visit
// commits the W whole units the clock crossed (no partial). The text below is the IMPL-30 description.
// IMPL-23 item 5 / IMPL-27 item 5 / IMPL-30 item 5: yaw increments of 0x4AB9DC (yaw -= sin(phase) 10deg amp) / 0x4ABB70 (yaw +=). One native unit: amp = clamp(amp + ds), term =
// sin(phase) 10deg amp (the NEW amplitude, the OLD phase), yaw -= term, phase += 10deg (FAdd at KeySite, f times). ds = +-0.1 x the caller step (AmpStash: amp0 = the amplitude before the
// native ramp, ds = the whole step's ramp). Fractional-unit framework, anchor per (Addr, x21): v[0] = phase pa and v[1] = amplitude aa at the last whole-unit boundary, v[2] = progress u,
// v[3] = yaw (the term) already applied for the unit in progress, v[4] = the phase the field held after the last update (YawRecord, behind the wrap store), v[5] = the amplitude reported,
// v[6] = the ramp dsAcc the unit in progress has consumed. The update walks its pieces (UnitBudgets of the actor x19; r scaled to f, ds shared by the BUDGETS b_j / sum(b), not ds / f
// averaged): a unit crossed gets amp = clamp(aa + its ds), term = sin(pa) 10deg amp, then pa += 10deg; the rest has u x term with the partial amplitude clamp(aa + dsAcc). The yaw moves by
// committed + new partial - old partial; the native term (from the fields) is taken out. The amplitude field is written with the anchored value. f == 1 with u == 0: native.
// Table full: anchored at the fields (u = 0) for this visit. The phase field itself stays native + FAdd (pa + 10deg u mod 360, same value).
template <int Sign>
struct AmpStash : exl::hook::impl::InlineHook<AmpStash<Sign>> {   // before `fadd s1,s1,s2` (s1 = amp, s2 = signed ramp step); x21 = yaw state
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        ThreadCtx& t = Thr();
        t.aOwner = ctx->X[21]; t.aN = g_N; t.aAmp0 = ctx->S[1]; t.aDs = ctx->S[2];
    }
};
template <uintptr_t Addr, uintptr_t KeySite, int Sign>
struct YawUnits : exl::hook::impl::InlineHook<YawUnits<Addr, KeySite, Sign>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const u64 base = ctx->X[21];
        const float f = PeekStep<IsTail(KeySite)>(KeySite, base + 0x2D70);
        ThreadCtx& th = Thr();
        const bool stash = th.aN == g_N && th.aOwner == base;
        const float ph = *reinterpret_cast<float*>(base + 0x2D70), ampF = *reinterpret_cast<float*>(base + 0x2D74), dl = 0.174532920f;
        const float amp0 = stash ? th.aAmp0 : ampF;
        Anchor spill; Anchor* st = AnchorOpen(Addr, base, spill);
        const bool valid = st->n != 0 && st->n == g_N - 1 && SameBits(st->v[4], ph) && SameBits(st->v[5], amp0);
        double pa = valid ? st->v[0] : ph, u = valid ? st->v[2] : 0.0;
        const bool late = LateStart(valid, st->whole, f);
        float ac = valid ? st->v[1] : amp0;   // IMPL-32 item 3: the CURRENT amplitude (ordered, clamped per piece); no anchor + summed signed budget
        const double pOld = valid ? st->v[3] : 0.0;
        if (f == 1.0f && u == 0.0 && pOld == 0.0) { st->active = false; st->n = 0; return; }   // IMPL-31 item 4: same rule (pOld is 0 whenever u == 0 here; nothing is owed)
        const float ds = stash ? th.aDs : 0.0f;
        const bool wh = st->whole || late;
        const double fw = wh ? static_cast<double>(WholeCrossed<IsTail(KeySite)>()) : static_cast<double>(f), sf = wh ? (f > 0.0f ? fw / f : 0.0) : 1.0;   // IMPL-32 item 1
        float ub[17], ur[17]; const int n = UnitBudgets(reinterpret_cast<u8*>(ctx->X[19]), ub, ur);
        double len[18], bud[18]; int m = 0; double sr = 0.0, sb = 0.0;
        for (int j = 0; j < n; j++) { sr += ur[j]; sb += ub[j]; }
        if (n > 0 && sr > 0.0) { for (int j = 0; j < n; j++) { len[m] = ur[j] * (fw / sr); bud[m] = (sb > 0.0 ? ds * (ub[j] / sb) : 0.0) * sf; m++; } }
        else { len[0] = fw; bud[0] = ds * sf; m = 1; }
        auto clamp01 = [](float a) { a = __builtin_fminf(a, 1.0f); return __builtin_fmaxf(a, 0.0f); };
        // IMPL-32 item 3 (example: amplitude 0.98, ramps +0.05 then -0.05 -> 1.0 then 0.95): each piece applies its share of the ramp to the current amplitude IN ORDER and clamps; a unit crossed gets
        // the term with the amplitude at that moment (native: the NEW amplitude of the unit). Not FracWalk: the ramp is not a sum.
        double committed = 0.0;
        auto run = [&](const double* lenA, const double* budA, int cnt) {
            for (int j = 0; j < cnt; j++) {
                double left = lenA[j];
                if (!(left > 0.0)) { ac = clamp01(ac + static_cast<float>(budA[j])); continue; }
                const double rate = budA[j] / lenA[j];
                for (int guard = 0; left > 0.0 && guard < 64; guard++) {
                    const double room = 1.0 - u, take = left < room ? left : room;
                    ac = clamp01(ac + static_cast<float>(rate * take)); u += take; left -= take;
                    if (u >= 1.0 - 1.0e-6) {
                        committed += (__builtin_sinf(static_cast<float>(pa)) * dl) * ac;
                        pa = __builtin_fmod(pa + dl, 6.283185307179586); u = 0.0;
                    }
                }
            }
        };
        run(len, bud, m);
        if (late) { const double l2 = PhaseAfter<IsTail(KeySite)>(), b2 = f > 0.0f ? ds * (l2 / f) : 0.0; run(&l2, &b2, 1); }   // IMPL-34 item 1
        const float ampVis = ac; double pNew = 0.0;
        if (u > 0.0) pNew = u * ((__builtin_sinf(static_cast<float>(pa)) * dl) * ampVis);
        const float t0 = (__builtin_sinf(ph) * dl) * ampF;
        ctx->S[0] += Sign * static_cast<float>(committed + pNew - pOld - t0);
        *reinterpret_cast<float*>(base + 0x2D74) = ampVis;
        st->v[0] = static_cast<float>(pa); st->v[1] = ac; st->v[2] = static_cast<float>(u); st->v[3] = static_cast<float>(pNew); st->v[4] = std::bit_cast<float>(0xFFFFFFFFu); st->v[5] = ampVis;
        st->n = g_N; st->active = u > 0.0;
        th.yOwner = base; th.yN = g_N;
    }
};
template <uintptr_t OwnerAddr>
struct YawRecord : exl::hook::impl::InlineHook<YawRecord<OwnerAddr>> {   // 0x4AB9FC / 0x4ABB90: behind the wrap store of the phase; records the phase bits the field holds now
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        ThreadCtx& th = Thr();
        if (th.yN != g_N || th.yOwner != ctx->X[21]) return;
        th.yN = 0;
        Anchor* st = AnchorLook(OwnerAddr, ctx->X[21]);
        if (st != nullptr && st->n == g_N) st->v[4] = *reinterpret_cast<float*>(ctx->X[21] + 0x2D70);
    }
};

// IMPL-13 step 2 (RELOAD-INT, tools/reload_int.py): after the constant store P of an expired integer countdown, the ticks the floor cut in this update (c.left) belong to the
// reloaded timer: field = P - left, in the same update. left >= P: the field stays <= 0, so the next update expires again (deferred one update, counted).
u32 g_reloadIntMulti = 0;
template <uintptr_t Addr, int Rt, int Rb, int Off, int Size, uintptr_t Site>
struct ReloadInt : exl::hook::impl::InlineHook<ReloadInt<Addr, Rt, Rb, Off, Size, Site>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        void* f = reinterpret_cast<void*>(ctx->X[Rb] + Off);
        Counter* c = LookCounter(Site, ctx->X[Rb] + Off);
        if (!c || c->U != g_N || c->left <= 0) return;
        s64 left = c->left; c->left = 0; c->carry -= std::min<s64>(c->carry, left);
        if (Size == 4) { s32 p = *static_cast<s32*>(f); if (p <= 0) return; if (left >= p) g_reloadIntMulti++; *static_cast<s32*>(f) = static_cast<s32>(p - left); }
        else { s8 p = *static_cast<s8*>(f); if (p <= 0) return; if (left >= p) g_reloadIntMulti++; *static_cast<s8*>(f) = static_cast<s8>(p - left); }
    }
};

// IMPL-14 step 1 + IMPL-15 (multi-expiry float reloads, tools/reload_multi.py -> impl/reload_multi.inc, fix review 1b item B): a countdown with a small period P can expire
// k = 1 + floor(-r / P) times in one update (r <= 0 = the remainder after the step). The store of P is followed only by argument setup and ONE call X (SIMPLE shape).
// Native order is replayed: (1) MultiHook (instruction after the store) writes the native value P + r, so the native `bl X` is expiry 1; it pushes {k, P, field, first value}.
// (2) MultiCall (the `bl X`, or MultiHook itself when that is the next instruction) saves the argument registers. (3) MultiAfter (instruction after the `bl X`) re-reads the field:
// != P + r -> the native call changed the timer, stop and count the k - 1 dropped expiries in g_multiStop. Else expiry i = 2..k: field = previous + P, call X with the saved
// registers, re-read; != what we wrote -> stop, count k - i in g_multiStop. A stack (not a global) because X may reach another multi site. k is capped at kMultiMax.
constexpr int kMultiMax = 32;
u32 g_multiCap = 0, g_multiCalls = 0, g_multiStop = 0;
struct MultiState { int k; float p, last; float* field; uintptr_t site; bool force; u64 x[8]; float64x2_t v[8]; };
constexpr int kMultiDepth = 8;
MultiState g_multiStack[kMultiDepth]; int g_multiDepth = 0;
inline double MultiExpiriesRaw(float p, float r) {   // number of expiries k >= 1 for remainder r (<= 0) and period p
    if (!(p > 0.0f) || !(r < 0.0f)) return 1;
    return std::floor(-static_cast<double>(r) / p) + 1.0;
}
inline bool MultiSame(float a, float b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
// IMPL-15b: no call sees a field value <= 0. k >= 2: calls 1..k-1 see P, call k sees f = k P + r in (0, P] (computed once). k == 1: f = P + r. k > 32: 32 calls, all see P, field keeps P.
// Only the logic thread pushes a frame; MultiCall / MultiAfter act only when the top frame belongs to their site.
inline bool MultiTop(uintptr_t site) { return ThreadId() == g_logicThread && g_multiDepth >= 1 && g_multiDepth <= kMultiDepth && g_multiStack[g_multiDepth - 1].site == site; }
inline void MultiCapture(exl::hook::InlineFloatCtx* ctx, uintptr_t site) {
    if (!MultiTop(site)) return;
    MultiState& m = g_multiStack[g_multiDepth - 1];
    for (int i = 0; i < 8; i++) { m.x[i] = ctx->X[i]; __builtin_memcpy(&m.v[i], &ctx->Q[i], 16); }
}
template <uintptr_t Addr, int C, int X, int Rb, int Off, uintptr_t CallAddr, ptrdiff_t Target>
struct MultiHook : exl::hook::impl::InlineHook<MultiHook<Addr, C, X, Rb, Off, CallAddr, Target>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread || g_multiDepth >= kMultiDepth) return;
        const float p = std::bit_cast<float>(static_cast<u32>(ctx->W[C])), r = ctx->S[X];
        constexpr bool kForceSite = Addr == 0x8DD54 + 4;   // IMPL-29 item 1: an exact-zero expiry (r == 0, k = 1) of 0x8DD54 also needs the forceOne scope
        if (!((r < 0.0f || (kForceSite && r == 0.0f)) && p > 0.0f)) return;
        const double kr = MultiExpiriesRaw(p, r);
        int k = static_cast<int>(kr > kMultiMax ? kMultiMax : kr);
        float f = static_cast<float>(static_cast<double>(k) * p + r);
        if (!(f > 0.0f) || f > p) f = p;
        if (kr > kMultiMax) { g_multiCap += static_cast<u32>(kr - kMultiMax); f = p; }
        float* fld = &At<float>(reinterpret_cast<void*>(ctx->X[Rb]), Off);
        *fld = k >= 2 ? p : f;
        MultiState& m = g_multiStack[g_multiDepth++];
        m.k = k; m.p = p; m.last = f; m.field = fld; m.site = CallAddr + 4;
        // IMPL-25 item 3 (0x8DD54): HandlerGate must run exactly ONE Orig call from the first native expiry call through every extra call (descendant gates included): forceOne is
        // raised here (before the native `bl` at 0x94040) and lowered in MultiAfter after the last extra call, on every exit of that function.
        m.force = kForceSite;
        if (m.force) Thr().forceOne++;
        if (CallAddr == Addr) MultiCapture(ctx, CallAddr + 4);
    }
};
template <uintptr_t CallAddr, ptrdiff_t Target>
struct MultiCall : exl::hook::impl::InlineHook<MultiCall<CallAddr, Target>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { MultiCapture(ctx, CallAddr + 4); }
};
template <uintptr_t AfterAddr, ptrdiff_t Target>
struct MultiAfter : exl::hook::impl::InlineHook<MultiAfter<AfterAddr, Target>> {
    static void Callback(exl::hook::InlineFloatCtx*) {
        if (!MultiTop(AfterAddr)) return;
        const MultiState m = g_multiStack[--g_multiDepth];   // copy: the extra calls may push onto the same slot
        struct Unforce { bool on; ~Unforce() { if (on) { ThreadCtx& t = Thr(); if (t.forceOne > 0) t.forceOne--; } } } unforce{m.force};   // IMPL-25 item 3
        if (m.k < 2) return;
        if (!MultiSame(*m.field, m.p)) { g_multiStop += m.k - 1; return; }
        using Fn = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64, float64x2_t, float64x2_t, float64x2_t, float64x2_t, float64x2_t, float64x2_t, float64x2_t, float64x2_t);
        for (int i = 2; i <= m.k; i++) {
            const float w = i == m.k ? m.last : m.p; *m.field = w;
            g_multiCalls++;
            Game<Fn>(Target)(m.x[0], m.x[1], m.x[2], m.x[3], m.x[4], m.x[5], m.x[6], m.x[7], m.v[0], m.v[1], m.v[2], m.v[3], m.v[4], m.v[5], m.v[6], m.v[7]);
            if (!MultiSame(*m.field, w)) { g_multiStop += m.k - i; return; }
        }
    }
};

// IMPL-14 step 6 (tools/reload_int_before.py): the same rewrite for stores whose NEXT instruction is a branch target: the hook sits BEFORE the store and changes the register that
// holds P (P - leftover); the native store writes it. The register is dead after the store (proved by the generator), so the changed value is not seen elsewhere.
template <uintptr_t Addr, int Rt, int Rb, int Off, int Size, uintptr_t Site>
struct ReloadIntBefore : exl::hook::impl::InlineHook<ReloadIntBefore<Addr, Rt, Rb, Off, Size, Site>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread) return;
        Counter* c = LookCounter(Site, ctx->X[Rb] + Off);
        if (!c || c->U != g_N || c->left <= 0) return;
        s64 left = c->left; c->left = 0; c->carry -= std::min<s64>(c->carry, left);
        const s64 p = Size == 4 ? static_cast<s64>(static_cast<s32>(ctx->W[Rt])) : static_cast<s64>(static_cast<s8>(ctx->W[Rt] & 0xFF));
        if (p <= 0) return;
        if (left >= p) g_reloadIntMulti++;
        ctx->W[Rt] = static_cast<u32>(p - left) & (Size == 4 ? 0xFFFFFFFFu : 0xFFu);
    }
};

// IMPL-22 item 2 (tools/reset_carry.py): hook AFTER a reset store of the SAME field a crossing counter owns. The crossing left carry > 0 (units the exact trigger cut off
// in THIS update); the native reset wiped the field, so the carry is applied to the reset value now: v = reset + sign*n*K, kept strictly below ExactL (sign +1) or >= 1 (sign -1),
// so the native visit still fires the event. The rest of the carry stays for the next visit.
u32 g_reloadLost = 0;
// IMPL-25 item 1: ZeroOnly = the hook sits at a join point (0xF22348 is also the b.ne target that SKIPS the reset store): it acts only when the field in memory holds 0, i.e. the reset store ran.
template <uintptr_t Addr, uintptr_t Site, int Base, ptrdiff_t Off, int K, int Sign, int ExactL, bool MainOnly, bool ZeroOnly = false>
struct ResetCarry : exl::hook::impl::InlineHook<ResetCarry<Addr, Site, Base, Off, K, Sign, ExactL, MainOnly, ZeroOnly>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || (MainOnly && ThreadId() != g_logicThread)) return;
        Counter* c = LookCounter(Site, ctx->X[Base] + Off);   // find only, never creates
        if (!c || c->U != g_N || c->carry <= 0) return;
        s32* f = reinterpret_cast<s32*>(ctx->X[Base] + Off);
        if (ZeroOnly && *f != 0) return;
        const s64 reset = *f;
        s64 n = c->carry;
        if (Sign > 0) { if (ExactL < 0) return; n = std::min<s64>(n, reset > ExactL - 1 ? 0 : (ExactL - 1 - reset) / K); }
        else n = std::min<s64>(n, reset < 1 ? 0 : (reset - 1) / K);
        if (n <= 0) return;
        *f = static_cast<s32>(reset + Sign * n * K);
        c->carry -= n;
    }
};

// IMPL-13 step 4 (slot-167 call sites, W6): hook on `blr x8` (virtual slot 167, callee 0x285584 / 0x294924 / 0x2A45E8) where w2 = the amount. The amount becomes amount x d, with d = whole
// ticks since this (site, actor) last ran (<= 4); d = 1 on the first visit, after a gap, or when the actor's state words changed since the last visit (a new action).
// d = 0: w2 = 0 and the callee returns 0 at once (`cmp w2,#1 ; b.lt` exit, no state touched); the caller then takes its no-event path.
u32 g_slot167Zero = 0;
// Item 4. Generation of the action state per actor: every call of the state setter 0x58C7B8 (hook on its +0x145C store 0x58C7BC; x0 = actor) takes the next value of a global counter.
// The Slot167 key of 0xEB368 XORs it (as 0x17CE90 does with Slot167Gen), so a state set between two visits makes the second visit a new key (amount 1).
// Residual: direct +0x145C writes that bypass 0x58C7B8 are not seen. ponytail: 512 actors, 8 probes; a lost entry reads as generation 0 = one extra "new key" visit (native amount).
struct GenSt { u64 actor, gen, n; };   // IMPL-52: n = last update that used the entry
constexpr int kGen = 512;
GenSt g_gen[kGen]; u64 g_genCtr = 0;
GenSt* GenFind(u64 actor, bool create) {
    GenSt* old = nullptr;
    const u64 h = (actor / 16) * 0x9E3779B97F4A7C15ull >> 20;
    for (int i = 0; i < kProbes; i++) {
        GenSt& e = g_gen[(h + i) % kGen];
        if (e.actor == actor) { e.n = g_N; return &e; }
        if (e.actor == 0) { old = &e; break; }
        if (old == nullptr || e.gen < old->gen) old = &e;
    }
    if (!create) return nullptr;
    if (old->actor != 0 && old->n + 1 >= g_N) g_genEvictLive++;   // IMPL-52: genEvictLive
    *old = {actor, 0, g_N};
    return old;
}
struct StateSetGen : exl::hook::impl::InlineHook<StateSetGen> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread) return;
        // IMPL-31 item 3 (fix review 7): the hook runs BEFORE the FIRST store of the setter (str w1,[x0,#0x1458] at 0x58C7B8; w1 = new +0x1458, w2 = new +0x145C), so both fields
        // still hold the old action tuple: the generation goes up when (+0x1458, +0x145C) differs, so A -> B -> A with +0x145C unchanged bumps at each change. An identical re-set keeps it.
        // Residual: a genuine restart of the SAME tuple (re-entry) looks identical; it needs an entry marker that is not known.
        if (ctx->W[1] == At<u32>(reinterpret_cast<void*>(ctx->X[0]), 0x1458) && ctx->W[2] == At<u32>(reinterpret_cast<void*>(ctx->X[0]), 0x145C)) return;
        GenFind(ctx->X[0], true)->gen = ++g_genCtr;
    }
};

constexpr u64 kGenSite = (1ull << 41) | 0x17CD38;
// IMPL-19: hook after `str w8,[x19,#0x145C]` (0x17CD34, owner 0x17BF20, x19 = actor): once per entry into substate 0x79. Counter.aux = generation.
struct Slot167Gen : exl::hook::impl::InlineHook<Slot167Gen> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread) return;
        FindCounter(kGenSite, ctx->X[19], g_G).aux++;
    }
};
template <uintptr_t Addr>
struct Slot167 : exl::hook::impl::InlineHook<Slot167<Addr>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread) return;
        u8* a = reinterpret_cast<u8*>(ctx->X[19]);
        u64 st = (static_cast<u64>(*reinterpret_cast<u32*>(a + 0x1458)) << 32) ^ (static_cast<u64>(*reinterpret_cast<u32*>(a + 0x145C)) * 0x9E3779B1ull) ^ *reinterpret_cast<u32*>(a + 0x16B0);
        if (Addr == 0x17CE90) { Counter* gc = LookCounter(kGenSite, ctx->X[19]); if (gc) st ^= gc->aux * 0xD6E8FEB86659FD93ull; }   // IMPL-19: new entry into state 0x79 = new generation
        if (Addr == 0xEB368) { GenSt* g = GenFind(ctx->X[19], false); if (g) st ^= g->gen * 0xD6E8FEB86659FD93ull; }   // IMPL-26 item 4: generation of the state setter 0x58C7B8
        Counter& c = FindCounter(Addr, ctx->X[19], g_G);
        u64 dt = (c.U == g_N - 1 && c.aux == st) ? g_G - c.S : 1;
        if (dt > 4) dt = 4;
        c.S = g_G; c.U = g_N; c.aux = st;
        if (dt == 0) g_slot167Zero++;
        ctx->W[2] = static_cast<u32>(ctx->W[2] * dt);
    }
};

// RELOAD (6, tools/reload.py): `x -= s; fcmp x,0; str x; b.hi skip; C = P; str C` reloads a timer and throws away the overshoot.
// Hook at the constant store: C (register holding P) becomes P + x, x = the remainder (<= 0) still in sX. IMPL-22: a remainder below -P keeps its phase: P + fmod(x, P).
template <uintptr_t Addr, int C, bool IsF, int X>
struct ReloadHook : exl::hook::impl::InlineHook<ReloadHook<Addr, C, IsF, X>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        float p = IsF ? ctx->S[C] : std::bit_cast<float>(static_cast<u32>(ctx->W[C]));
        float r = ctx->S[X];
        if (!(p > 0.0f && r < 0.0f)) return;   // also rejects NaN
        float v;
        if (r > -p) v = p + r;   // IMPL-22: -P < r < 0, the old formula (bit-exact)
        else {   // overshoot of one or more whole periods: keep the phase, count the expiries the game cannot run
            v = p + std::fmod(r, p);
            if (!(v > 0.0f && v <= p)) v = p;
            g_reloadLost += static_cast<u32>(std::floor(-r / p));
        }
        if (IsF) ctx->S[C] = v; else ctx->W[C] = std::bit_cast<u32>(v);
    }
};

// IMPL-25 item 2 (0x56DBF0, review round 18): the state-2 path (0x56DA48) clears the timer, so its first visit of state 3 (0x56DBD4) subtracts from 0 and the native first reload 1.0 must stay.
// OldTimer (before the `fsub` at 0x56DBDC, s[Reg] = the old timer) records the old value for this owner; ReloadOld (the constant store 0x56DBF0) applies the ReloadHook phase rule
// only when that old value was > 0 (an established expiry). Old value 0: native.
template <uintptr_t Addr, int Reg, int Base>
struct OldTimer : exl::hook::impl::InlineHook<OldTimer<Addr, Reg, Base>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { ThreadCtx& t = Thr(); t.oOwner = ctx->X[Base]; t.oOld = ctx->S[Reg]; }
};
template <uintptr_t Addr, int C, bool IsF, int X, int Base>
struct ReloadOld : exl::hook::impl::InlineHook<ReloadOld<Addr, C, IsF, X, Base>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        const bool est = t.oOwner == ctx->X[Base] && t.oOld > 0.0f;
        t.oOwner = 0;
        if (est) ReloadHook<Addr, C, IsF, X>::Callback(ctx);
    }
};

// IMPL-25 item 4 (0xB01C, P 2.0, actor x19): the expiry body 0xB014..0xB050 runs ONCE natively (str reload at 0xB01C, then four record calls). ExpiryMark (the str, w8 = P, s0 = remainder)
// applies the phase rule (ReloadHook) and notes k = 1 + floor(-r / P) expiries due for this actor on the thread; ExpiryExtra (0xB054, the join, reached by both paths) runs the four calls
// k - 1 more times from module code, each with a fresh zeroed 16-byte-aligned record. The frame reserves sp+0x30 .. sp+0x160 (the next used slot is the saved d8 at sp+0x160) = 0x130 bytes;
// the constructor 0xB90954 writes up to +0x120, so the real record size is below that bound (the size 0x130 is the distance, not a proven struct size).
constexpr size_t kRecBytes = 0x130;
u32 g_expiryExtra = 0;
inline double ExpiryCount(float p, float r) { return (p > 0.0f && r < 0.0f) ? std::floor(-static_cast<double>(r) / p) + 1.0 : 1.0; }
template <uintptr_t Addr, int C, int X, int Base>
struct ExpiryMark : exl::hook::impl::InlineHook<ExpiryMark<Addr, C, X, Base>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        const float p = std::bit_cast<float>(static_cast<u32>(ctx->W[C])), r = ctx->S[X];
        const double kr = ExpiryCount(p, r);
        ReloadHook<Addr, C, false, X>::Callback(ctx);
        if (g_N == 0 || ThreadId() != g_logicThread || !(kr >= 2.0)) return;
        ThreadCtx& t = Thr();
        t.xOwner = ctx->X[Base]; t.xK = static_cast<int>(kr > kMultiMax ? kMultiMax : kr);
    }
};
inline void ExpirySeq(u64 actor) {
    alignas(16) u8 rec[kRecBytes] = {};
    Game<void (*)(void*)>(0xB90954)(rec);
    Game<void (*)(void*, u64)>(0xB90B64)(rec, actor + 0x1A18);
    Game<void (*)(void*, u64, u32)>(0xB8FD44)(rec, At<u64>(reinterpret_cast<void*>(actor), 0x12E0), 0xFF);
    Game<void (*)(void*, u64)>(0xB90B74)(rec, actor + 0x41D0);
}
template <uintptr_t Addr, int Base>
struct ExpiryExtra : exl::hook::impl::InlineHook<ExpiryExtra<Addr, Base>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        if (t.xOwner == 0 || t.xOwner != ctx->X[Base]) return;
        const int k = t.xK; t.xOwner = 0; t.xK = 0;
        for (int i = 1; i < k; i++) { g_expiryExtra++; ExpirySeq(ctx->X[Base]); }
    }
};

// RELOAD arithmetic init (IMPL-8 item 6): `fsub sX, sP, s0` starts the next wait at P - step (stock charges one 60 Hz tick for the transition).
// Before the fsub sX still holds the old remainder r (<= 0); after it the hook sets sX = P - 1 + r. The guard tests the value initialised
// (P - 1 + r > 0, r <= 0); otherwise the native P - step stays. Two hooks per site (the second is at the next instruction, a plain fcmp).
float g_subR = 0.0f; bool g_subValid = false;
template <uintptr_t Addr, int X>
struct ReloadSubBefore : exl::hook::impl::InlineHook<ReloadSubBefore<Addr, X>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { g_subR = ctx->S[X]; g_subValid = true; }
};
template <uintptr_t Addr, int X, int PR>
struct ReloadSubAfter : exl::hook::impl::InlineHook<ReloadSubAfter<Addr, X, PR>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (!g_subValid) return;
        g_subValid = false;
        float r = g_subR, v = ctx->S[PR] - 1.0f + r;
        if (r <= 0.0f && v > 0.0f) ctx->S[X] = v;   // IMPL-10: r = 0 included; also rejects NaN
    }
};

// IMPL-9 item 5 (review round 12): zero-interval adapters. `fdiv sR, 1.0, step*k` yields Inf when the step is 0. RecipFlag (hook right after the fdiv) notes that;
// RecipKeep (hook at the quad store that follows) then puts back the 16 bytes that are already stored at [X[Base] + Off], so no NaN or Inf is ever stored
// and the previous finite value stays. Positive steps are untouched.
template <uintptr_t Addr, int Reg>
struct RecipFlag : exl::hook::impl::InlineHook<RecipFlag<Addr, Reg>> {
    static inline bool zero = false;
    static void Callback(exl::hook::InlineFloatCtx* ctx) { zero = !std::isfinite(ctx->S[Reg]); }
};
template <uintptr_t FlagAddr, int FlagReg, uintptr_t Addr, int Q, int Base, ptrdiff_t Off>
struct RecipKeep : exl::hook::impl::InlineHook<RecipKeep<FlagAddr, FlagReg, Addr, Q, Base, Off>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        bool& z = RecipFlag<FlagAddr, FlagReg>::zero;
        if (!z) return;
        z = false;
        __builtin_memcpy(&ctx->Q[Q], reinterpret_cast<void*>(ctx->X[Base] + Off), 16);
    }
};

// IMPL-10 item 4, variant for a result that is a sum a + b of the unsafe term b and a kept term: before `fadd sD, sA, sB` set sA = 0 and sB = the 4 bytes
// stored at [X[Base] + Off] (0xBE3160 path: the blend result then equals the previously stored value).
template <uintptr_t FlagAddr, int FlagReg, uintptr_t Addr, int A, int B, int Base, ptrdiff_t Off>
struct RecipKeepSum : exl::hook::impl::InlineHook<RecipKeepSum<FlagAddr, FlagReg, Addr, A, B, Base, Off>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        bool& z = RecipFlag<FlagAddr, FlagReg>::zero;
        if (!z) return;
        z = false;
        ctx->S[A] = 0.0f;
        ctx->S[B] = *reinterpret_cast<float*>(ctx->X[Base] + Off);
    }
};
// IMPL-10 item 3/4 rule B: the derived value goes to a stack temporary, so no stored previous value exists. Per-object cache of the last finite value
// (direct-mapped, object pointer kept for the match check). Hook at the instruction that stores Q[Q]: step > 0 -> remember Q[Q] for the object X[Key];
// step == 0 (RecipFlag) -> use the remembered value of that object, zero if there is none. ponytail: 64 entries; a colliding object loses its entry (-> zero).
// IMPL-12 item 5: open-addressed table, 1,024 entries, keyed by the object pointer (no direct mapping, so two objects never share an entry). Zero only when this object has no
// entry. When the probe window (64 slots) has no free slot, the least recently used entry of the window is replaced and g_velEvict counts it.
struct VelEntry { u64 obj; u64 used; float v[4]; };
constexpr int kVel = 1024, kVelProbe = 64;
u32 g_velEvict = 0;
template <uintptr_t FlagAddr, int FlagReg, int Q, int Key>
struct RecipCache : exl::hook::impl::InlineHook<RecipCache<FlagAddr, FlagReg, Q, Key>> {
    static inline VelEntry tab[kVel];
    static VelEntry* Find(u64 obj, bool create) {
        VelEntry* lru = nullptr;
        for (int i = 0; i < kVelProbe; i++) {
            VelEntry& e = tab[((obj / 16) * 0x9E3779B1ull + i) % kVel];
            if (e.obj == obj) return &e;
            if (e.obj == 0) { if (!create) return nullptr; e.obj = obj; return &e; }
            if (lru == nullptr || e.used < lru->used) lru = &e;
        }
        if (!create) return nullptr;
        g_velEvict++; if (lru->used != 0 && lru->used + 1 >= g_N) g_velEvictLive++; *lru = {};   // IMPL-52: velEvictLive
        lru->obj = obj; return lru;
    }
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        bool& z = RecipFlag<FlagAddr, FlagReg>::zero;
        float* q = reinterpret_cast<float*>(&ctx->Q[Q]);
        if (z) {
            z = false;
            VelEntry* e = Find(ctx->X[Key], false);
            if (e != nullptr) { e->used = g_N; __builtin_memcpy(q, e->v, 16); } else __builtin_memset(q, 0, 16);
            return;
        }
        if (std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3])) {
            VelEntry* e = Find(ctx->X[Key], true); e->used = g_N; __builtin_memcpy(e->v, q, 16);
        }
    }
};

// C4b handler gate: a handler that a caller runs every update while a window is open. Key (handler, x0), site ids
// start at 1<<40 so they never meet an IntHook address. Held: n = ticks since the last call (0, 1, 2, clamp 4); first call or
// after a gap: n = 1 (stock). The return value is not used by any caller (checked by tools/handler_gate.py).
// (IMPL-8: the owner mode of IMPL-7 is removed; no owner is replayed.)
template <uintptr_t Addr>
struct HandlerGate : exl::hook::impl::TrampolineHook<HandlerGate<Addr>> {
    // All argument registers pass through unchanged: x0..x7 and v0..v7 (full 128 bit), so every Orig call sees what the caller set.
    static u64 Callback(u64 x0, u64 x1, u64 x2, u64 x3, u64 x4, u64 x5, u64 x6, u64 x7,
                        float64x2_t v0, float64x2_t v1, float64x2_t v2, float64x2_t v3,
                        float64x2_t v4, float64x2_t v5, float64x2_t v6, float64x2_t v7) {
        u64 n = 1;
        if (Thr().forceOne > 0) return HandlerGate<Addr>::Orig(x0, x1, x2, x3, x4, x5, x6, x7, v0, v1, v2, v3, v4, v5, v6, v7);   // IMPL-25 item 3: inside the 0x8DD54 expiry transaction: one call, no smart-tick state touched
        const Clk clk = ClockNow<IsTail(Addr)>();   // IMPL-26 item 0(b): the same clock as Credit (captured N / G inside an installed async context)
        if (clk.n != 0) {
            Counter tmp;
            Counter& c = CounterFor((1ull << 40) | Addr, x0, clk.n, clk.now, tmp);
            bool held = c.U == clk.n - 1;
            n = held ? clk.now - c.S : 1;
            if (n > 4) n = 4;
            c.S = clk.now;
            c.U = clk.n;
        }
        u64 r = 0;
        for (u64 i = 0; i < n; i++) r = HandlerGate<Addr>::Orig(x0, x1, x2, x3, x4, x5, x6, x7, v0, v1, v2, v3, v4, v5, v6, v7);
        return r;
    }
};

// TOKEN v7 (IMPL-8 item 5): owner-level fix for the token loop in fn 0xC1F798. Per-token body starts at 0xC1F868 (x14 = token):
//   +0x2c active, +0x34 delay (signed), +0x30 duration, +0x1c base, +0x20 current scale, +0x24 target scale (1.0f = 0x3f800000, kept in w13),
//   +0x18 channel, +0x28 published step = base * scale * channelBudget(channel) (channel budget = [[GOT 0x24ABBA8] + ch*16 + 0x94], 1.0 if ch > 4).
//   native unit: if active { if (delay-1 >= 0) {delay--; scale = 1.0} else { dur--; if (dur > 0) scale = target;
//                else { scale = target = 1.0; active = 0; dur = 0 } } }, then the loop publishes.
// Visit rule: the first visit of a token in an update (epoch = g_N) spends d = T units: d-1 here, the native body runs the last one.
//   A publication-only visit (T == 0, or a second visit in the epoch) makes the active flag read 0, so 0xC1F86C takes its cbz to 0xC1F884 and the
//   token state stays unchanged (no pre-add). An inactive token needs nothing: the native cbz skips the unit already.
// Segments: per token address and epoch the ordered unmultiplied budgets b_i (channel budget split evenly over the d units; one segment b = budget
//   when T == 0) and factors f_i = base * scale after unit i. Published step = sum(b_i * f_i); both publication paths (pool loop, after 0xC1F890,
//   and the single-token path 0xC1F054, after 0xC1F070) replace the product base*scale by the mean factor sum(b_i f_i) / sum(b_i) and the native channel
//   multiply finishes it. sum(b_i) == 0 keeps the endpoint factor (native). Re-arming (0xC1F134 / 0xC1F13C) clears the entry of that token.
constexpr u32 kOne = 0x3f800000;
struct PubClock { u64 N, G, Gt; float step, e0, ft, raw, accBefore; int T; bool paired; u64 Gp, Gtp; float accAfter, accTAfter, accTBefore; };   // the clock tuple of one published node (IMPL-31 item 2)
constexpr int kSegCap = 16 + 8;   // IMPL-35 item 1: output room of SplitUnitBoundaries = every input segment (<= kMaxSeg) + every boundary cut (T <= 5)
constexpr int kMaxSeg = 16;     // IMPL-12 item 3: <= 4 units + tail + override splits; overflow merges into the last segment
// IMPL-12 item 3(c): the cache is indexed by the native pool slot of the token (pool object = x0 of the loop 0xC1F798: base [x0+0x30], slot count [x0+0x38], stride 0x50),
// so there is no hashing, no collision and no eviction. The pool object is read by TokenHook every update (x0 is the pool there).
struct TokenEntry { u64 tok, epoch; int n; bool rearmed; float b[kMaxSeg], f[kMaxSeg], r[kMaxSeg]; float dim; bool dimOk, dimConst; };   // IMPL-57: dim = published step / e0 (not cleared by set-scale / rearm)   // b = raw x override (effective budget), r = raw duration
constexpr int kTokEntries = 1024;
constexpr u64 kTokStride = 0x50;
TokenEntry g_tok[kTokEntries];
u64 g_poolBase = 0; u64 g_poolCount = 0;
u32 g_asyncLock = 0;
inline void AsyncLock() { while (__atomic_exchange_n(&g_asyncLock, 1u, __ATOMIC_ACQUIRE)) svcSleepThread(1000); }   // HANG-1: sleep, never spin: Horizon does not time-slice, so a spinning higher-priority thread on the holder's core starves the holder forever
inline void AsyncUnlock() { __atomic_store_n(&g_asyncLock, 0u, __ATOMIC_RELEASE); }
// IMPL-31 item 2 (fix review 7): VERSIONED PUBLICATION NODES (replace the IMPL-29 clock double buffer and the IMPL-26 snapshot pool). A node is immutable once it is published:
// the clock tuple (N, G, Gt, step, e0, ft, raw, accBefore, T, paired), every pool token's published step (token+0x28, pool slot order) and the global +0xC4 read by 0xC1F0C0.
// WRITER = the logic thread only (Publish). It builds a NEW node (never the current one, never one with a reader pin) and publishes it with ONE release store of its index into g_pub:
// (a) at every N change (end of FrameTimer), (b) at the END of each publication batch: token loop exit 0xC1F998, single publication 0xC1F098, set-scale 0xC1F0FC, rearm 0xC1F140, and
// after every store of the global +0xC4 (0xC1F354 / 0xC1F4C8 reset, 0xC1F780 = the store at 0xC1F77C). Between a store and the end of its batch a capture sees the PREVIOUS node, which is a
// valid "capture just before the store". No free node (all 128 pinned): g_nodeFull counts it and the published index stays (captures get the older node; residual).
// READER (AsyncCapture): load g_pub (acquire), add one to that node's ref, load g_pub again; if it changed, drop the pin and retry (5 tries in all); the last try keeps its pin (g_pinRace)
// because a pinned node is still immutable. The pin is released when the record is consumed (TailPop of the callback) or replaced.
// ref bit 31 (kBuilding) = the in-use flag: the writer claims a node by a CAS 0 -> kBuilding, so a reader that raced a stale index sees the bit in the result of its add, drops the pin and retries.
// No spin-wait on another thread anywhere (HANG-1): the writer scans for a free node, the reader retries a bounded number of times.
// IMPL-33: 512 nodes (4 KB each). Pool full (g_nodeFull): the writer sets g_nodeFullFlag; a capture on the LOGIC thread while it is set copies the clock tuple from the live globals (exact: the logic thread
// is the only writer) and marks the record 'clock live' (AsyncCtx.live); its token +0x28 / C4 getters read the newest published node (g_nodeStaleTok). The flag clears at the next successful publication.
constexpr int kNodes = 512;   // IMPL-33 item 3: 128 -> 512
constexpr u32 kBuilding = 0x80000000u;
struct PubNode { u32 ref; PubClock c; bool c4ok; float c4; u32 cnt; u64 base; float v[kTokEntries]; };
PubNode g_node[kNodes + 64];   // + kOvf overflow nodes (IMPL-35 item 2)
int g_pub = -1;
float g_rawNow = 1.0f;   // the real frame time of this update (no override), set by FrameTimer, published as clock.raw
u32 g_nodeFull = 0, g_pinRace = 0, g_snapNotPool = 0, g_nodeStaleTok = 0, g_nodeHigh = 0, g_nodeFullFlag = 0;
inline void NodeUnpin(int id) { if (id >= 0) __atomic_sub_fetch(&g_node[id].ref, 1u, __ATOMIC_RELEASE); }
inline void NodePinAgain(int id) { if (id >= 0) __atomic_add_fetch(&g_node[id].ref, 1u, __ATOMIC_ACQ_REL); }   // a second pin on a node the caller already holds
// IMPL-33 item 2 (fix review 9): returns -1 only when nothing was ever published. A failed try (kBuilding seen, or the index moved) sleeps svcSleepThread(1000) before the next try (HANG-1: never spin).
// A pin on a node that is NOT building is immutable even if the index moved on; the last such pin is the fallback. After 5 tries the fallback is accepted (g_pinRace). If every try saw kBuilding
// the loop goes on (with the sleep): the writer finishes a build in bounded time and rebuilds a node only after kNodes - 1 other publications.
inline int NodePin() {
    int fb = -1;
    for (int a = 0;; a++) {
        const int i = __atomic_load_n(&g_pub, __ATOMIC_ACQUIRE);
        if (i < 0) return -1;
        const u32 r = __atomic_add_fetch(&g_node[i].ref, 1u, __ATOMIC_ACQ_REL);
        if (r & kBuilding) __atomic_sub_fetch(&g_node[i].ref, 1u, __ATOMIC_RELEASE);   // the writer is rebuilding this (stale) node
        else {
            if (__atomic_load_n(&g_pub, __ATOMIC_ACQUIRE) == i) { NodeUnpin(fb); return i; }
            NodeUnpin(fb); fb = i;                                                       // older but immutable: keep as the fallback
        }
        if (a >= 4 && fb >= 0) { __atomic_add_fetch(&g_pinRace, 1u, __ATOMIC_RELAXED); return fb; }
        svcSleepThread(1000);
    }
}
// Logic thread only. Fills a node from the live globals (clock tuple with the phase after, every pool token's +0x28, the global +0xC4).
void BuildNode(PubNode& n) {
    n.c = {g_N, g_G, g_Gt, g_step, g_e0, g_ft, g_rawNow, g_accBefore, g_T, g_paired, g_Gp, g_Gtp, static_cast<float>(g_acc), static_cast<float>(g_accT), g_accTBefore};
    n.base = g_poolBase;
    n.cnt = g_poolBase != 0 ? static_cast<u32>(std::min<u64>(g_poolCount, kTokEntries)) : 0;
    for (u32 j = 0; j < n.cnt; j++) n.v[j] = *reinterpret_cast<float*>(g_poolBase + j * kTokStride + 0x28);
    u8* tbl = **reinterpret_cast<u8***>(exl::util::modules::GetTargetStart() + 0x24ABBA8);   // the pointer chain of the getter asm 0xC1F0AC..0xC1F0BC
    n.c4ok = tbl != nullptr; n.c4 = tbl ? *reinterpret_cast<float*>(tbl + 0xC4) : 1.0f;
}
// IMPL-35 item 2 (fix review 11): the getter 0xC1F0A8 reads its token from [x0] when it RUNS (ldr x8,[x0]; ldr s0,[x8,#0x28]), so the pool index is not known at capture time. A capture on the
// logic thread while the 512 nodes are pinned builds a private overflow node (index kNodes + j, 64 of them, used only then, never published) from the live state: clock + all token values + C4
// together, exact because the logic thread is the only writer. Returns its index pinned once, or -1 when all 64 are pinned too (g_ovfFull, residual).
constexpr int kOvf = 64;
u32 g_nodeOverflow = 0, g_ovfFull = 0;
int OverflowNode() {
    for (int j = 0; j < kOvf; j++) {
        u32 zero = 0;
        if (!__atomic_compare_exchange_n(&g_node[kNodes + j].ref, &zero, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
        BuildNode(g_node[kNodes + j]); g_nodeOverflow++; return kNodes + j;
    }
    g_ovfFull++; return -1;
}
// Logic thread only. Builds the node from the live globals and publishes it.
void Publish() {
    if (g_N == 0 || ThreadId() != g_logicThread) return;
    const int cur = g_pub;
    { u32 pinned = 0; for (int i = 0; i < kNodes; i++) if (__atomic_load_n(&g_node[i].ref, __ATOMIC_RELAXED) != 0) pinned++; if (pinned > g_nodeHigh) g_nodeHigh = pinned; }
    for (int k = 1; k <= kNodes; k++) {
        const int i = ((cur < 0 ? -1 : cur) + k) % kNodes;
        if (i == cur) continue;
        u32 zero = 0;
        if (!__atomic_compare_exchange_n(&g_node[i].ref, &zero, kBuilding, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
        PubNode& n = g_node[i];
        BuildNode(n);
        __atomic_sub_fetch(&n.ref, kBuilding, __ATOMIC_RELEASE);   // readers that raced the build keep their (transient) counts
        __atomic_store_n(&g_pub, i, __ATOMIC_RELEASE);
        g_nodeFullFlag = 0;
        return;
    }
    g_nodeFull++; g_nodeFullFlag = 1;
}
// IMPL-32 item 1: whole units the clock crossed in the update the thread sees (an installed async context: its pinned node), and the fractional phase before that update.
template <bool Tail> inline u64 WholeCrossed() {
    const ThreadCtx& t = Thr();
    if (t.hasPaired) return Tail ? t.cGt - t.cGtp : t.cG - t.cGp;
    return Tail ? g_Gt - g_Gtp : g_G - g_Gp;
}
template <bool Tail> inline double PhaseAfter() {
    const ThreadCtx& t = Thr();
    return t.hasPaired ? (Tail ? t.cAccTA : t.cAccA) : (Tail ? g_accT : g_acc);
}
template <bool Tail> inline double PhaseBefore() {   // IMPL-36 item 2: both phases come from the installed context
    const ThreadCtx& t = Thr();
    return t.hasPaired ? (Tail ? t.cAccTB : t.cAcc) : (Tail ? g_accTBefore : g_accBefore);   // IMPL-36 item 2: an installed context uses its captured tail phase before
}
struct PubBatch : exl::hook::impl::InlineHook<PubBatch> {   // after the last store of a publication batch (and after every +0xC4 store)
    static void Callback(exl::hook::InlineFloatCtx*) { Publish(); }
};
TokenEntry* FindToken(u64 tok, bool create) {
    if (g_poolBase == 0 || tok < g_poolBase) return nullptr;
    const u64 off = tok - g_poolBase;
    if (off % kTokStride != 0 || off / kTokStride >= g_poolCount || off / kTokStride >= kTokEntries) return nullptr;
    TokenEntry& e = g_tok[off / kTokStride];
    if (e.tok != tok) { if (!create) return nullptr; e = {}; e.tok = tok; e.epoch = ~0ull; }   // epoch = never visited
    return &e;
}
inline void TokenUnit(u8* t) {
    if (At<u32>(t, 0x2c) == 0) return;
    s32 dl = At<s32>(t, 0x34) - 1;
    if (dl >= 0) { At<s32>(t, 0x34) = dl; At<u32>(t, 0x20) = kOne; return; }
    s32 du = At<s32>(t, 0x30) - 1;
    At<s32>(t, 0x30) = du;
    if (du > 0) { At<u32>(t, 0x20) = At<u32>(t, 0x24); return; }
    At<u32>(t, 0x20) = kOne; At<u32>(t, 0x24) = kOne;
    At<u32>(t, 0x2c) = 0; At<u32>(t, 0x30) = 0;
}
inline float ChannelBudget(u8* t) {   // mirrors 0xC1F894..0xC1F8A8 / 0xC1F074..0xC1F08C
    u32 ch = At<u32>(t, 0x18);
    if (static_cast<s32>(ch) > 4) return 1.0f;
    u8* tbl = **reinterpret_cast<u8***>(exl::util::modules::GetTargetStart() + 0x24ABBA8);
    return tbl ? At<float>(tbl + static_cast<size_t>(ch) * 16, 0x94) : 1.0f;
}
inline float TokenFactor(u8* t) { return At<float>(t, 0x1c) * At<float>(t, 0x20); }
// Mean factor of the stored segments, or false (keep the native endpoint factor).
inline bool MeanFactor(u64 tok, float& out) {
    TokenEntry* e = FindToken(tok, false);
    if (e == nullptr || e->epoch != g_N || e->n == 0) return false;
    float sb = 0.0f, sf = 0.0f;
    for (int i = 0; i < e->n; i++) { sb += e->b[i]; sf += e->b[i] * e->f[i]; }
    if (!(sb > 0.0f)) return false;
    out = sf / sb;
    return true;
}
// Publication-only visit (IMPL-9 item 4): the load `ldr w15,[x14,#0x2c]` at 0xC1F868 is followed by `cbz w15,0xC1F884`, a cbz the installer cannot relocate, so
// no hook can sit after the load. Instead the load is patched to `mov w15,w15` (a no-op, Patch in exl_main) and the hook at 0xC1F868 does the load itself:
// ctx->W[15] = 0 on a publication-only visit (so the native cbz takes its branch to 0xC1F884), else the real +0x2c value. No memory write, no restore hook.
// Segments of one update (IMPL-12 item 3a): the admitted pieces (g_adm: raw, override) are cut at the integer tick boundaries of the G accumulator (boundary k lies at
// raw position k - g_accBefore, k = 1..T) and at the piece (override) boundaries. A segment has real raw duration r, effective budget b = r x override and the factor
// of the unit that ends it (f_k after unit k; time after the last boundary and all time when T == 0 use the factor after the last unit / the current one).
inline void SegAdd(TokenEntry* e, float dur, float ov, float f) {
    const float b = dur * ov;
    if (e->n > 0 && e->f[e->n - 1] == f && e->b[e->n - 1] / e->r[e->n - 1] == ov) { e->b[e->n - 1] += b; e->r[e->n - 1] += dur; return; }   // same factor and override: one segment
    if (e->n == kMaxSeg) {   // overflow: merge into the last (b-weighted factor)
        const int k = e->n - 1; const float sb = e->b[k] + b;
        e->f[k] = sb > 0.0f ? (e->b[k] * e->f[k] + b * f) / sb : f; e->b[k] = sb; e->r[k] += dur; return;
    }
    e->b[e->n] = b; e->f[e->n] = f; e->r[e->n] = dur; e->n++;
}
inline void BuildSegs(TokenEntry* e, const float* fu, int T, float fCur) {   // fu[k] = factor after unit k (k = 1..T)
    e->n = 0;
    float pos = 0.0f; int k = 1;
    for (int j = 0; j < g_admN; j++) {
        float left = g_adm[j].raw; const float ov = g_adm[j].ov;
        while (left > 0.0f) {
            float take = left;
            if (k <= T) { const float to = static_cast<float>(k) - g_accBefore - pos; if (to < left) take = to > 0.0f ? to : 0.0f; }
            if (take > 0.0f) SegAdd(e, take, ov, k <= T ? fu[k] : (T > 0 ? fu[T] : fCur));
            pos += take; left -= take;
            if (k <= T && pos >= static_cast<float>(k) - g_accBefore - 1e-6f) { pos = static_cast<float>(k) - g_accBefore; k++; }
            else if (take <= 0.0f) break;   // cannot make progress (rounding): leave the rest to the next boundary test
        }
    }
}
// IMPL-14 step 2 (fix review 1b item E): the one native startup unit of a re-armed token. Its segments are the admitted raw of the LAST unit of this update (the raw between
// the boundaries T - 1 and T of the G accumulator; the whole admitted raw when T == 0, i.e. less than one tick), each piece with its own override, all at factor f.
inline void BuildLastUnitSegs(TokenEntry* e, int T, float f) {
    e->n = 0;
    const float lo = T > 0 ? std::max(0.0f, static_cast<float>(T - 1) - g_accBefore) : 0.0f;
    const float hi = T > 0 ? static_cast<float>(T) - g_accBefore : 3.0e38f;
    float pos = 0.0f;
    for (int j = 0; j < g_admN; j++) {
        const float a = std::max(pos, lo), b = std::min(pos + g_adm[j].raw, hi);
        if (b > a) SegAdd(e, b - a, g_adm[j].ov, f);
        pos += g_adm[j].raw;
    }
}
// Publication-only visit (IMPL-9 item 4): the load `ldr w15,[x14,#0x2c]` at 0xC1F868 is followed by `cbz w15,0xC1F884`, a cbz the installer cannot relocate, so
// no hook can sit after the load. Instead the load is patched to `mov w15,w15` (a no-op, Patch in exl_main) and the hook at 0xC1F868 does the load itself:
// ctx->W[15] = 0 on a publication-only visit (so the native cbz takes its branch to 0xC1F884), else the real +0x2c value. No memory write, no restore hook.
struct TokenHook : exl::hook::impl::InlineHook<TokenHook> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        u8* t = reinterpret_cast<u8*>(ctx->X[14]);
        const u32 active = At<u32>(t, 0x2c);
        ctx->W[15] = active;
        if (g_N == 0) return;
        if (ctx->X[0] != 0) { g_poolBase = At<u64>(reinterpret_cast<void*>(ctx->X[0]), 0x30); g_poolCount = static_cast<u64>(At<s32>(reinterpret_cast<void*>(ctx->X[0]), 0x38)); }
        TokenEntry* e = FindToken(ctx->X[14], true);
        const bool again = e != nullptr && e->epoch == g_N;           // second visit in the epoch
        // IMPL-14 step 2: a re-armed token (TokenRearm) gets exactly ONE unit, the native startup unit, whether or not it was visited in this update. The latch (epoch) stays set,
        // so the units already consumed in this update are never granted again; rearming only invalidates the cached segments.
        const bool rearmed = e != nullptr && e->rearmed;
        if (e != nullptr) { e->epoch = g_N; if (!again || rearmed) e->n = 0; e->rearmed = false; }
        const u64 d = rearmed ? 1 : (again ? 0 : static_cast<u64>(g_T));
        if (rearmed) {
            if (active != 0 && e != nullptr) {
                u8 tmp[0x40]; __builtin_memcpy(tmp, t, sizeof tmp); TokenUnit(tmp);
                BuildLastUnitSegs(e, g_T, TokenFactor(tmp));
            }
        } else if (!again && active != 0) {
            float fu[kMaxSeg]; const int n = static_cast<int>(d < 5 ? d : 5);   // d <= 4 (kMaxStep)
            for (int i = 1; i <= n; i++) {
                if (i < static_cast<int>(d)) { TokenUnit(t); fu[i] = TokenFactor(t); }
                else { u8 tmp[0x40]; __builtin_memcpy(tmp, t, sizeof tmp); TokenUnit(tmp); fu[i] = TokenFactor(tmp); }   // last unit: native runs it
            }
            for (u64 i = 5; i < d; i++) TokenUnit(t);                // never reached (T <= 4); keeps the d-1 rule if kMaxStep grows
            if (e != nullptr) BuildSegs(e, fu, static_cast<int>(d < 5 ? d : 5), TokenFactor(t));
        }
        if (d == 0 && active != 0) ctx->W[15] = 0;                    // publication only
    }
};
// 0xC1F0E8 (set scale: writes +0x24 / +0x20 and clears active, x0 = handle, [x0] = token): the cached segments of this epoch no longer describe the token.
// IMPL-12 item 3(b): drop them (the once-per-epoch latch stays), so a following 0xC1F054 publication uses the new scale natively.
struct TokenSetScale : exl::hook::impl::InlineHook<TokenSetScale> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (ctx->X[0] == 0) return;
        const u64 tok = *reinterpret_cast<u64*>(ctx->X[0]);
        if (tok == 0) return;
        if (TokenEntry* e = FindToken(tok, false)) e->n = 0;
    }
};
// After 0xC1F890 (pool loop): s1 = base * scale becomes the mean factor; 0xC1F844 multiplies the channel budget.
inline void PubDim(u64 tok, float m) {   // IMPL-57: m = final factor, published step = m x channel budget
    TokenEntry* e = FindToken(tok, true);
    if (e == nullptr) return;
    const s32 ch = At<s32>(reinterpret_cast<u8*>(tok), 0x18);
    if (ch < 0) { e->dimOk = false; e->dimConst = false; }
    else if (ch > 4) { e->dim = m; e->dimOk = true; e->dimConst = true; }
    else { e->dim = m * g_dimBud[ch]; e->dimOk = g_dimBudOk; e->dimConst = g_budConst; }
}
struct TokenPoolPub : exl::hook::impl::InlineHook<TokenPoolPub> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { float m; if (MeanFactor(ctx->X[14], m)) ctx->S[1] = m; PubDim(ctx->X[14], ctx->S[1]); }
};
// After 0xC1F070 (single-token publication, x8 = token): s0 = base * scale becomes the mean factor; 0xC1F090 multiplies the channel budget.
struct TokenSinglePub : exl::hook::impl::InlineHook<TokenSinglePub> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { float m; if (MeanFactor(ctx->X[8], m)) ctx->S[0] = m; PubDim(ctx->X[8], ctx->S[0]); }
};
// Before 0xC1F134 / 0xC1F13C (arm: x8 = token): the pre-arming segments are dropped and the next visit is the single native startup unit (IMPL-14 step 2).
struct TokenRearm : exl::hook::impl::InlineHook<TokenRearm> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (g_N != 0) if (TokenEntry* e = FindToken(ctx->X[8], true)) { e->n = 0; e->rearmed = true; } }   // IMPL-14 step 2: the epoch latch is NOT reset
};

// ---- IMPL-10 item 5: manual adapter for the owner 0x1B1A24 (jump table 0x182A66A, state word owner+0x145c, countdown owner+0x23904).
// Hook at 0x1B1A60 (`mov x19,x0` right after the state load; x0 = owner, w8 = state). Credit is per OWNER with carry. Units due d_eff (smart tick):
//   state 12 (0x1B20FC): n = --count; if n <= 0 { count = 30; CC4950(0,1,30; 1,.2,120,120); CC4950(0,0,30; same); state++ }, then body 13
//   state 13 (0x1B2178): AE8F8(owner,0,1,1,1,1); n = --count; if n <= 0 { CC4950(0,1,45; same); CC4950(0,0,45; same); state++ }
// d_eff - 1 units run here in order (a state change makes the rest go to the successor in the same update), the native code runs the last unit.
// d_eff == 0: nothing is due, the dispatch is sent to the common tail (state 15). Any other state (also after a change): the native code runs one
// unit and the rest stays in the owner carry. The old per-site hooks at 0x1B2100 and 0x1B2198 are removed (impl/exclusions.tsv, kind adapter).
namespace owner1b {
constexpr ptrdiff_t kState = 0x145c, kCount = 0x23904;
inline void Expiry(u8* o, int w1, int w2) {   // CC4950(w0=0, w1, w2, s0=1.0, s1=0.2 (0x3e4ccccd), s2=s3=120.0)
    (void)o;
    Game<void (*)(int, int, int, float, float, float, float)>(0xCC4950)(0, w1, w2, 1.0f, std::bit_cast<float>(0x3e4ccccdu), 120.0f, 120.0f);
}
inline void Helper(u8* o) { Game<void (*)(void*, int, float, float, float, float)>(0xAE8F8)(o, 0, 1.0f, 1.0f, 1.0f, 1.0f); }   // the call of 0x1B2190
inline void Body13(u8* o) {
    Helper(o);
    s32 c = At<s32>(o, kCount) - 1;
    At<s32>(o, kCount) = c;
    if (c > 0) return;
    Expiry(o, 1, 45); Expiry(o, 0, 45);
    At<u32>(o, kState) += 1;
}
inline void Unit(u8* o, u32 st) {
    if (st == 12) {
        s32 c = At<s32>(o, kCount) - 1;
        At<s32>(o, kCount) = c;
        if (c <= 0) { At<s32>(o, kCount) = 30; Expiry(o, 1, 30); Expiry(o, 0, 30); At<u32>(o, kState) += 1; }
    }
    Body13(o);
}
}  // namespace owner1b
// IMPL-14 step 4 (fix review 1 item 8, W8): the helper 0xAE8F8 -> ... -> 0xB5D3F0 reads the step through the getter 0xC1F09C. Each C++ unit sets a scoped step override that the
// getter return hooks honour (TailGetter, AnimLiteral): the step of that unit; the native final unit gets what is left of the update's published step.
// UnitSteps: steps u[k] of the ticks k = 1..T of this update and the rest (the fraction after the last tick boundary); built from the TOKEN segments of the owner's holder when it has
// them for this epoch, else from the admitted pieces at factor 1. A step is raw x override x token factor; r[] are the matching raw durations.
struct UnitSteps { float u[kMaxSeg + 1], r[kMaxSeg + 1]; int T; float total, totalRaw; };   // index T = the remainder
inline void ComputeUnitSteps(u8* owner, UnitSteps& us) {
    float b[kMaxSeg], f[kMaxSeg], rr[kMaxSeg]; int n = 0;
    u64 holder = At<u64>(owner, 0x12e0);
    if (holder == 0) holder = *reinterpret_cast<u64*>(exl::util::modules::GetTargetStart() + 0x249C390);
    u64 tok = holder != 0 ? *reinterpret_cast<u64*>(holder) : 0;
    if (TokenEntry* e = tok != 0 ? FindToken(tok, false) : nullptr; e != nullptr && e->epoch == g_N && e->n > 0) {
        for (; n < e->n; n++) { b[n] = e->b[n]; f[n] = e->f[n]; rr[n] = e->r[n]; }
    } else {
        const float tf = tok != 0 ? TokenFactor(reinterpret_cast<u8*>(tok)) : 1.0f;   // IMPL-20: no segments this epoch = the token's current published factor (inactive token fixed at 0.5 -> 0.5)
        for (int j = 0; j < g_admN && n < kMaxSeg; j++, n++) { b[n] = g_adm[j].raw * g_adm[j].ov; f[n] = tf; rr[n] = g_adm[j].raw; }
    }
    const int T = std::min(g_T, kMaxSeg);
    us = {}; us.T = T;
    int k = 0; float pos = 0.0f, next = T >= 1 ? 1.0f - g_accBefore : 3.0e38f;
    for (int j = 0; j < n; j++) {
        float left = rr[j]; const float rate = rr[j] > 0.0f ? b[j] / rr[j] * f[j] : 0.0f;
        while (left > 0.0f) {
            const float to = k < T ? next - pos : 3.0e38f;
            const float take = std::min(left, std::max(to, 0.0f));
            if (take > 0.0f) { us.u[k] += take * rate; us.r[k] += take; pos += take; left -= take; }
            if (k < T && (take <= 0.0f || pos >= next - 1e-6f)) { k++; next = static_cast<float>(k + 1) - g_accBefore; }
            else if (take <= 0.0f) break;
        }
    }
    for (int i = 0; i <= T; i++) { us.total += us.u[i]; us.totalRaw += us.r[i]; }
}
inline int UnitBudgets(u8* owner, float* b, float* r) {
    UnitSteps us; ComputeUnitSteps(owner, us);
    int n = 0;
    for (int k = 0; k <= us.T && n < 17; k++) if (us.r[k] > 0.0f || us.u[k] > 0.0f) { b[n] = us.u[k]; r[n] = us.r[k]; n++; }
    return n;
}
static_assert(kMaxSeg + 1 == 17, "UnitBudgets arrays");
inline void OwnerOverride(ThreadCtx& t, float step, float raw) { t.ovOn = true; t.ov = step > 0.0f ? step : 0.0f; t.ovRaw = raw > 0.0f ? raw : 0.0f; }
// Wrapper of the whole owner function 0x1B1A24: the override and the credit cap live exactly as long as one call (nested calls restore the outer values).
template <uintptr_t Addr>
struct OwnerScope : exl::hook::impl::TrampolineHook<OwnerScope<Addr>> {
    static u64 Callback(u64 x0, u64 x1, u64 x2, u64 x3, u64 x4, u64 x5, u64 x6, u64 x7,
                        float64x2_t v0, float64x2_t v1, float64x2_t v2, float64x2_t v3,
                        float64x2_t v4, float64x2_t v5, float64x2_t v6, float64x2_t v7) {
        ThreadCtx& t = Thr();
        const bool o = t.ovOn; const float ov = t.ov, ovr = t.ovRaw; const int cc = t.credCap1;
        t.ovOn = false; t.credCap1 = 0;
        u64 r = OwnerScope<Addr>::Orig(x0, x1, x2, x3, x4, x5, x6, x7, v0, v1, v2, v3, v4, v5, v6, v7);
        if (t.credCap1 != 0) __atomic_fetch_sub(&g_capOn, 1, __ATOMIC_RELAXED);
        t.ovOn = o; t.ov = ov; t.ovRaw = ovr; t.credCap1 = cc;
        return r;
    }
};
u32 g_ownerFrac = 0, g_ownerCapSet = 0;
struct OwnerHook : exl::hook::impl::InlineHook<OwnerHook> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        u8* o = reinterpret_cast<u8*>(ctx->X[0]);
        u32 st = static_cast<u32>(ctx->W[8]);
        if (st != 12 && st != 13) return;
        const u64 now = Now<false>();
        Counter& c = FindCounter(0x1B1A60, ctx->X[0], now);
        const bool held = c.U == g_N - 1;
        u64 dt = held ? now - c.S : 1;
        if (!held) c.carry = 0;
        if (dt > 4) dt = 4;
        s64 de = static_cast<s64>(dt) + c.carry;
        c.carry = 0;
        if (de > 8) { c.carry = de - 8; de = 8; }
        c.S = now; c.U = g_N;
        ThreadCtx& t = Thr();
        UnitSteps us; ComputeUnitSteps(o, us);
        if (de == 0) {
            // no whole tick is due: the continuous helper work still gets the fractional step of this update (one call, override = the fractional step; the discrete effects of the
            // helper run once per update then). The dispatch goes to the common tail (state 15).
            if (g_step > 0.0f && us.total > 0.0f) {
                OwnerOverride(t, us.total, g_step); g_ownerFrac++;
                owner1b::Helper(o);
                t.ovOn = false;
            }
            ctx->W[8] = 15; return;
        }
        s64 rem = de;
        float used = 0.0f, usedRaw = 0.0f; s64 unit = 0;
        while (rem > 1 && (st == 12 || st == 13)) {
            const bool inT = unit < us.T;   // ticks beyond this update's T (carry units) get one nominal tick
            const float stp = inT ? us.u[unit] : (us.T > 0 ? us.u[us.T - 1] : us.total), rw = inT ? us.r[unit] : (us.T > 0 ? us.r[us.T - 1] : us.totalRaw);
            OwnerOverride(t, stp, rw);
            owner1b::Unit(o, st);
            used += stp; usedRaw += rw; unit++;
            rem--; st = At<u32>(o, owner1b::kState);
        }
        if (unit > 0) OwnerOverride(t, us.total - used, us.totalRaw - usedRaw);   // the native final unit (or the successor) gets the rest of the update
        if (rem > 1) {   // left the countdown states: the successor's site hooks may charge only the units left (no double charge)
            t.credCap1 = static_cast<int>(rem) + 1; __atomic_fetch_add(&g_capOn, 1, __ATOMIC_RELAXED); g_ownerCapSet++;
        }
        ctx->W[8] = st;
    }
};
// ---- IMPL-26 items 2 and 3: random input picker 0x55BA00 (called only at 0x55B280 inside 0x55B058) and the unit transaction around it.
// DueTicks: whole 1/60 s ticks due for (site, field) in this update, the Credit rule: held (visited in the previous update) = ticks since the last visit (<= 4), else 1; 0 = none due.
// Inside a credit cap (g_capOn) the result is cut to the cap, as Credit does.
template <bool Tail>
inline u64 DueTicks(u64 site, u64 field) {
    const Clk clk = ClockNow<Tail>();
    if (clk.n == 0) return 1;
    Counter tmp;
    Counter& c = CounterFor(site, field, clk.n, clk.now, tmp);
    u64 dt = c.U == clk.n - 1 ? clk.now - c.S : 1;
    if (dt > 4) dt = 4;
    if (g_capOn != 0 && ThreadId() == g_logicThread) { const int cap1 = Thr().credCap1; if (cap1 > 0 && dt > static_cast<u64>(cap1 - 1)) dt = cap1 - 1; }
    c.S = clk.now; c.U = clk.n;
    return dt;
}
// Item 2. Cache of the last native draw results per actor (x19 = x0 of 0x55BA00). open addressed, 8 probes, the oldest entry (smallest n) is replaced.
// ponytail: 256 actors; a lost entry only means one native draw. n == 0 visit: both LCG results come from the cache and the two global state stores (0x55BAEC, 0x55BB28)
// write the unchanged state back (the store register is set to the old state value just before the store: no write-and-restore of memory).
struct RpEntry { u64 actor, n; u32 r1, r2; bool have; };
constexpr int kRp = 256;
RpEntry g_rp[kRp];
u32 g_rpCached = 0, g_rpNative = 0, g_pairUnits = 0;
RpEntry* RpFind(u64 actor, bool create) {
    RpEntry* old = nullptr;
    const u64 h = (actor / 16) * 0x9E3779B97F4A7C15ull >> 20;
    for (int i = 0; i < kProbes; i++) {
        RpEntry& e = g_rp[(h + i) % kRp];
        if (e.actor == actor) return &e;
        if (e.actor == 0) { old = &e; break; }
        if (old == nullptr || e.n < old->n) old = &e;
    }
    if (!create) return nullptr;
    if (old->actor != 0 && old->n + 1 >= g_N) g_pickEvictLive++;   // IMPL-52: pickEvictLive
    *old = {actor, 0, 0, 0, false};
    return old;
}
// 0x55BAA0 (behind `madd w17` = first LCG result, before `lsr w0,w17,#8`).
struct PickDraw1 : exl::hook::impl::InlineHook<PickDraw1> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        t.rpMode = 0; t.rpActor = 0;
        if (g_N == 0 || ThreadId() != g_logicThread) return;
        const u64 actor = ctx->X[19];
        const u64 n = DueTicks<IsTail(0x55BAA0)>(0x55BA00, actor);
        RpEntry* e = RpFind(actor, false);
        if (n == 0 && e != nullptr && e->have) { t.rpMode = 1; t.rpR2 = e->r2; ctx->W[17] = e->r1; g_rpCached++; return; }
        g_rpNative++;
        e = RpFind(actor, true);
        e->n = g_N; e->r1 = static_cast<u32>(ctx->W[17]); e->have = false;
        t.rpActor = actor;
    }
};
// 0x55BAEC / 0x55BB28 (before `str w17,[x14]` / `str w11,[x14]`): a cached visit stores the old state value back. The first one may clobber w17 (dead after 0x55BB24 reads it,
// and the second draw is replaced anyway).
template <int Reg>
struct PickStoreR : exl::hook::impl::InlineHook<PickStoreR<Reg>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (Thr().rpMode != 0) ctx->W[Reg] = *reinterpret_cast<u32*>(ctx->X[14]); }
};
// 0x55BB2C (behind the second `madd w11`, before `lsr w11,w11,#8`): cached visit = cached second result; native visit = remember it.
struct PickDraw2 : exl::hook::impl::InlineHook<PickDraw2> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        if (t.rpMode != 0) { ctx->W[11] = t.rpR2; t.rpMode = 0; return; }
        if (t.rpActor != 0) { if (RpEntry* e = RpFind(t.rpActor, false)) { e->r2 = static_cast<u32>(ctx->W[11]); e->have = true; } t.rpActor = 0; }
    }
};
// Item 3. The unit transaction of the caller 0x559EBC: `bl 0x55A7EC` (0x55A400) then `bl 0x55B058` (0x55A40C), both with x0 = x19 (the picker/state block), x1 = x20 (the actor);
// no other argument, return values unused, nothing between them (impl/IMPL-26-facts.md). PairBegin (hook on the first bl, before it): n = ticks due for key (0x55A400, x19).
// n >= 2: units 1..n-1 run here from module code, each with the scoped step override of that unit (ComputeUnitSteps of the actor), HandlerGate forced to one call (forceOne) and the
// smart-tick credit capped at one unit; the native pair then runs the last unit with the rest of the step. PairEnd (0x55A410, after the second bl) restores everything.
// n <= 1: untouched native. A pair entered while a replay is active is native.
struct PairBegin : exl::hook::impl::InlineHook<PairBegin> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread) return;
        ThreadCtx& t = Thr();
        if (t.rpActive != 0) return;
        const u64 n = DueTicks<IsTail(0x55A400)>(0x55A400, ctx->X[19]);
        if (n < 2) return;
        UnitSteps us; ComputeUnitSteps(reinterpret_cast<u8*>(ctx->X[20]), us);
        t.rpActive = 1; t.rpOvOn = t.ovOn; t.rpOv = t.ov; t.rpOvRaw = t.ovRaw; t.rpCap1 = t.credCap1;
        if (t.credCap1 == 0) { t.credCap1 = 2; __atomic_fetch_add(&g_capOn, 1, __ATOMIC_RELAXED); }   // credit cap 1 (cap1 = 2) for every counter inside the transaction
        else if (t.credCap1 > 2) t.credCap1 = 2;
        t.forceOne++;
        float used = 0.0f, usedRaw = 0.0f;
        for (u64 i = 0; i + 1 < n; i++) {
            const bool inT = static_cast<int>(i) < us.T;
            const float stp = inT ? us.u[i] : (us.T > 0 ? us.u[us.T - 1] : us.total), rw = inT ? us.r[i] : (us.T > 0 ? us.r[us.T - 1] : us.totalRaw);
            OwnerOverride(t, stp, rw);
            Game<void (*)(u64, u64)>(0x55A7EC)(ctx->X[19], ctx->X[20]);
            Game<void (*)(u64, u64)>(0x55B058)(ctx->X[19], ctx->X[20]);
            used += stp; usedRaw += rw; g_pairUnits++;
        }
        OwnerOverride(t, us.total - used, us.totalRaw - usedRaw);   // the native last unit gets the rest of the update
    }
};
struct PairEnd : exl::hook::impl::InlineHook<PairEnd> {
    static void Callback(exl::hook::InlineFloatCtx*) {
        ThreadCtx& t = Thr();
        if (t.rpActive == 0) return;
        t.rpActive = 0;
        t.ovOn = t.rpOvOn; t.ov = t.rpOv; t.ovRaw = t.rpOvRaw;
        if (t.rpCap1 == 0) { t.credCap1 = 0; __atomic_fetch_sub(&g_capOn, 1, __ATOMIC_RELAXED); } else t.credCap1 = t.rpCap1;
        if (t.forceOne > 0) t.forceOne--;
    }
};
// Segments of the update for the actor `actor` (token holder at actor+0x12e0, default GOT 0x249C390): n, weights w_i = b_i f_i / sum(b f) and the real raw durations r_i
// of the TOKEN entry of this update, else one segment of the whole raw step. s_i = (published step) x w_i, sigma_i = s_i / r_i.
// IMPL-14 step 5(a): units = true cuts the segments at the native unit (integer tick) boundaries and at the override boundaries also when the token has no segments (BuildSegs on the
// admitted pieces at factor 1), so a per-unit motion/collision sees every native unit of the update.
// IMPL-30 item 0 (fix review 6 item 7): SegAdd merges adjacent pieces with an equal factor and an equal override, also across a unit boundary. With units = true every
// unit boundary k - g_accBefore (k = 1..T, raw time) that lies inside a segment cuts it in two again (w and r in proportion to the raw time); SegAdd itself is unchanged for the other users.
// A segment is cut only when both parts are longer than 1e-6 raw; at kMaxSeg segments no further cut is made.
// IMPL-33 item 1 (fix review 9): cl[i] = the number of tick boundaries the output segment i CLOSES (an integer, no float tolerance later). Boundary k belongs to the segment whose end is within
// the cut tolerance 1e-6 of it (no cut then), else to the left part of the cut made at k. A boundary beyond the total length (not completed) belongs to nobody; with n == kMaxSeg (no cut possible) it
// belongs to the segment that contains it.
u32 g_closeMulti = 0;
inline int SplitUnitBoundaries(float (&w)[kSegCap], float (&r)[kSegCap], int n, int* cl = nullptr) {
    int c[kSegCap + 1] = {};
    for (int k = 1; k <= g_T && k <= kSegCap - kMaxSeg; k++) {   // IMPL-35 item 1: at most 8 boundaries; each needs one cut slot, and kSegCap leaves 8 beyond the kMaxSeg input
        const float bd = static_cast<float>(k) - g_accBefore;
        float pos = 0.0f;
        for (int i = 0; i < n; i++) {
            const float end = pos + r[i];
            if (bd > end + 1.0e-6f) { pos = end; continue; }               // later segment
            if (bd > pos + 1.0e-6f && bd < end - 1.0e-6f && n < kSegCap) {   // inside: cut, the left part closes k
                const float r1 = bd - pos, r2 = r[i] - r1, wi = w[i], ri = r[i];
                for (int j = n; j > i + 1; j--) { w[j] = w[j - 1]; r[j] = r[j - 1]; c[j] = c[j - 1]; }
                w[i] = wi * (r1 / ri); w[i + 1] = wi * (r2 / ri); r[i] = r1; r[i + 1] = r2; c[i + 1] = 0; n++;
            }
            c[i]++;                                                         // the end is on the boundary (or no room to cut): this segment closes k
            break;
        }
    }
    for (int i = 0; i < n; i++) if (c[i] > 1) g_closeMulti++;   // must stay 0: with room for every cut no segment closes two boundaries
    if (cl != nullptr) for (int i = 0; i < n; i++) cl[i] = c[i];
    return n;
}
inline int LoadSegs(u8* actor, float (&w)[kSegCap], float (&r)[kSegCap], bool units = false, int* cl = nullptr) {
    int n = 1; w[0] = 1.0f; r[0] = g_step;
    u64 holder = At<u64>(actor, 0x12e0);
    if (holder == 0) holder = *reinterpret_cast<u64*>(exl::util::modules::GetTargetStart() + 0x249C390);
    u64 tok = holder != 0 ? *reinterpret_cast<u64*>(holder) : 0;
    if (TokenEntry* e = tok != 0 ? FindToken(tok, false) : nullptr; e != nullptr && e->epoch == g_N && e->n > 0) {
        float sum = 0.0f;
        for (int i = 0; i < e->n; i++) sum += e->b[i] * e->f[i];
        if (sum > 0.0f) { n = e->n; for (int i = 0; i < n; i++) { w[i] = e->b[i] * e->f[i] / sum; r[i] = e->r[i]; } return units ? SplitUnitBoundaries(w, r, n, cl) : n; }
    }
    if (units && g_admN > 0) {
        TokenEntry tmp; float fu[kMaxSeg]; for (float& x : fu) x = 1.0f;
        BuildSegs(&tmp, fu, std::min(g_T, kMaxSeg - 1), 1.0f);
        float sum = 0.0f; for (int i = 0; i < tmp.n; i++) sum += tmp.b[i] * tmp.f[i];
        if (tmp.n > 0 && sum > 0.0f) { n = tmp.n; for (int i = 0; i < n; i++) { w[i] = tmp.b[i] * tmp.f[i] / sum; r[i] = tmp.r[i]; } }
    }
    return units ? SplitUnitBoundaries(w, r, n, cl) : n;
}
// Calibrated damping (review rounds 8, 9; IMPL-10 item 5): velocity q = prod (1 - 0.1 sigma_i)^r_i over the segments. No time (r = 0): q = 1.
inline float DampQ(u8* owner, float s) {
    if (!(g_step > 0.0f)) return 1.0f;
    float w[kSegCap], r[kSegCap]; const int n = LoadSegs(owner, w, r);
    float q = 1.0f;
    for (int i = 0; i < n; i++) {
        if (!(r[i] > 0.0f)) continue;
        float base = 1.0f - 0.1f * (s * w[i] / r[i]);
        q *= __builtin_powf(base > 0.0f ? base : 0.0f, r[i]);
    }
    return q;
}
// IMPL-23 item 6 (fix review 3): calibrated map of the step-scaled smoothers 0x490A8 (x += 0.08 g (1-x)) and 0x1158AC (y += 0.1 g (T-y)); g = actor+0x160D0.
// new = T + (old - T) prod_i (1 - gain sigma_i)^r_i over the LoadSegs segments, sigma_i = g w_i / r_i; r_i == 0 skipped (no time: unchanged). One segment, r == 1, sigma == 1: native.
inline bool CalMap(u8* actor, float gain, float g, float old, float target, float& out) {
    if (g_N == 0 || !(g > 0.0f)) return false;
    float w[kSegCap], r[kSegCap]; const int n = LoadSegs(actor, w, r);
    if (n == 1 && r[0] == 1.0f && g * w[0] / r[0] == 1.0f) return false;
    float prod = 1.0f;
    for (int i = 0; i < n; i++) {
        if (!(r[i] > 0.0f)) continue;
        const float base = 1.0f - gain * (g * w[i] / r[i]);
        prod *= __builtin_powf(base > 0.0f ? base : 0.0f, r[i]);
    }
    out = target + (old - target) * prod;
    return true;
}
// Seam hooks BEFORE the final `fadd sX, s1, sDelta` (s1 = old): the delta register becomes new - old. Target: fixed 1 (TFix) or the value stashed by TargetStash.
template <uintptr_t Addr, int DReg, int ABase, ptrdiff_t ActorOff, ptrdiff_t GOff, u32 GainBits, bool Stash>
struct CalSeam : exl::hook::impl::InlineHook<CalSeam<Addr, DReg, ABase, ActorOff, GOff, GainBits, Stash>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        float out;
        const float old = ctx->S[1], target = Stash ? Thr().stT : 1.0f;
        if (CalMap(reinterpret_cast<u8*>(ctx->X[ABase] - ActorOff), std::bit_cast<float>(GainBits), *reinterpret_cast<float*>(ctx->X[ABase] + GOff), old, target, out))
            ctx->S[DReg] = out - old;
    }
};
struct TargetStash : exl::hook::impl::InlineHook<TargetStash> {   // 0x1158AC, before `fsub s0,s0,s1`: s0 = target T
    static void Callback(exl::hook::InlineFloatCtx* ctx) { Thr().stT = ctx->S[0]; }
};
// IMPL-27 item 6: 0x1158BC, y += 0.1 s (T - y) with T a function of the distance D to the target object (x0 at the hook): D > 60: T = 1; else T = min(1.8, (80 - D) 0.05 + 1).
// In the states 103..105 ([x22], set at 0x1157A4) the same call first moves the position (x19+0x50) by 0.26 dir (x19+0x30) s (s = [x21+0x1BC], 0x1157C4..0x1157F8), so D changes
// per unit. The movement is in reach of this hook (same function), so per unit j: position_j = position_end - 0.26 dir (s - sum_{i<=j} s_i), D_j = |target - position_j| (xyz, as native),
// T_j = target(D_j), then the calibrated step of that unit y = T_j + (y - T_j)(1 - 0.1 sigma_j)^r_j, sigma_j = s_j / r_j, over UnitBudgets (s_j = b_j). One unit with r == 1 and
// sigma == 1 is native. Other states keep the fixed-target map (CalMap with the stashed T).
inline float Target1158(float D) {
    if (D > 60.0f) return 1.0f;
    float t = (80.0f - D) * 0.0500000007f + 1.0f;
    return t > 1.79999995f ? 1.79999995f : t;
}
struct CalMove : exl::hook::impl::InlineHook<CalMove> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const u32 state = *reinterpret_cast<u32*>(ctx->X[22]);
        float old = ctx->S[1];
        float out;
        u8* actor = reinterpret_cast<u8*>(ctx->X[21] - 0x15F14);
        const float gain = 0.100000001f, g = *reinterpret_cast<float*>(ctx->X[21] + 0x1BC);
        float ub[17], ur[17]; const int n = (state - 0x67u <= 2u && g > 0.0f) ? UnitBudgets(actor, ub, ur) : 0;
        const u8* o = reinterpret_cast<const u8*>(ctx->X[0]); const u8* me = reinterpret_cast<const u8*>(ctx->X[19]);
        // IMPL-30 item 6: UnitBudgets counts raw x override x token factor, but this code multiplies by the PUBLISHED step g (= base x scale x channel budget, [x21+0x1BC]); the unit budgets are
        // normalised to it: b_j' = b_j g / sum(b) (example: raw 2, factor 1, channel scale 0.5, g 1 -> steps [0.5, 0.5], result 0.0975, not 0.19). One unit: exactly g.
        if (n >= 1) { float sb = 0.0f; for (int j = 0; j < n; j++) sb += ub[j]; if (n == 1) ub[0] = g; else if (sb > 0.0f) for (int j = 0; j < n; j++) ub[j] = (ub[j] * g) / sb; }
        constexpr uintptr_t kCalSite = 0x1158BC;
        if (n >= 1 && o != nullptr) {
            // IMPL-34 item 3 (fix review 10, replaces IMPL-32 item 4): the movement, the target selection and the filter are ONE anchored unit that KEEPS the work it consumed. Anchor (v[0..2] =
            // position pa at the last whole-unit boundary, v[3] = filter ya there, v[4] = progress u, v[5] = the filter value reported, v[6] = Bc = the budget consumed since the boundary; the
            // movement applied is mv x Bc, the same budget). Each visit moves only its own piece at its own rate sg = budget per unit of progress (Bc += sg x take). The unit completes with ONE
            // native step of the TOTAL consumed budget Bc: move mv Bc, select the target from the post-move distance (native order), ya += gain Bc (T - ya). Never the latest rate x a whole unit.
            // The visible value before completion = ya + u (completion value with Bc + sg (1 - u)), from the current state. The position field itself is moved by the native code.
            const float f = StepFactor<IsTail(kCalSite)>(kCalSite, ctx->X[21] + 0x1BC);
            Anchor spill; Anchor* st = AnchorOpen(kCalSite, ctx->X[21], spill);
            const bool valid = st->n != 0 && st->n == g_N - 1 && SameBits(st->v[5], old);
            float mv[3], pos[3];
            for (int i = 0; i < 3; i++) mv[i] = std::bit_cast<float>(0x3e851eb8u) * *reinterpret_cast<const float*>(me + 0x30 + 4 * i);
            double u = valid ? st->v[4] : 0.0, Bc = valid ? st->v[6] : 0.0;
            double Mc[3] = {valid ? st->v[7] : 0.0, valid ? st->v[8] : 0.0, valid ? st->v[9] : 0.0};   // IMPL-36 item 1: accumulated movement vector since the boundary
            const bool late = LateStart(valid, st->whole, f);
            float ya = valid ? st->v[3] : old;
            for (int i = 0; i < 3; i++) pos[i] = valid ? st->v[i] : *reinterpret_cast<const float*>(me + 0x50 + 4 * i) - mv[i] * g;   // not valid: the position at the start of this visit
            if (f == 1.0f && u == 0.0 && n == 1 && ur[0] == 1.0f && ub[0] == 1.0f && g == 1.0f) { st->active = false; st->n = 0; return; }   // one native unit
            const bool wh = st->whole || late;
            const double fw = wh ? static_cast<double>(WholeCrossed<IsTail(kCalSite)>()) : static_cast<double>(f);
            double sr = 0.0; for (int j = 0; j < n; j++) sr += ur[j];
            double sg = 0.0;
            auto unitEnd = [&](double B, const double* M, float* pe) {   // ONE native unit with the total budget B and the accumulated movement M, from (pos, ya)
                const float Bf = static_cast<float>(B);
                for (int i = 0; i < 3; i++) pe[i] = pos[i] + static_cast<float>(M[i]);
                float dd[3];
                for (int i = 0; i < 3; i++) dd[i] = *reinterpret_cast<const float*>(o + 0x50 + 4 * i) - pe[i];
                const float p0 = dd[0] * dd[0], p1 = dd[1] * dd[1], p2 = dd[2] * dd[2];   // IMPL-30 item 6: native reduction order
                const float sq = (p2 + p0) + (0.0f + p1);
                return ya + (gain * Bf) * (Target1158(__builtin_sqrtf(sq)) - ya);
            };
            auto advance = [&](double left) {
                for (int guard = 0; left > 0.0 && guard < 64; guard++) {
                    const double room = 1.0 - u, take = left < room ? left : room;
                    u += take; Bc += sg * take; left -= take;
                    for (int i = 0; i < 3; i++) Mc[i] += static_cast<double>(mv[i]) * (sg * take);   // IMPL-36 item 1: this piece at its own direction
                    if (u >= 1.0 - 1.0e-6) { float pe[3]; ya = unitEnd(Bc, Mc, pe); for (int i = 0; i < 3; i++) { pos[i] = pe[i]; Mc[i] = 0.0; } u = 0.0; Bc = 0.0; }
                }
            };
            for (int j = 0; j < n; j++) {
                const double lenU = sr > 0.0 ? ur[j] * (static_cast<double>(f) / sr) : 0.0;   // progress of the piece in this update (unscaled: the rate)
                if (!(lenU > 0.0)) continue;
                sg = ub[j] / lenU;
                advance(lenU * (wh ? (f > 0.0f ? fw / f : 0.0) : 1.0));
            }
            if (late) advance(PhaseAfter<IsTail(kCalSite)>());   // IMPL-34 item 1
            out = ya;
            if (u > 0.0) { float pe[3]; double Mv[3]; for (int i = 0; i < 3; i++) Mv[i] = Mc[i] + static_cast<double>(mv[i]) * (sg * (1.0 - u)); const float ye = unitEnd(Bc + sg * (1.0 - u), Mv, pe); out = ya + static_cast<float>(u) * (ye - ya); }
            st->v[0] = pos[0]; st->v[1] = pos[1]; st->v[2] = pos[2]; st->v[3] = ya; st->v[4] = static_cast<float>(u); st->v[5] = out; st->v[6] = static_cast<float>(Bc); for (int i = 0; i < 3; i++) st->v[7 + i] = static_cast<float>(Mc[i]); st->n = g_N; st->active = u > 0.0;
        } else if (!CalMap(actor, gain, g, old, Thr().stT, out)) return;
        ctx->S[3] = out - old;
    }
};

// Hook at 0x1B1D2C (`ldr q2,[x19,#0x50]`, state 5): v1 = velocity, v0 = native displacement (0.1 s v). v0 becomes v_old - v_old q, so position += v_old - v_new
// and the native `fsub v0,v1,v0` leaves v_new = v_old q.
struct OwnerDamp : exl::hook::impl::InlineHook<OwnerDamp> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        const float q = DampQ(reinterpret_cast<u8*>(ctx->X[19]), At<float>(reinterpret_cast<void*>(ctx->X[22]), 0x44));
        float* d = reinterpret_cast<float*>(&ctx->Q[0]);
        const float* v = reinterpret_cast<const float*>(&ctx->Q[1]);
        for (int i = 0; i < 4; i++) d[i] = v[i] * (1.0f - q);
    }
};

// ---- IMPL-10 item 6 (first part): calibrated kick-drift of 0x2978A4 in phases 2 and 3 (0x297AE8..0x297B20): v += a s; distance = v_new s (a = 0.01, s3 = step).
// Segment i (s_i, r_i, sigma_i = s_i / r_i): v += a s_i; x += v_new_i s_i - 0.5 a s_i sigma_i (r_i - 1); the sum of the segment distances replaces the native
// distance s2 = v_new s (arrival test and snap at 0x297B34 use it). One segment with r = 1 is left native. KdEnter keeps the actor (x1) of the call for the token.
// Not done yet (listed in the ticket report): phase 5 drift 0x297D88..0x297DB0, collision 0x297E4C and factor 0.85 0x297E8C between segments, phase timer
// +0x54 crossing and the successor-phase routing (table 0x182CA0C, 0x297F40).
u64 g_kdActor = 0;
struct KdEnter : exl::hook::impl::InlineHook<KdEnter> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { g_kdActor = ctx->X[1]; }
};
struct KickDrift : exl::hook::impl::InlineHook<KickDrift> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || !(g_step > 0.0f) || g_kdActor == 0) return;
        float w[kSegCap], r[kSegCap]; const int n = LoadSegs(reinterpret_cast<u8*>(g_kdActor), w, r);
        if (n == 1 && r[0] == 1.0f) return;                      // native reference update
        const float a = std::bit_cast<float>(0x3c23d70au), s = ctx->S[3];
        const float vOld = ctx->S[0] - a * s;                    // S[0] = v_new here
        float acc = 0.0f, dist = 0.0f;
        for (int i = 0; i < n; i++) {
            if (!(r[i] > 0.0f)) continue;
            const float si = s * w[i], sigma = si / r[i];
            acc += si;
            dist += (vOld + a * acc) * si - 0.5f * a * si * sigma * (r[i] - 1.0f);
        }
        ctx->S[2] = dist;
    }
};

// ---- IMPL-11 item 1 (b, c): phase 5 of 0x2978A4 (also entered from phase 4 in the same call): drift 0x297D88..0x297DB0, collision 0x297E4C, factor 0.85 0x297E8C,
// timer +0x54 (0x297E9C), expiry branch 0x297EA8 -> phase +0x30 + 1 (0x297F40/0x297F44, table 0x182CA0C: phase 6 = 0x2979C4, which uses no time).
// P5Run replaces the native single step: per segment (s_i = S w_i, r ticks of sigma_i = s_i / r each): vy += a s; pos += v s (y corrected by -0.5 a s sigma (r - 1));
// native ray query 0x749390(result, old pos, new pos, 2) and, on a hit, v = (v - n 2 (v.n)) 0.85; timer -= s. When the timer runs out inside a segment the motion stops at
// the expiry tick m = ceil(timer / sigma) (the native tick count), and the leftover ticks (r - m) + later segments are returned: with >= 1 whole tick left the successor
// phase 6 runs in the same update. The hook at 0x297D88 does all this, then makes the native code below a no-op: s3 = 0 for the drift, pos in x8/x9 (and in memory),
// stack copy of the step = 0 for the timer; 0x297DC8 gives the rotation its step back, the native collision call 0x297E4C is patched to `mov w0,wzr` (IMPL-14 step 5b: P5Run does the per-unit queries),
// 0x297EA8 starts the successor (phase 6 = a nested native call with step 0) and clears the branch.
struct P5State { float v[4]; float pos[4]; float timer; };
struct P5Out { float used; bool expired; float left; };
inline bool P5Collide(const float* p0, const float* p1, float* v) {
    alignas(16) float a[4], b[4], res[0x130 / 4];
    __builtin_memcpy(a, p0, 16); __builtin_memcpy(b, p1, 16);
    if (Game<int (*)(void*, const void*, const void*, int)>(0x749390)(res, a, b, 2) == 0) return false;
    const float* n = res + 4;                                  // result + 0x10
    float pr[4];
    for (int i = 0; i < 4; i++) pr[i] = v[i] * n[i];
    pr[3] = 0.0f;
    float d = (pr[2] + pr[0]) + (pr[3] + pr[1]);               // ext, fadd v2s, faddp (native lane order)
    d = d + d;
    for (int i = 0; i < 4; i++) v[i] = (v[i] - n[i] * d) * 0.85f;
    return true;
}
template <typename Col>
inline P5Out P5Run(P5State& st, float S, const float* w, int n, const float* rs, Col col) {
    constexpr float a = -0.01f;
    P5Out o{0.0f, false, 0.0f};
    for (int i = 0; i < n; i++) {
        const float si = S * w[i], r = rs[i];
        if (!(si > 0.0f) || !(r > 0.0f)) continue;             // a segment without game speed: time passes, nothing moves
        const float sigma = si / r;
        float m = r, se = si;
        const bool exp = (st.timer - si <= 0.0f);
        if (exp) { m = std::min(r, std::max(1.0f, std::ceil(st.timer / sigma))); se = m * sigma; }
        float old[4], nw[4];
        __builtin_memcpy(old, st.pos, 16);
        st.v[1] += a * se;
        for (int k = 0; k < 4; k++) nw[k] = old[k] + st.v[k] * se;
        nw[1] -= 0.5f * a * se * sigma * (m - 1.0f);
        col(old, nw, st.v);
        __builtin_memcpy(st.pos, nw, 16);
        st.pos[3] = 1.0f;
        st.timer -= se;
        o.used += se;
        if (exp) {
            if (st.timer > 0.0f) st.timer = 0.0f;
            o.expired = true;
            o.left = r - m; for (int k = i + 1; k < n; k++) o.left += rs[k];
            break;
        }
    }
    return o;
}
struct P5Pending { bool active; float used; bool expired; float left; };
P5Pending g_p5 = {};
struct P5Drift : exl::hook::impl::InlineHook<P5Drift> {   // 0x297D88: x19 = this (vector +0x40, timer +0x54), x20 = entity (position +0x30), s3 = step
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        g_p5.active = false;
        const float S = ctx->S[3];
        // IMPL-14 step 5(b): the native collision query is patched out (0x297E4C), so the module ALWAYS handles phase 5, also one unit of r = 1 (the 60 FPS reference, python test:
        // impl/test_owner.py, one unit equals the native sequence). Only S <= 0 (nothing moves) is left to the native code. Before the clock starts (g_N == 0) one native unit.
        if (!(S > 0.0f)) return;
        u8* self = reinterpret_cast<u8*>(ctx->X[19]); u8* ent = reinterpret_cast<u8*>(ctx->X[20]);
        float w[kSegCap], rr[kSegCap]; int n = 1; w[0] = 1.0f; rr[0] = 1.0f;
        if (g_N != 0 && g_step > 0.0f) n = LoadSegs(reinterpret_cast<u8*>(g_kdActor), w, rr, true);
        P5State st;
        __builtin_memcpy(st.v, self + 0x40, 16); __builtin_memcpy(st.pos, ent + 0x30, 16); st.timer = At<float>(self, 0x54);
        const P5Out o = P5Run(st, S, w, n, rr, P5Collide);
        __builtin_memcpy(self + 0x40, st.v, 16); __builtin_memcpy(ent + 0x30, st.pos, 16); At<float>(self, 0x54) = st.timer;
        __builtin_memcpy(&ctx->X[8], st.pos, 8); __builtin_memcpy(&ctx->X[9], st.pos + 2, 8);
        At<float>(reinterpret_cast<void*>(ctx->X[29] - 0x1a0), 0) = 0.0f;   // native frame: the step copy that 0x297E94 reloads for the timer
        ctx->S[3] = 0.0f;
        g_p5 = {true, o.used, o.expired, o.left};
    }
};
struct P5Rotate : exl::hook::impl::InlineHook<P5Rotate> {  // 0x297DC8: the rotation angle 15 s3 uses the step really moved
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (g_p5.active) ctx->S[3] = g_p5.used; }
};
struct P5Expiry : exl::hook::impl::InlineHook<P5Expiry> {  // 0x297EA8 (b.ls expiry): any positive budget left -> phase 6 now (nested native call, step 0)
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (!g_p5.active) return;
        g_p5.active = false;
        if (!g_p5.expired || !(g_p5.left > 0.0f)) return;       // IMPL-14 step 5(c): any positive budget left -> phase 6 now (it uses no time); none left: native, phase 6 next update
        void* self = reinterpret_cast<void*>(ctx->X[19]);
        At<u32>(self, 0x30) = 6;
        Game<void (*)(void*, void*, float)>(0x2978A4)(self, reinterpret_cast<void*>(g_kdActor), 0.0f);
        ctx->NZCV = (ctx->NZCV & ~(0xFull << 28)) | (1ull << 29);   // C = 1, Z = 0 (hi): b.ls not taken, the function returns
    }
};

// ---- IMPL-24 (fix review 3 item 3): one-tick clamp of 0x359934 (0x359AC8 `fmin s4, s8, s9`: d = min(B, 1)). All 9 callers pass an elapsed budget B (the getter 0x58C908 result,
// stored to [x1]); none is synthetic (impl/IMPL-24-facts.md). The hook sits at 0x359AEC (head stores done, d in s4). For B > 1 and n >= 2 LoadSegs units the passes run per segment:
// the segments 0..n-2 here (Unit359: the 17-row pass, the 17-row phase pass and the +0xD20 commit, 0x359B18..0x359D10, then the tail row stores 0x359D20..0x359E48 that the next
// native call would see), the last segment natively (s4 := its d). B <= 1, n < 2 or no clock: native, bit-exact. fp-contract is off; the native fmla lanes use fmaf.
namespace clamp359 {
constexpr float F(u32 b) { return __builtin_bit_cast(float, b); }
inline void TailRows(u8* o, const u8* a) {   // 0x359D20: row 0 = [o+0xCD0], row 16 = [o+0xCF0]; then the nine argument rows (index >= 1)
    __builtin_memcpy(o, o + 0xCD0, 16); __builtin_memcpy(o + 0x400, o + 0xCF0, 16);
    for (int f = 0x18; f <= 0x38; f += 4) { const s32 idx = *reinterpret_cast<const s32*>(a + f); if (idx >= 1) __builtin_memcpy(o + idx * 64, a + 0x40 + (f - 0x18) * 4, 16); }
}
inline void Unit359(u8* o, const u8* a, const float* A /*[x29-0x60]*/, const float* C /*[x29-0x50]*/, float d) {
    using Fn1 = float (*)(float); using FnRot = void (*)(float*, float);
    const float v17[4] = {A[0] - C[0], A[1] - C[1], A[2] - C[2], A[3] - C[3]};
    const bool c70 = At<u32>(o, 0xC70) != 0; const bool slow = (*reinterpret_cast<const u32*>(a + 0x14) & 2) != 0;
    const u8* q5 = reinterpret_cast<const u8*>(exl::util::modules::GetTargetStart() + 0x176AFD0);
    for (int j = 0; j < 17; j++) {                       // 0x359B44..0x359B3C
        u8* r = o + j * 0x40; float* rf = reinterpret_cast<float*>(r);
        __builtin_memcpy(r + 0x10, r, 16);
        const float l0 = At<float>(o, 0xC7C), lam = slow ? l0 * F(0x3E4CCCCD) : l0;
        rf[9] = rf[9] - d * lam;
        if (!c70) { for (int l = 0; l < 4; l++) rf[l] = rf[l] + d * rf[8 + l]; }
        else { const float t = static_cast<float>(j) * F(0x3D800000); for (int l = 0; l < 4; l++) rf[l] = C[l] + v17[l] * t; }
        __builtin_memcpy(r + 0x30, q5, 16);
    }
    const float* vq = reinterpret_cast<const float*>(o + 0xD10);
    const float p0 = vq[0] * vq[0], p1 = vq[1] * vq[1], p2 = vq[2] * vq[2];
    if (!((p2 + p0) + (p1 + 0.0f) > 0.0f)) return;       // 0x359C10 b.le: no second pass, no commit
    const Fn1 Sin = Game<Fn1>(0x1321290), Wrap = Game<Fn1>(0xCC7440); const FnRot Rot = Game<FnRot>(0xCC79A0);
    const float s10 = d * F(0x3EC49809);
    float z = At<float>(o, 0xD20), s0in = z;
    for (int x = 0; x < 0x440; x += 0x40) {
        alignas(16) float m[16] = {};
        const float ang = Sin(s0in) * F(0x3EB2B8C2) + F(0xBE32B8C2);
        Rot(m, ang);
        float v[4], vec[4];
        for (int l = 0; l < 4; l++) v[l] = m[l] * vq[0];
        for (int l = 0; l < 4; l++) v[l] = __builtin_fmaf(m[4 + l], vq[1], v[l]);
        for (int l = 0; l < 4; l++) v[l] = __builtin_fmaf(m[8 + l], vq[2], v[l]);
        for (int l = 0; l < 4; l++) vec[l] = v[l] * F(0x3DCCCCCD);
        const float ph = Sin(z) * F(0x3F333333) + F(0x3E99999A);
        const float w1 = Wrap(s10 + z); z = w1; s0in = Wrap(w1);
        float* row = reinterpret_cast<float*>(o + x);
        for (int l = 0; l < 4; l++) row[l] = row[l] + vec[l] * ph;
    }
    At<float>(o, 0xD20) = At<float>(o, 0xD20) + d * At<float>(o, 0xD24);
    At<float>(o, 0xD20) = Wrap(At<float>(o, 0xD20));
}
}
struct Clamp359 : exl::hook::impl::InlineHook<Clamp359> {   // 0x359AEC: x19 = this (actor + 0x1690), x21 = args ([x21] = B), x29 frame, s8 = B, s4 = min(B, 1)
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        const float B = ctx->S[8];
        if (g_N == 0 || !(B > 0.0f)) return;   // IMPL-27 item 7: replay whenever the update has >= 2 segments (B <= 1 included); exactly one segment is native
        u8* o = reinterpret_cast<u8*>(ctx->X[19]); const u8* a = reinterpret_cast<const u8*>(ctx->X[21]);
        float w[kSegCap], r[kSegCap]; const int n = LoadSegs(o - 0x1690, w, r, true);
        if (n < 2) return;
        const float* A = reinterpret_cast<const float*>(ctx->X[29] - 0x60); const float* C = reinterpret_cast<const float*>(ctx->X[29] - 0x50);
        float last = 0.0f;
        for (int i = 0; i < n; i++) {
            float b = B * w[i];
            if (__builtin_fabsf(b - r[i]) <= 2.0e-6f * r[i]) b = r[i];   // unscaled: the segment is exactly its raw length (1, 1, 0.4)
            const float d = b < 1.0f ? b : 1.0f;
            if (i == n - 1) { last = d; break; }
            if (!(r[i] > 0.0f)) continue;
            clamp359::Unit359(o, a, A, C, d);
            clamp359::TailRows(o, a);
        }
        ctx->S[4] = last;
    }
};

// ---- IMPL-28 item 1 (fix review 3 item 3): cloth step 0x8132F8 caps its budget to one tick (0x813650 `fmin s0, s8, 1.0`). Caller 0x812F1C calls it at 0x813154 (x0 = x19, w1 = w20,
// s0 = B). Ordinary path = w21 == 1 (loop count 1, s0 = entry budget); the synthetic initialisation path has w21 == 15 (s0 = 10.0) and stays native. With >= 2 LoadSegs unit segments the
// segments 0..n-2 run here as complete 0x8132F8 calls (budget = segment budget), then the native call runs the last one (s0 := its budget). The caller's loop count is not touched.
// Scope of each call: credit cap = whole units of the segment (raw length >= 1 -> 1, else 0) for the integer countdown 0x814208 (and every other counter below), and
// segF = the segment's raw length for the ramps 0x814228 / 0x81425C (StepFactor). ClothPost (0x813158) closes the scope of the native call. Logic thread only (Credit's cap is logic thread only).
namespace cloth813 {
// IMPL-33 item 1: the credit of a segment is the integer cl[i] from SplitUnitBoundaries (boundaries it closes); no float comparison here.
inline void SetSeg(ThreadCtx& t, float raw, int credit) {
    int cap1 = credit + 1;
    const int outer = t.clDepth > 0 ? t.clCap1[t.clDepth - 1] : 0;
    if (outer > 0 && outer < cap1) cap1 = outer;   // inside an outer cap (owner 0x1B1A24) the smaller cap holds
    if (t.credCap1 == 0) __atomic_fetch_add(&g_capOn, 1, __ATOMIC_RELAXED);
    t.credCap1 = cap1; t.segOn = true; t.segF = raw;
}
}
struct ClothPre : exl::hook::impl::InlineHook<ClothPre> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread || ctx->W[21] != 1) return;   // w21 == 15: synthetic initialisation, native
        const float B = ctx->S[0];
        u8* x19 = reinterpret_cast<u8*>(ctx->X[19]); u8* actor = reinterpret_cast<u8*>(At<u64>(x19, 0xB10));
        if (!(B > 0.0f) || actor == nullptr) return;
        ThreadCtx& t = Thr();
        if (t.clDepth >= 4) return;
        float w[kSegCap], r[kSegCap]; int cls[kSegCap]; const int n = LoadSegs(actor, w, r, true, cls);
        if (n < 2) return;
        const int k = t.clDepth++; t.clObj[k] = ctx->X[19]; t.clSegOn[k] = t.segOn; t.clSegF[k] = t.segF; t.clCap1[k] = t.credCap1;
        __atomic_fetch_add(&g_segOn, 1, __ATOMIC_RELAXED);
        using Fn = void (*)(u64, u32, float);
        for (int i = 0; i < n; i++) {
            float b = B * w[i];
            if (__builtin_fabsf(b - r[i]) <= 2.0e-6f * r[i]) b = r[i];
            const float d = b < 1.0f ? b : 1.0f;
            const int cr = cls[i];
            if (i == n - 1) { cloth813::SetSeg(t, r[i], cr); ctx->S[0] = d; break; }
            if (!(r[i] > 0.0f)) continue;
            cloth813::SetSeg(t, r[i], cr);
            Game<Fn>(0x8132F8)(ctx->X[19], static_cast<u32>(ctx->W[20]), d);
        }
    }
};
struct ClothPost : exl::hook::impl::InlineHook<ClothPost> {   // 0x813158 (behind the bl): close the scope opened by ClothPre for this object
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        if (t.clDepth <= 0 || t.clObj[t.clDepth - 1] != ctx->X[19]) return;
        const int k = --t.clDepth;
        if (t.credCap1 != 0 && t.clCap1[k] == 0) __atomic_fetch_sub(&g_capOn, 1, __ATOMIC_RELAXED);
        t.credCap1 = t.clCap1[k]; t.segOn = t.clSegOn[k]; t.segF = t.clSegF[k];
        __atomic_fetch_sub(&g_segOn, 1, __ATOMIC_RELAXED);
    }
};
// ---- IMPL-28 item 2: record loop 0x819FCC (cap 0x81A0CC/0x81A0D0). Everything before 0x81A0D8 is lookups (0x6414C8, 0xBD3DBC) except the reset block 0x81A03C..0x81A0CC, which runs when
// [actor+0x12D6] & 0x210 or [x0+0xB20] != 0 and rewrites the record state, so the call is NOT idempotent. Adapter (TrampolineHook): segment 1 runs the whole function natively (reset included),
// segments 2..n run the whole function again with the 0x210 bits of [actor+0x12D6] masked (b20 is already 0), the bits segment 1 left are put back afterwards (IMPL-29 item 7: the asm of 0x819FCC only READS
// [actor+0x12D6] at 0x81A018 and clears [x0+0xB20]; no store to +0x12D6 / actor-base+0x1240+0x96 in it or in the callees 0xBD3B78 / 0xBD3D0C / 0xBD3DBC / 0xBDB028 / 0xCC8290 / 0xCC88F0 windows; 0x1321280 not read). The state sits in memory, so it carries over.
// The synthetic literal budget 0.001 (0xA5C4C) stays native. Logic thread only.
struct ClothRec : exl::hook::impl::TrampolineHook<ClothRec> {
    static u64 Callback(u64 x0, u64 x1, u64 x2, u64 x3, u64 x4, u64 x5, u64 x6, u64 x7,
                        float64x2_t v0, float64x2_t v1, float64x2_t v2, float64x2_t v3,
                        float64x2_t v4, float64x2_t v5, float64x2_t v6, float64x2_t v7) {
        auto call = [&](float64x2_t v) { return ClothRec::Orig(x0, x1, x2, x3, x4, x5, x6, x7, v, v1, v2, v3, v4, v5, v6, v7); };
        if (g_N == 0 || ThreadId() != g_logicThread) return call(v0);
        const float B = vgetq_lane_f32(vreinterpretq_f32_f64(v0), 0);
        if (!(B > 0.0f) || std::bit_cast<u32>(B) == 0x3A83126Fu) return call(v0);
        const u64 h = Game<u64 (*)(u64)>(0x6414C8)(x0 + 0xB1C);
        u8* actor = h != 0 ? reinterpret_cast<u8*>(At<u64>(reinterpret_cast<void*>(h), 0x48)) : nullptr;
        if (actor == nullptr) return call(v0);
        float w[kSegCap], r[kSegCap]; const int n = LoadSegs(actor, w, r, true);
        if (n < 2) return call(v0);
        u16 post = 0;   // IMPL-29 item 7: the 0x210 bits that segment 1 LEFT (what the native call did to them); the end of the replay restores exactly those
        u64 res = 0; bool ran = false;
        for (int i = 0; i < n; i++) {
            float b = B * w[i];
            if (__builtin_fabsf(b - r[i]) <= 2.0e-6f * r[i]) b = r[i];
            const float d = b < 1.0f ? b : 1.0f;
            if (i < n - 1 && !(r[i] > 0.0f)) continue;
            if (ran) At<u16>(actor, 0x12D6) = static_cast<u16>(At<u16>(actor, 0x12D6) & ~0x210);
            res = call(vreinterpretq_f64_f32(vsetq_lane_f32(d, vreinterpretq_f32_f64(v0), 0)));
            if (!ran) post = At<u16>(actor, 0x12D6) & 0x210;
            ran = true;
        }
        if (ran && n > 1) At<u16>(actor, 0x12D6) = static_cast<u16>((At<u16>(actor, 0x12D6) & ~0x210) | post);
        return res;
    }
};

// ---- IMPL-11 item 3: successor of the 0x371CA8 crossing. The crossing visit (counter actor+0xA24 lands on 5, timers 10.0 stored by 0x371CC8, decrement skipped by 0x371D60)
// has leftover units u in the owner carry of the IntHookZ key (0x371CB0, x24 + 0xA24). Hook at 0x371D60 (the `b 0x37244c` that ends the crossing block): u more units, each
// exactly 0x371CA4..0x371CB0 and 0x372428..0x372448: counter += 1; if counter >= 6 (unsigned) and timer [x24+0xA20] > 0: timer -= 1.0, stored to +0xA20 and +0xA1C.
// Not repeated (calls, listed in the report): the shared tail 0x37244C (query 0xB90700 w1 = 0x5A, effect 0xB90954/0xB8FD44/0xB90040) runs once, natively.
inline int Cross371(u8* x24, Counter& c) {
    s64 u = c.carry;
    if (u <= 0 || At<s32>(x24, 0xA24) != 5) return 0;
    if (u > 8) u = 8;
    for (s64 k = 0; k < u; k++) {
        u32 cnt = At<u32>(x24, 0xA24) + 1;
        At<u32>(x24, 0xA24) = cnt;
        if (cnt >= 6) { float t = At<float>(x24, 0xA20); if (t > 0.0f) { t += -1.0f; At<float>(x24, 0xA20) = t; At<float>(x24, 0xA1C) = t; } }
    }
    c.carry = 0;
    return static_cast<int>(u);
}
struct CrossSucc : exl::hook::impl::InlineHook<CrossSucc> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0) return;
        Counter& c = FindCounter(0x371CB0, ctx->X[24] + 0xA24, Now<false>());
        if (c.U != g_N) return;                                  // no hook visit in this update
        Cross371(reinterpret_cast<u8*>(ctx->X[24]), c);
    }
};

// ---- IMPL-17: unit replay of the owner 0x371814 (one caller 0x3717DC in 0x3717C4; the caller ignores the return value). A trampoline wrapper calls the original d times per update (d = smart due
// ticks of the key (0x371814, owner), 0 allowed). Between two calls it does the caller's work for the unit: T = [owner+0x4FFC]; if T > 0, T -= sigma_u (sigma_u = S_hat * e_u / sum(e), S_hat = 0x58C908(owner),
// e_u = unit budgets of ComputeUnitSteps; all 0 when sum(e) is 0 or not finite). The native caller code does the last unit; the hook at 0x371804 gives it sigma_d. d = 0: one visit with "zero" set; small hooks
// then keep every counter step out of it (0x371914 reset skipped, 0x371A20 / 0x371EAC pre-bias, cascade forced to mode 1 with a28 - 1). The +0x1FE0 increment (0x371C70) is scaled by r = f / d (f in a zero visit).
#if REPLAY_371814
namespace owner371 {
struct Visit { bool on, zero, biased; float r; };
Visit g_v = {};
struct Pending { u64 owner, n; float sigma; };
Pending g_pend = {};
u32 g_a28Odd = 0, g_visits = 0, g_zeroVisits = 0;   // debug counters: a28 outside 0..5 (neutralizer skipped), replay visits, zero visits
constexpr int kMaxD = 8;
inline bool Active() { return g_v.on && ThreadId() == g_logicThread; }
inline bool Zero() { return Active() && g_v.zero; }
template <uintptr_t Addr>
struct Replay : exl::hook::impl::TrampolineHook<Replay<Addr>> {
    static u64 Callback(u64 x0, u64 x1, u64 x2, u64 x3, u64 x4, u64 x5, u64 x6, u64 x7,
                        float64x2_t v0, float64x2_t v1, float64x2_t v2, float64x2_t v3,
                        float64x2_t v4, float64x2_t v5, float64x2_t v6, float64x2_t v7) {
        auto call = [&]() { return Replay<Addr>::Orig(x0, x1, x2, x3, x4, x5, x6, x7, v0, v1, v2, v3, v4, v5, v6, v7); };
        if (g_N == 0 || ThreadId() != g_logicThread) return call();
        u8* o = reinterpret_cast<u8*>(x0);
        g_pend = {};
        // d: same smart tick as the other owners (carry per key, a gap forgives it, at most 4 ticks per update plus carry, at most 8 units)
        const u64 now = Now<false>();
        Counter& c = FindCounter(0x371814, x0, now);
        const bool held = c.U == g_N - 1;
        u64 dt = held ? now - c.S : 1;
        if (!held) c.carry = 0;
        if (dt > 4) dt = 4;
        s64 d = static_cast<s64>(dt) + c.carry;
        c.carry = 0;
        if (d > kMaxD) { c.carry = d - kMaxD; d = kMaxD; }
        c.S = now; c.U = g_N;
        g_visits++;
        const float f = g_step;
        u64 res;
        if (d == 0) {
            g_zeroVisits++;
            g_v = {true, true, false, f};
            res = call();
        } else {
            const float sHat = Game<float (*)(void*)>(0x58C908)(o);
            UnitSteps us; ComputeUnitSteps(o, us);
            float e[kMaxD], sigma[kMaxD], sum = 0.0f, used = 0.0f;
            for (int k = 0; k < d; k++) {
                if (k < d - 1) e[k] = k < us.T ? us.u[k] : (us.T > 0 ? us.u[us.T - 1] : us.total);
                else e[k] = std::max(us.total - used, 0.0f);
                used += e[k]; sum += e[k];
            }
            const bool ok = sum > 0.0f && std::isfinite(sum) && std::isfinite(sHat);
            for (int k = 0; k < d; k++) sigma[k] = d == 1 ? sHat : (ok ? sHat * e[k] / sum : 0.0f);   // d == 1: exactly the native subtraction
            const float r = f / static_cast<float>(d);
            res = 0;
            for (int u = 1; u <= d; u++) {
                g_v = {true, false, false, r};
                res = call();
                if (u < d) { float T = At<float>(o, 0x4FFC); if (T > 0.0f) { T -= sigma[u - 1]; At<float>(o, 0x4FFC) = T; } }
            }
            g_pend = {x0, g_N, sigma[d - 1]};
        }
        g_v = {};
        return res;
    }
};
struct CallerFix : exl::hook::impl::InlineHook<CallerFix> {   // before 0x371804 `str s0,[x20]`: s0 = s1 - sigma_d
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (ThreadId() != g_logicThread) return;
        if (g_pend.n != 0 && g_pend.n == g_N && g_pend.owner == ctx->X[19]) ctx->S[0] = ctx->S[1] - g_pend.sigma;
        g_pend = {};
    }
};
struct NoReset : exl::hook::impl::InlineHook<NoReset> {       // 0x371914, before `fcmp s0,#0.0`: register s0 = 1.0 so b.hi skips the reset and the transaction
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (Zero() && ctx->S[0] <= 0.0f) ctx->S[0] = 1.0f; }
};
struct PreBiasUp : exl::hook::impl::InlineHook<PreBiasUp> {   // 0x371A20, before `sub w8,w8,#1`: w8 += 1 so the native sub stores L unchanged
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (Zero()) ctx->W[8] += 1; }
};
struct PreBiasDown : exl::hook::impl::InlineHook<PreBiasDown> {   // 0x371EAC, before `add w8,w8,#1` (the re-increment of L, net 0)
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (Zero()) ctx->W[8] -= 1; }
};
struct CascadeA28 : exl::hook::impl::InlineHook<CascadeA28> { // 0x371C7C, before `ldr w8,[x20,#8]`: a28 -= 1 (the native wrap step stores it unchanged)
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (!Zero()) return;
        u8* x24 = reinterpret_cast<u8*>(ctx->X[24]);
        const u32 a = At<u32>(x24, 0xA28);
        if (a <= 5) { At<u32>(x24, 0xA28) = a - 1; g_v.biased = true; } else g_a28Odd++;
    }
};
struct CascadeMode : exl::hook::impl::InlineHook<CascadeMode> { // 0x371C80, before `cmp w8,#1`: mode 1 (the a28 path exits at 0x371CA0, a24 / a20 untouched)
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (Zero() && g_v.biased) ctx->W[8] = 1; }
};
struct Scale1FE0 : exl::hook::impl::InlineHook<Scale1FE0> {   // 0x371C70, before `fadd v0.4s,v1.4s,v0.4s`: v0 *= r (0 -> exactly 0)
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (!Active()) return;
        float* q = reinterpret_cast<float*>(&ctx->Q[0]);
        for (int i = 0; i < 4; i++) q[i] = g_v.r == 0.0f ? 0.0f : q[i] * g_v.r;
    }
};
}  // namespace owner371
#endif

// ---- IMPL-17 item 6 (review round 15 R4, agreed part): the reload store 0x55AF90 (`[x19+0x204] = 15`) ran in this visit; with d >= 2 for the key (0x55AF04, x19+0x204) the remaining units are the
// positive path with stable input: [0x204] = w22 (10 natively), [0x20C..0x213] = 0, [0x210] = 15, [0x214] = [0xB4]. The angle thresholds at 0x55AF3C / 0x55AF58 are NOT changed (still open).
struct Reload55Rec { u64 actor, n; };
Reload55Rec g_r55 = {};
struct Reload55Mark : exl::hook::impl::InlineHook<Reload55Mark> {   // before 0x55AF90
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (g_N != 0 && ThreadId() == g_logicThread) g_r55 = {ctx->X[19], g_N}; }
};
struct Reload55Apply : exl::hook::impl::InlineHook<Reload55Apply> {  // before 0x55AF94
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || ThreadId() != g_logicThread || g_r55.n != g_N || g_r55.actor != ctx->X[19]) return;
        g_r55 = {};
        u8* a = reinterpret_cast<u8*>(ctx->X[19]);
        Counter* c = LookCounter(0x55AF04, ctx->X[19] + 516);   // peek only: the reload visit itself runs no hook of this key
        if (c == nullptr || c->U != g_N - 1) return;
        u64 dt = Now<IsTail(0x55AF04)>() - c->S; if (dt > 4) dt = 4;
        s64 d = static_cast<s64>(dt) + c->carry; if (d > 8) d = 8;
        if (d < 2) return;
        At<u32>(a, 0x204) = static_cast<u32>(ctx->W[22]);
        At<u64>(a, 0x20C) = 0;
        At<u32>(a, 0x210) = 15;
        At<u32>(a, 0x214) = At<u32>(a, 0xB4);
    }
};

// ---- IMPL-11 item 4: phase gate 0x3EBB40..0x3EBB90 (actor +0x3f9c mod 60 == global +0x10c mod 60 -> fire). The writer 0x66FEC4 (smart tick, d >= 0 per update) records
// the interval (old, new] of this update; the gate fires when the actor phase lies in it (mod 60; once even if the advance is >= 60; never for d = 0).
// If no writer visit was seen in this or the previous update the native equality stays.
struct PhaseRec { u32 oldv, newv; u64 n; };
PhaseRec g_ph = {};
struct PhaseWriter : exl::hook::impl::InlineHook<PhaseWriter> {   // before `add w8,w8,#1` (w8 = [x19+0x10c])
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        const u32 old = ctx->W[8];
        Credit<8, 1, +1, false, 19, 268, -1, IsTail(0x66FEC4), false, 0, IsLevel(0x66FEC4)>(ctx, 0x66FEC4);
        const u32 nw = ctx->W[8] + 1;                              // the native add follows
        if (g_N != 0 && g_ph.n == g_N && g_ph.newv == old) g_ph.newv = nw;   // second writer visit in one update: contiguous
        else g_ph = {old, nw, g_N};
    }
};
inline bool PhaseFire(u32 oldv, u32 newv, s32 actorMod60) {
    const u32 d = newv - oldv;
    if (d >= 60) return true;
    if (d == 0) return false;
    const s32 a = (static_cast<s32>(oldv) % 60 + 60) % 60, p = (actorMod60 % 60 + 60) % 60;
    return ((p - a - 1 + 120) % 60) < static_cast<s32>(d);
}
// IMPL-12 item 6: per actor (x19), the epoch of the last interval it consumed. Open addressed, 1,024 entries, probe window 64; a full window replaces the entry with the oldest epoch (counted).
struct PhaseActor { u64 actor, epoch; };
constexpr int kPhaseActors = 1024, kPhaseProbe = 64;
PhaseActor g_phActor[kPhaseActors];
u32 g_phEvict = 0;
inline PhaseActor& PhaseSlot(u64 actor) {
    PhaseActor* old = nullptr;
    for (int i = 0; i < kPhaseProbe; i++) {
        PhaseActor& e = g_phActor[((actor / 16) * 0x9E3779B1ull + i) % kPhaseActors];
        if (e.actor == actor) return e;
        if (e.actor == 0) { e = {actor, 0}; return e; }
        if (old == nullptr || e.epoch < old->epoch) old = &e;
    }
    g_phEvict++; if (old->epoch != 0 && old->epoch + 1 >= g_N) g_phEvictLive++;   // IMPL-52: phaseEvictLive
    *old = {actor, 0}; return *old;
}
struct PhaseGate : exl::hook::impl::InlineHook<PhaseGate> {        // at the b.ne after `cmp w8,w9` (w8 = actor phase mod 60): Z = fire
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        if (g_N == 0 || g_ph.n == 0 || g_ph.n + 1 < g_N) return;
        PhaseActor& a = PhaseSlot(ctx->X[19]);
        const bool seen = a.epoch == g_ph.n;                       // this actor already consumed the latest interval: no second crossing from it
        a.epoch = g_ph.n;
        const bool fire = !seen && PhaseFire(g_ph.oldv, g_ph.newv, static_cast<s32>(ctx->W[8]));
        ctx->NZCV = (ctx->NZCV & ~(1ull << 30)) | (fire ? (1ull << 30) : 0);
    }
};

inline void TailPush(bool v, bool hasPaired = false, bool paired = false, float e0 = 1.0f, float raw = 1.0f, u64 n = 0, u64 g = 0, u64 gt = 0, float ft = 1.0f, int snap = -1, u64 gp = 0, u64 gtp = 0, float acc = 0.0f, bool live = false, float accA = 0.0f, float accTA = 0.0f, float accTB = 0.0f) {
    ThreadCtx& t = Thr();
    if (t.depth < 4) { t.ownSnap[t.depth] = false; t.savedSnap[t.depth] = t.snap; t.saved[t.depth] = t.tail; t.savedPaired[t.depth] = t.paired; t.savedHas[t.depth] = t.hasPaired; t.savedE0[t.depth] = t.e0; t.savedRaw[t.depth] = t.raw;
                       t.savedN[t.depth] = t.cN; t.savedG[t.depth] = t.cG; t.savedGt[t.depth] = t.cGt; t.savedFt[t.depth] = t.cFt; t.savedGp[t.depth] = t.cGp; t.savedGtp[t.depth] = t.cGtp; t.savedAcc[t.depth] = t.cAcc; t.savedLive[t.depth] = t.live; t.savedAccA[t.depth] = t.cAccA; t.savedAccTA[t.depth] = t.cAccTA; t.savedAccTB[t.depth] = t.cAccTB; }
    t.depth++; t.tail = v; if (hasPaired) { t.hasPaired = true; t.paired = paired; t.e0 = e0; t.raw = raw; t.cN = n; t.cG = g; t.cGt = gt; t.cFt = ft; t.snap = snap; t.cGp = gp; t.cGtp = gtp; t.cAcc = acc; t.live = live; t.cAccA = accA; t.cAccTA = accTA; t.cAccTB = accTB; }   // a synchronous push keeps an installed context
}
inline void TailPop() { ThreadCtx& t = Thr(); if (t.depth > 0) { t.depth--; if (t.depth < 4) { if (t.ownSnap[t.depth]) { t.ownSnap[t.depth] = false; NodeUnpin(t.snap); } t.snap = t.savedSnap[t.depth]; t.tail = t.saved[t.depth]; t.paired = t.savedPaired[t.depth]; t.hasPaired = t.savedHas[t.depth]; t.e0 = t.savedE0[t.depth]; t.raw = t.savedRaw[t.depth];
                       t.cN = t.savedN[t.depth]; t.cG = t.savedG[t.depth]; t.cGt = t.savedGt[t.depth]; t.cFt = t.savedFt[t.depth]; t.cGp = t.savedGp[t.depth]; t.cGtp = t.savedGtp[t.depth]; t.cAcc = t.savedAcc[t.depth]; t.live = t.savedLive[t.depth]; t.cAccA = t.savedAccA[t.depth]; t.cAccTA = t.savedAccTA[t.depth]; t.cAccTB = t.savedAccTB[t.depth]; } } }
inline bool EffPaired() { ThreadCtx& t = Thr(); return t.hasPaired ? t.paired : g_paired; }
// The tail function 0x671520 (entered by `b` from 0x671310) calls the first-pass-only roots and ends in the tail branch 0x6715D4 -> 0x696A64.
// Scope = entry of 0x671520 (push) .. return of 0x696A64 (pop, TailExit below): covers every wrapper call listed in IMPL-8 item 7 and the tail branch, nested-safe.
struct TailEnter : exl::hook::impl::InlineHook<TailEnter> {
    static void Callback(exl::hook::InlineFloatCtx*) { TailPush(true); }
};
template <uintptr_t Addr>
struct TailExit : exl::hook::impl::TrampolineHook<TailExit<Addr>> {
    static u64 Callback(u64 x0, u64 x1, u64 x2, u64 x3, u64 x4, u64 x5, u64 x6, u64 x7,
                        float64x2_t v0, float64x2_t v1, float64x2_t v2, float64x2_t v3,
                        float64x2_t v4, float64x2_t v5, float64x2_t v6, float64x2_t v7) {
        u64 r = TailExit<Addr>::Orig(x0, x1, x2, x3, x4, x5, x6, x7, v0, v1, v2, v3, v4, v5, v6, v7);
        TailPop();
        return r;
    }
};
// Getter return hooks: at every `ret` of 0xC1F09C (0xC1F0A8, 0xC1F0C0, 0xC1F0C8) and 0xC1F0CC (0xC1F0D8, 0xC1F0E4) the returned step s0 is halved while
// the thread is in the tail scope and the game is paired. 0x58C908 tail-calls 0xC1F09C, so it is covered once.
// IMPL-14 step 3: inside an installed async callback the getter returns the step of the SUBMITTING update: the value v (the current step times the token factor) is rescaled by
// captured E0 / current E0 (v == current E0 gives exactly the captured E0); current E0 == 0 gives the captured E0 itself. Raw-step readers use the captured raw step.
inline float TailScale(float v) {
    ThreadCtx& t = Thr();
    return (t.tail && (t.hasPaired ? t.paired : g_paired)) ? v * 0.5f : v;   // the getter's scaling helper
}
inline float RawStepNow() { ThreadCtx& t = Thr(); return t.hasPaired ? t.raw : g_step; }
// IMPL-26 item 1: the E0 ratio rule of IMPL-14 is removed. Inside an installed async context with a snapshot, 0xC1F0A8 (x8 = token) returns the value that token had published when the
// record was queued (a token outside the pool keeps the native value, g_snapNotPool counts it) and 0xC1F0C0 the global +0xC4 of that moment; the tail halving still applies on top.
// No snapshot (id -1, pool full): the current values, as before. 0xC1F0C8 / 0xC1F0D8 / 0xC1F0E4 only get the tail halving.
inline void GetRec(u8 m, float d) { g_getMode = m; g_getDim = d; g_getN = g_N; g_getSeq++; }
template <uintptr_t Addr>
struct TailGetter : exl::hook::impl::InlineHook<TailGetter<Addr>> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        ThreadCtx& t = Thr();
        constexpr bool kRec = Addr == 0xC1F0A8 || Addr == 0xC1F0C0 || Addr == 0xC1F0C8;
        const bool rec = kRec && ThreadId() == g_logicThread;
        u8 gm = 0; float gd = 0.0f;   // IMPL-57 getter record for the writer 0x4423A0
        if (t.ovOn) { if (rec) GetRec(0, 0.0f); ctx->S[0] = t.ov; return; }   // IMPL-14 step 4: a scoped step override (owner 0x1B1A24) comes first
        float v = ctx->S[0];
        if (rec) {
            if (Addr == 0xC1F0C8) gm = 2;
            else if (t.hasPaired && t.snap >= 0) gm = 0;
            else if (Addr == 0xC1F0C0) { if (g_budConst) gm = 2; else if (g_dimBudOk) { gm = 1; gd = TailScale(g_dimBud[3]); } }
            else if (const TokenEntry* e = FindToken(ctx->X[8], false); e != nullptr && e->dimOk) { if (e->dimConst) gm = 2; else { gm = 1; gd = TailScale(e->dim); } }
            GetRec(gm, gd);
        }
        if (t.hasPaired && t.snap >= 0 && (Addr == 0xC1F0A8 || Addr == 0xC1F0C0)) {
            const PubNode& s = g_node[t.snap];
            if (t.live) g_nodeStaleTok++;   // IMPL-33 item 3: a 'clock live' record reads the newest published node
            if (Addr == 0xC1F0C0) { if (s.c4ok) v = s.c4; }
            else {
                const u64 tok = ctx->X[8], off = tok - s.base;
                if (s.base != 0 && tok >= s.base && off % kTokStride == 0 && off / kTokStride < s.cnt) v = s.v[off / kTokStride];
                else g_snapNotPool++;
            }
        }
        ctx->S[0] = TailScale(v);
    }
};
// IMPL-12 item 8: 0xB5D3D4..0xB5D3DC is the bit-4 path of 0xB5D38C (selected at 0xB5D3B8): `fmov s8,w8` with w8 = 0x3c888889 = 1/60, a literal per call (the unscaled path,
// like 0x643460: no override, no token factor). Hook on the next instruction (0xB5D3E0): s8 = admitted raw step / 60, halved in the tail scope while paired (same helper as the getters).
struct AnimLiteral : exl::hook::impl::InlineHook<AnimLiteral> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (g_N != 0) { ThreadCtx& t = Thr(); if (t.ovOn) { ctx->S[8] = t.ovRaw / 60.0f; return; } const float raw = RawStepNow(); ctx->S[8] = ((t.tail && (t.hasPaired ? t.paired : g_paired)) ? raw * 0.5f : raw) / 60.0f; } }
};
// Async work: 0xD04FD0 (`stp x5,x6,[x8]` of the queue submit 0xD04EA0) stores callback and argument into record x8 (generation w11 -> +0x24).
// The submitter's context is captured per (record, generation); around `blr x9` at 0xD04E88 (record x8) the captured context is installed, restored at 0xD04E8C.
// IMPL-12 item 4: open-addressed table, 4,096 entries (the queue capacity [queue+0xA0] is a runtime value, so it is not known statically), key = record address.
// IMPL-14 step 3: an entry is removed when its record is consumed (AsyncInstall knows the record x8); a record run a second time without a new submission finds no entry and keeps the
// current context (g_asyncMiss). The table is bounded by the number of distinct record addresses of all queues. Never evicts another record:
// when the table has no free slot or a lookup misses, the consumer keeps its CURRENT tail context and a counter goes up (g_asyncFull / g_asyncMiss). One global spin lock.
struct AsyncCtx { u64 rec; u32 gen; u8 state; bool tail; bool paired; u64 epoch; float e0, raw; u64 g, gt; float ft; int snap; u64 gp, gtp; float acc; bool live; float accA, accTA, accTB; };   // IMPL-26: snap = getter snapshot id (-1 none); IMPL-25: epoch = N; g/gt/ft = G, tail G, tail step of the submitting update;   // state 0 free, 1 live, 2 removed at consumption (IMPL-14 step 3; probe chains continue over it)
constexpr int kAsync = 4096;
AsyncCtx g_async[kAsync];
u32 g_asyncFull = 0, g_asyncMiss = 0, g_asyncLate = 0;
struct AsyncCapture : exl::hook::impl::InlineHook<AsyncCapture> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        const u64 rec = ctx->X[8];
        // IMPL-25 item 5: a nested submit (the submitting thread itself runs inside an installed context) captures THAT context, not the globals.
        const ThreadCtx& t = Thr();
        // IMPL-31 item 2: the clock tuple AND the getter values come from ONE pinned node (immutable while pinned). A nested submit shares the context's node (one more pin).
        AsyncCtx n;
        if (t.hasPaired) { NodePinAgain(t.snap); n = AsyncCtx{rec, ctx->W[11], 1, t.tail, t.paired, t.cN, t.e0, t.raw, t.cG, t.cGt, t.cFt, t.snap, t.cGp, t.cGtp, t.cAcc, t.live, t.cAccA, t.cAccTA, t.cAccTB}; }
        else {
            int id = -2;
            if (g_nodeFullFlag != 0 && g_N != 0 && ThreadId() == g_logicThread) id = OverflowNode();   // IMPL-35 item 2: pool full: a private complete snapshot of the live state
            const bool ovf = id >= 0;
            if (id == -2 || id == -1) id = NodePin();                                                   // normal path (or overflow also full: the newest published node, residual)
            PubClock ck = id >= 0 ? g_node[id].c : PubClock{0, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1, false, 0, 0, 0.0f, 0.0f};   // nothing published yet (N == 0): stock values
            bool live = false;
            if (!ovf && g_nodeFullFlag != 0 && g_N != 0 && ThreadId() == g_logicThread) {   // overflow full too: the clock tuple from the live globals, token/C4 from the newest node (g_nodeStaleTok)
                ck = {g_N, g_G, g_Gt, g_step, g_e0, g_ft, g_rawNow, g_accBefore, g_T, g_paired, g_Gp, g_Gtp, static_cast<float>(g_acc), static_cast<float>(g_accT), g_accTBefore}; live = true;
            }
            n = AsyncCtx{rec, ctx->W[11], 1, t.tail, ck.paired, ck.N, ck.e0, ck.step, ck.G, ck.Gt, ck.ft, id, ck.Gp, ck.Gtp, ck.accBefore, live, ck.accAfter, ck.accTAfter, ck.accTBefore};
        }
        AsyncLock();
        AsyncCtx* freeSlot = nullptr; bool done = false;
        for (int i = 0; i < kAsync; i++) {
            AsyncCtx& a = g_async[((rec / 16) + i) % kAsync];
            if (a.state == 1 && a.rec == rec) { NodeUnpin(a.snap); a = n; done = true; break; }   // same record resubmitted: the old record's reference goes
            if (a.state == 2 && freeSlot == nullptr) freeSlot = &a;                // a removed entry can be reused, but the chain goes on (the record may sit behind it)
            if (a.state == 0) { if (freeSlot == nullptr) freeSlot = &a; break; }   // probe chain ends at the first free slot
        }
        if (!done) { if (freeSlot != nullptr) *freeSlot = n; else { g_asyncFull++; NodeUnpin(n.snap); } }
        AsyncUnlock();
    }
};
struct AsyncInstall : exl::hook::impl::InlineHook<AsyncInstall> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) {
        const u64 rec = ctx->X[8]; const u32 gen = At<u32>(reinterpret_cast<void*>(rec), 0x24);
        AsyncCtx a = {}; bool found = false;
        AsyncLock();
        for (int i = 0; i < kAsync; i++) {
            AsyncCtx& e = g_async[((rec / 16) + i) % kAsync];
            if (e.state == 1 && e.rec == rec) { if (e.gen == gen) { a = e; found = true; e.state = 2; } break; }   // consumed: removed (IMPL-14 step 3)
            if (e.state == 0) break;
        }
        AsyncUnlock();
        if (!found) { g_asyncMiss++; TailPush(Thr().tail); return; }   // unknown record: keep the current context, counted
        if (a.epoch != g_N) g_asyncLate++;                             // consumed in a later update than it was submitted (diagnostic)
        TailPush(a.tail, true, a.paired, a.e0, a.raw, a.epoch, a.g, a.gt, a.ft, a.snap, a.gp, a.gtp, a.acc, a.live, a.accA, a.accTA, a.accTB);
        // IMPL-26: the record's snapshot reference moves to the installed context and is released by TailPop (AsyncRestore), so a snapshot is never reused while its callback runs.
        // ponytail: ticket said "-1 at consumption"; holding until the callback ends keeps the data alive. Depth > 4 (not saved by TailPush): released at once.
        if (a.snap >= 0) { ThreadCtx& t = Thr(); if (t.depth <= 4) t.ownSnap[t.depth - 1] = true; else NodeUnpin(a.snap); }
    }
};
struct AsyncRestore : exl::hook::impl::InlineHook<AsyncRestore> {
    static void Callback(exl::hook::InlineFloatCtx*) { TailPop(); }
};

// IMPL-9 item 7: 0x643460 is the "unscaled" timing getter (bit 4 of [obj+0xD0] set): `fmov s0,#1.0; ret`. A hook before the fmov would be overwritten, so it sits on
// the next instruction (0x643464, the epilogue ldp): s0 = the admitted raw step of this update (no override, no token factor). Covers 0xC5C5E0.
struct RawStepGetter : exl::hook::impl::InlineHook<RawStepGetter> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { if (g_N != 0) ctx->S[0] = RawStepNow(); }
};

// D: at 0x670C1C (before the cset that follows the pairing test) w21 = (w24 & ~flags) != 0; paired = (w21 == 0).
struct PairHook : exl::hook::impl::InlineHook<PairHook> {
    static void Callback(exl::hook::InlineFloatCtx* ctx) { g_paired = (ctx->W[21] == 0); }
};

// C1: replacement for the frame timer. Only caller: 0x670C24 (main loop), so the globals follow that one object.
// IMPL-52: one [B3DYN] line every 600 updates when any counter changed (and at n == 600). Fixed stack buffer, no heap; every value is a running total since boot.
inline void B3DynLog() {
    if (g_N % 600 != 0 || g_N == 0) return;
    static u64 last[21]; static bool any = false;
    const u64 v[21] = {g_G, g_dGt1, g_gateShort, g_nodeFull, g_nodeHigh, g_nodeOverflow, g_ovfFull, g_pinRace, g_nodeStaleTok, g_snapNotPool, g_cntEvictLive, g_anchorFull, g_genEvictLive, g_velEvictLive, g_pickEvictLive, g_phEvictLive, g_asyncFull, g_thrOverflow, g_stepStale, g_stepFull, g_stepZeroUnk};
    bool changed = g_N == 600 && !any;
    for (int i = 0; i < 21; i++) if (v[i] != last[i]) changed = true;
    if (!changed) return;
    any = true; for (int i = 0; i < 21; i++) last[i] = v[i];
    static const char* const nm[21] = {"G", "dGt1", "gateShort", "nodeFull", "nodeHigh", "ovf", "ovfFull", "pinRace", "staleTok", "snapNotPool", "cntEvictLive", "anchorFull", "genEvictLive", "velEvictLive", "pickEvictLive", "phaseEvictLive", "asyncDrop", "thrOverflow", "stepStale", "stepFull", "stepZeroUnk"};
    constexpr int kCap = 704;   // IMPL-55: 21 u64 fields at max = 679 B + NUL. IMPL-54: every append is bounded, the line truncates, b[n] is always NUL
    char b[kCap]; int n = 0;
    auto ch = [&](char c) { if (n < kCap - 1) b[n++] = c; };
    auto put = [&](const char* t) { while (*t) ch(*t++); };
    auto num = [&](u64 x) { char d[20]; int k = 0; do { d[k++] = static_cast<char>('0' + x % 10); x /= 10; } while (x); while (k) ch(d[--k]); };
    put("[B3DYN] n="); num(g_N);
    for (int i = 0; i < 21; i++) { ch(' '); put(nm[i]); ch('='); num(v[i]); }
    b[n] = 0;
    svcOutputDebugString(b, n);
}
HOOK_DEFINE_REPLACE(FrameTimer) {
    static void Callback(void* timer, float fixedStep, u32 measured) {
        g_logicThread = ThreadId();
        u64 tick = Game<u64 (*)()>(kGetSystemTick)();
        u64 last = At<u64>(timer, 0xD8);
        At<u64>(timer, 0xD8) = tick;
        float raw = 1.0f, e0 = 1.0f;            // raw = real frame time in ticks (no override); e0 = value stored for the game
        if (last != 0) {
            s64 freq = **reinterpret_cast<s64**>(exl::util::modules::GetTargetStart() + kGotTickFreq);
            float ms = static_cast<float>(static_cast<double>(static_cast<s64>(tick - last)) / static_cast<double>(freq)) * 1000.0f;
            At<float>(timer, 0xF0) = ms;
            raw = ms / At<float>(timer, 0xEC);  // +0xEC = 1000/60
        }
        // IMPL-8 item 3 (+ review rounds 9, 10): ONE raw-budget stream as a FIFO queue of {raw, override at origin}. Each update pushes its raw and its
        // override, then admits up to kMaxStep raw from the front. Ticks and token segments use the admitted raw; E0 = sum(admitted x override_i), stored
        // exactly (no cap, no floor; a zero-length interval publishes E0 = 0 exactly, no epsilon, no debt: IMPL-9 item 1, review round 12). raw > 15 ticks (250 ms) is a stall:
        // the queue is cleared before and after, so only kMaxStep of that update is kept.
        float gc;
        if (measured) {
            float ov = last != 0 ? Game<float (*)()>(kStepOverride)() : 1.0f;  // sound-sync ratio per 60 Hz frame; scales only the stored step
            if (!(ov > 0.0f)) ov = 1.0f;
            if (raw > kStallTicks) g_qn = 0;
            QPush(raw, ov);
            gc = QAdmit(kMaxAdmit, e0);
            g_qn = 0;   // IMPL-44: drop every unadmitted piece (g_adm stays: token segments read it)
            if (raw > kStallTicks) g_qn = 0;   // the rest of the stalled update is discarded
        } else {
            e0 = raw = gc = std::min(fixedStep, kMaxAdmit);   // IMPL-44 item 2
        }
        At<float>(timer, 0xE0) = e0;
        g_e0 = e0;
        if (measured) {
            At<u32>(timer, 0xE4) = 0;
            At<u32>(timer, 0xE8) = 0;
            At<u32>(timer, 0xF4) = 0;
        }
        if (!measured) { g_admN = 0; if (gc > 0.0f) g_adm[g_admN++] = {gc, 1.0f}; }
        g_step = gc;
        g_accBefore = g_acc;
        g_acc += gc;
        g_T = static_cast<int>(g_acc);
        g_acc -= g_T;
        if (g_T > 1) { g_dGt1++; g_T = 1; g_acc = g_accBefore; }   // IMPL-44 item 3: float rounding (0.99999994 + 1.0 = 2.0): one tick, phase unchanged
        g_Gp = g_G;
        g_G += g_T;
        // D: tail clock, half rate while the two-pass pairing is on
        g_ft = g_paired ? gc * 0.5f : gc;
        g_accTBefore = g_accT; g_Gtp = g_Gt;
        g_accT += g_ft;
        int tt = static_cast<int>(g_accT);
        g_accT -= tt;
        if (tt > 1) { g_dGt1++; tt = 1; g_accT = g_accTBefore; }   // IMPL-44 item 3
        g_Gt += tt;
        g_N++;
        g_rawNow = raw;
        Publish();   // IMPL-31 item 2: (a) publish the new clock as one immutable node
        B3DynLog();   // IMPL-52
    }
};

// Write one instruction, only when the original word is the expected one.
void Patch(exl::patch::RandomAccessPatcher& p, uintptr_t addr, u32 expected, u32 value) {
    if (p.Read<u32>(addr) == expected) p.Write<u32>(addr, value);
}

}  // namespace

// IMPL-38 item 2: gate on the submit function 0x98AAB0 (void; no caller uses a result). Only a d = 0 note from the 0x10AEB8 hook makes it skip; otherwise the native call.
struct SubmitGate : exl::hook::impl::TrampolineHook<SubmitGate> {
    static u64 Callback(u64 x0, u64 x1, u64 x2, u64 x3, u64 x4, u64 x5, u64 x6, u64 x7,
                        float64x2_t v0, float64x2_t v1, float64x2_t v2, float64x2_t v3,
                        float64x2_t v4, float64x2_t v5, float64x2_t v6, float64x2_t v7) {
        ThreadCtx& t = Thr();
        if (!t.subOn) return SubmitGate::Orig(x0, x1, x2, x3, x4, x5, x6, x7, v0, v1, v2, v3, v4, v5, v6, v7);
        t.subOn = false;
        return 0;
    }
};

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();

    // C2 presentation interval 0, C3 measured step in every mode, no second logic pass.
    exl::patch::RandomAccessPatcher p;
    Patch(p, 0xD0D038, 0xB876D901, 0x2A1F03E1); // ldr w1,[x8,w22,sxtw #2] -> mov w1, wzr
    Patch(p, 0xCFD994, 0xB8687921, 0x2A1F03E1); // ldr w1,[x9,x8,lsl #2]   -> mov w1, wzr
    Patch(p, 0x670C20, 0x0A0802A1, 0x52800021); // and w1,w21,w8           -> mov w1, #1
    Patch(p, 0x670D00, 0xB9400275, 0xD503201F); // ldr w21,[x19]           -> nop
    Patch(p, 0x297E4C, 0x9412C551, 0x2A1F03E0); // bl 0x749390 (phase-5 collision query) -> mov w0,wzr; P5Drift runs the query per unit (IMPL-14 step 5b)

    Patch(p, 0xC1F868, 0xB9402DCF, 0x2A0F03EF); // ldr w15,[x14,#0x2c] -> mov w15,w15; TokenHook loads the value itself (IMPL-9 item 4)
    p.Flush();   // the token hook copies the patched word at 0xC1F868

    FrameTimer::InstallAtOffset(kFrameTimer);

    // C4 table, generated: HOOK_INT(addr, reg, k, sign, basereg, off, exactl) and HOOK_INT_AFTER(same)
#define HOOK_INT(addr, reg, k, sign, basereg, off, exactl) IntHook<addr, reg, k, sign, basereg, off, exactl>::InstallAtOffset(addr);
#define HOOK_INT_AFTER(addr, reg, k, sign, basereg, off, exactl) IntHookAfter<addr, reg, k, sign, basereg, off, exactl>::InstallAtOffset(addr);
#define HOOK_INT_REC(addr, reg, k, sign, basereg, off, exactl) IntHook<addr, reg, k, sign, basereg, off, exactl, false, true>::InstallAtOffset(addr);
#define HOOK_INT_AFTER_REC(addr, reg, k, sign, basereg, off, exactl) IntHookAfter<addr, reg, k, sign, basereg, off, exactl, true>::InstallAtOffset(addr);
#define HOOK_INT_KEY_REC(addr, keysite, reg, k, sign, basereg, off, exactl) IntHookKey<addr, keysite, reg, k, sign, basereg, off, exactl>::InstallAtOffset(addr);
#define HOOK_INT_KEY(addr, keysite, reg, k, sign, basereg, off, exactl) IntHookKey<addr, keysite, reg, k, sign, basereg, off, exactl, false>::InstallAtOffset(addr);
#define HOOK_INT_MT(addr, reg, k, sign, basereg, off, exactl) IntHook<addr, reg, k, sign, basereg, off, exactl, true>::InstallAtOffset(addr);
#define HOOK_INT_Z(addr, reg, k, sign, basereg, off, exactl, imm, op) IntHookZ<addr, reg, k, sign, basereg, off, exactl, imm>::InstallAtOffset(addr);
#define HOOK_INT_G(addr, reg, k, sign, basereg, off, exactl, pkind, pimm, cond, op) IntHookG<addr, reg, k, sign, basereg, off, exactl, pkind, pimm, cond>::InstallAtOffset(addr);
#include "hooks_int.inc"
#undef HOOK_INT_Z
#undef HOOK_INT_G
#undef HOOK_INT
#undef HOOK_INT_AFTER
#undef HOOK_INT_MT
#undef HOOK_INT_REC
#undef HOOK_INT_AFTER_REC
#undef HOOK_INT_KEY_REC
#undef HOOK_INT_KEY

    SubmitGate::InstallAtOffset(0x98AAB0);   // IMPL-38 item 2
    TimerPlan::InstallAtOffset(0x10AE6C);    // IMPL-41
    Gate19E538::InstallAtOffset(0x19E538);   // IMPL-43 item 3a
    GateSpawn47574::InstallAtOffset(0x47574);   // IMPL-51 item 1
    GateB3CEEE0::InstallAtOffset(0x3CEEE0);   // IMPL-53
    StepWriter4423A0::InstallAtOffset(0x4423A0);   // IMPL-54
    StepReader442FE0::InstallAtOffset(0x442FE0);   // IMPL-54
    GateD6540::InstallAtOffset(0xD6540);     // IMPL-43 item 3b
    // IMPL-45 item 3 / IMPL-46: gate address, counter site, Lt (b.lt: N = 1, V = 0) else b.ne (Z = 0), counter base reg + offset (= the Rec hook's)
#define FG(gate, site, lt, base, off) FlagGate<site, lt, base, off>::InstallAtOffset(gate);
    FG(0x5441C, 0x54418, true, 24, 508) FG(0x1005D0, 0x1005CC, true, 21, 0) FG(0x1F1A18, 0x1F1A0C, false, 21, 2616) FG(0x3BE704, 0x3BE700, true, 0, 6148) FG(0x4FFAAC, 0x4FFAA0, false, 19, 11040)
    FG(0x500678, 0x50066C, false, 19, 11052) FG(0x501244, 0x501238, false, 19, 11032) FG(0x5DCA40, 0x5DCA3C, true, 0, 8288) FG(0x6E1C30, 0x6E1C24, false, 8, 0) FG(0x781560, 0x781554, false, 19, 6192)
    FG(0x7A2348, 0x7A233C, false, 19, 56) FG(0x7A2398, 0x7A238C, false, 19, 68) FG(0x7A2464, 0x7A2458, false, 19, 112) FG(0x494CD4, 0x494CC8, false, 22, 44) FG(0x4D5D20, 0x4D4C68, false, 23, 1972)
#undef FG
    // IMPL-49: chain gates. CG(gate, key, base, off, id, acc, first, last, hi)
#define CG(g, key, base, off, id, acc, first, last, hi) ChainGate<key, base, off, id, acc, first, last, hi>::InstallAtOffset(g);
    CG(0xE9680, 0xE9650, 22, 40, 1, 2, true, false, false) CG(0xE9874, 0xE9650, 22, 40, 1, 2, false, true, false)
    CG(0xE9840, 0xE9650, 22, 40, 2, 4, true, false, false) CG(0xE99C8, 0xE9650, 22, 40, 2, 4, false, true, false)
    CG(0xE9994, 0xE9650, 22, 40, 3, 8, true, false, false) CG(0xE99DC, 0xE9650, 22, 40, 3, 8, false, true, false)
    CG(0xEAC68, 0xEAC38, 25, 2616, 4, 16, true, false, false) CG(0xEAC94, 0xEAC38, 25, 2616, 4, 16, false, false, false) CG(0xEACA4, 0xEAC38, 25, 2616, 4, 16, false, false, true)
    CG(0xEAD9C, 0xEAC38, 25, 2616, 5, 32, true, false, false) CG(0xEADC8, 0xEAC38, 25, 2616, 5, 32, false, false, false)
    CG(0xEADD8, 0xEAC38, 25, 2616, 0, 48, false, true, true)   // last gate of chains 4 and 5
    CG(0xEAED0, 0xEAC38, 25, 2616, 6, 64, true, false, false) CG(0xEAF00, 0xEAC38, 25, 2616, 6, 64, false, true, false)
#undef CG
    CallGateCD86C::InstallAtOffset(0xCD86C);
    RegGate5DD530::InstallAtOffset(0x5DD530);
    TimerExit::InstallAtOffset(0x10B150);    // IMPL-41
    BudFill::InstallAtOffset(0xC1F794); BudReset35C::InstallAtOffset(0xC1F35C); BudReset4D0::InstallAtOffset(0xC1F4D0);   // IMPL-57
    TokenHook::InstallAtOffset(0xC1F868);
    TokenPoolPub::InstallAtOffset(0xC1F894);
    TokenSinglePub::InstallAtOffset(0xC1F074);
    TokenRearm::InstallAtOffset(0xC1F134);
    TokenRearm::InstallAtOffset(0xC1F13C);
    TokenSetScale::InstallAtOffset(0xC1F0E8);   // IMPL-12 item 3(b)
    // IMPL-29 item 3: after each publication store. 0xC1F850 (loop head, reached after the str at 0xC1F848 via the cbz at 0xC1F84C) and 0xC1F998 (loop exit ret) cover the pool store;
    // 0xC1F098 = ret after the single publication str 0xC1F094; 0xC1F0FC = ret after the set-scale stores; 0xC1F140 = ret after the arm stores.
    PubBatch::InstallAtOffset(0xC1F998);   // token loop exit (ret)
    PubBatch::InstallAtOffset(0xC1F098);   // single publication (ret)
    PubBatch::InstallAtOffset(0xC1F0FC);   // set-scale (ret)
    PubBatch::InstallAtOffset(0xC1F140);   // rearm (ret)
    PubBatch::InstallAtOffset(0xC1F354);   // +0xC4 writer: reset block (stur x9,[x19,#0xC4] at 0xC1F350)
    PubBatch::InstallAtOffset(0xC1F4C8);   // +0xC4 writer: init block (stur x8,[x0,#0xC4] at 0xC1F4C4)
    PubBatch::InstallAtOffset(0xC1F780);   // +0xC4 writer: channel budgets (str s1,[x0,#0xC4] at 0xC1F77C)
    PairHook::InstallAtOffset(0x670C1C);
    OwnerHook::InstallAtOffset(0x1B1A60);   // IMPL-10 item 5
    OwnerScope<0x1B1A24>::InstallAtOffset(0x1B1A24);   // IMPL-14 step 4: scope of the step override and the credit cap
    OwnerDamp::InstallAtOffset(0x1B1D2C);
    OwnerDamp::InstallAtOffset(0x1B1E6C);   // IMPL-12 item 7: the two remaining copies (same registers x19/x22/x23, same 9 instructions before the ldr q2)
    OwnerDamp::InstallAtOffset(0x1B1F04);
    OwnerDamp::InstallAtOffset(0x1B22B8);   // IMPL-11 item 2: the copy in state 3 (0x1B22B0..0x1B22CC)
    KdEnter::InstallAtOffset(0x2978BC);     // IMPL-10 item 6 (kick-drift, phases 2 and 3)
    KickDrift::InstallAtOffset(0x297B24);
    RawStepGetter::InstallAtOffset(0x643464);
    P5Drift::InstallAtOffset(0x297D88);     // IMPL-11 item 1: phase 5 segment loop, collision, 0.85, timer, successor phase
    P5Rotate::InstallAtOffset(0x297DC8);
    P5Expiry::InstallAtOffset(0x297EA8);
    Clamp359::InstallAtOffset(0x359AEC);   // IMPL-24
    ClothPre::InstallAtOffset(0x813154);   // IMPL-28 item 1
    ClothPost::InstallAtOffset(0x813158);
    ClothRec::InstallAtOffset(0x819FCC);   // IMPL-28 item 2
#if REPLAY_371814
    owner371::Replay<0x371814>::InstallAtOffset(0x371814);   // IMPL-17
    owner371::CallerFix::InstallAtOffset(0x371804);
    owner371::NoReset::InstallAtOffset(0x371914);
    owner371::PreBiasUp::InstallAtOffset(0x371A20);
    owner371::Scale1FE0::InstallAtOffset(0x371C70);
    owner371::CascadeA28::InstallAtOffset(0x371C7C);
    owner371::CascadeMode::InstallAtOffset(0x371C80);
    owner371::PreBiasDown::InstallAtOffset(0x371EAC);
#else
    CrossSucc::InstallAtOffset(0x371D60);   // IMPL-11 item 3
#endif
    PairBegin::InstallAtOffset(0x55A400);   // IMPL-26 item 3
    PairEnd::InstallAtOffset(0x55A410);
    PickDraw1::InstallAtOffset(0x55BAA0);   // IMPL-26 item 2
    PickStoreR<17>::InstallAtOffset(0x55BAEC);
    PickStoreR<11>::InstallAtOffset(0x55BB28);
    PickDraw2::InstallAtOffset(0x55BB2C);
    Reload55Mark::InstallAtOffset(0x55AF90);   // IMPL-17 item 6
    Reload55Apply::InstallAtOffset(0x55AF94);
    PhaseWriter::InstallAtOffset(0x66FEC4); // IMPL-11 item 4
    PhaseGate::InstallAtOffset(0x3EBB90);

    // IMPL-9 item 5 + IMPL-10 items 3, 4: zero-interval adapters (tools/recip-sites.tsv; rule A keep the stored value, rule B per-object cache)
    RecipFlag<0x5A09AC, 0>::InstallAtOffset(0x5A09AC);
    RecipKeep<0x5A09AC, 0, 0x5A09B0, 0, 19, 0x1690>::InstallAtOffset(0x5A09B0);
    RecipFlag<0x59A244, 5>::InstallAtOffset(0x59A244);
    RecipKeep<0x59A244, 5, 0x59A260, 2, 19, 0x1DE0>::InstallAtOffset(0x59A260);

    RecipFlag<0x59478C, 0>::InstallAtOffset(0x59478C);                       // 0x594788 fdiv s0,1.0,step/60; velocity stays on the stack (rule B)
    RecipCache<0x59478C, 0, 0, 19>::InstallAtOffset(0x594790);               // stur q0,[x29,#-0x68]; key = x19 (the object)
    RecipFlag<0x37342C, 2>::InstallAtOffset(0x37342C);                       // 0x373428 fdiv s2,s2,s0
    RecipKeep<0x37342C, 2, 0x373464, 2, 22, 0x24>::InstallAtOffset(0x373464);   // str s2,[x22,#0x24]
    RecipFlag<0x375E44, 2>::InstallAtOffset(0x375E44);                       // 0x375E40 same shape
    RecipKeep<0x375E44, 2, 0x375E7C, 2, 22, 0x24>::InstallAtOffset(0x375E7C);
    RecipFlag<0x60C904, 0>::InstallAtOffset(0x60C904);                       // 0x60C900 fdiv s0,1.0,s0; fmul at 0x60C904
    RecipKeep<0x60C904, 0, 0x60C918, 0, 23, 0>::InstallAtOffset(0x60C918);   // stp q0,q4,[x23]
    RecipFlag<0x611DE4, 5>::InstallAtOffset(0x611DE4);                       // 0x611DE0 fdiv s5,s5,s0
    RecipKeep<0x611DE4, 5, 0x611E2C, 2, 19, 0x1DB0>::InstallAtOffset(0x611E2C); // str q2,[x19,#0x1db0]
    RecipFlag<0xBE30AC, 0>::InstallAtOffset(0xBE30AC);                       // 0xBE30A8 fdiv s0,s4,s0 (s0 = timer step)
    RecipKeep<0xBE30AC, 0, 0xBE30CC, 8, 19, 0x80>::InstallAtOffset(0xBE30CC); // s8 (blend, stored at [x19+0x80]) keeps the stored value
    RecipFlag<0xBE3164, 0>::InstallAtOffset(0xBE3164);                       // 0xBE3160 fdiv s0,s4,s0
    RecipKeepSum<0xBE3164, 0, 0xBE318C, 2, 5, 19, 0x84>::InstallAtOffset(0xBE318C); // fadd s8,s2,s5 -> s8 = stored [x19+0x84]

    // IMPL-8 item 7: tail scope, getter halving, async context
    AnimLiteral::InstallAtOffset(0xB5D3E0);   // IMPL-12 item 8
    TailEnter::InstallAtOffset(0x671520);
    TailExit<0x696A64>::InstallAtOffset(0x696A64);
    TailGetter<0xC1F0A8>::InstallAtOffset(0xC1F0A8); TailGetter<0xC1F0C0>::InstallAtOffset(0xC1F0C0); TailGetter<0xC1F0C8>::InstallAtOffset(0xC1F0C8);
    TailGetter<0xC1F0D8>::InstallAtOffset(0xC1F0D8); TailGetter<0xC1F0E4>::InstallAtOffset(0xC1F0E4);
    AsyncCapture::InstallAtOffset(0xD04FD0);
    AsyncInstall::InstallAtOffset(0xD04E88);
    AsyncRestore::InstallAtOffset(0xD04E8C);

    // RELOAD sites, generated by tools/reload.py: HOOK_RELOAD(addr, creg, is_float_reg, xreg)
#define HOOK_RELOAD(addr, c, isf, x) ReloadHook<addr, c, isf != 0, x>::InstallAtOffset(addr);
#define HOOK_RELOAD_SUB(addr, x, pr)
#include "reload.inc"
#include "reload_float.inc"
#undef HOOK_RELOAD_SUB
#undef HOOK_RELOAD
#define HOOK_RELOAD(addr, c, isf, x)
#define HOOK_RELOAD_SUB(addr, x, pr) ReloadSubBefore<addr, x>::InstallAtOffset(addr); ReloadSubAfter<addr, x, pr>::InstallAtOffset(addr + 4);
#include "reload.inc"
#undef HOOK_RELOAD_SUB
#undef HOOK_RELOAD

    // slot-167 call sites (W6, the three proven repeating ones)
    Slot167<0xEB368>::InstallAtOffset(0xEB368);
    Slot167<0x17CE90>::InstallAtOffset(0x17CE90);
    Slot167Gen::InstallAtOffset(0x17CD38);
    StateSetGen::InstallAtOffset(0x58C7B8);   // IMPL-26 item 4; IMPL-31 item 3: moved to the first store

    // IMPL-14 step 1: multi-expiry float reloads (SIMPLE shape), generated by tools/reload_multi.py. IMPL-25: MULTI_EXPIRY 0 keeps every site off except 0x8DD54 (always installed).
#define HOOK_MULTI(addr, c, x, rb, off, call, tgt) if constexpr (MULTI_EXPIRY != 0 || (addr) == 0x8DD54) { \
    MultiHook<addr + 4, c, x, rb, off, call, tgt>::InstallAtOffset(addr + 4); \
    if constexpr ((call) != (addr) + 4) MultiCall<call, tgt>::InstallAtOffset(call); \
    MultiAfter<(call) + 4, tgt>::InstallAtOffset((call) + 4); }
#include "reload_multi.inc"
#undef HOOK_MULTI

    // IMPL-25 item 2: 0x56DBF0 phase correction only on established expiries; item 4: 0xB01C repeat expiries
    OldTimer<0x56DBDC, 1, 21>::InstallAtOffset(0x56DBDC);
    ReloadOld<0x56DBF0, 8, false, 0, 21>::InstallAtOffset(0x56DBF0);
    ExpiryMark<0xB01C, 8, 0, 19>::InstallAtOffset(0xB01C);
    ExpiryExtra<0xB054, 19>::InstallAtOffset(0xB054);

    // RELOAD-INT, generated by tools/reload_int.py
#define HOOK_RELOADI(addr, rt, rb, off, size, site) ReloadInt<addr, rt, rb, off, size, site>::InstallAtOffset(addr);
#include "reload_int.inc"
#undef HOOK_RELOADI
#define HOOK_RELOADIB(addr, rt, rb, off, size, site) ReloadIntBefore<addr, rt, rb, off, size, site>::InstallAtOffset(addr);
#include "reload_int_before.inc"
#undef HOOK_RELOADIB
#define HOOK_RESET_CARRY(addr, site, base, off, k, sign, exactl, valreg, mt) ResetCarry<addr, site, base, off, k, sign, exactl, mt != 0, valreg == 31>::InstallAtOffset(addr);   /* valreg 31 = the reset stores wzr: the hook checks the field is 0 (IMPL-25) */
#include "reset_carry.inc"
#undef HOOK_RESET_CARRY

    // C6 float table, generated: HOOK_FADD(addr, dreg, c, sign, basereg, off), HOOK_FSMOOTH(addr, reg, a, kind, basereg, off)
#define HOOK_FADD(addr, reg, c, sign, basereg, off) FAddHook<addr, reg, std::bit_cast<u32>(c), sign, basereg, off>::InstallAtOffset(addr);
#define HOOK_FSMOOTH(addr, reg, a, kind, basereg, off) FSmoothHook<addr, reg, std::bit_cast<u32>(a), kind, basereg, off>::InstallAtOffset(addr);
#define HOOK_FCLOSED(addr, rx, rz, a, t, base, off, mem) FClosed<addr, rx, rz, std::bit_cast<u32>(a), std::bit_cast<u32>(t), base, off, mem>::InstallAtOffset(addr);
#define HOOK_FSUBUNITS(addr, base, off) FSubUnits<addr, base, off>::InstallAtOffset(addr);
#define HOOK_GAINCLOSED(addr) GainClosed<addr>::InstallAtOffset(addr);
#define HOOK_GAINMOVE(addr) GainMove<addr>::InstallAtOffset(addr);
#define HOOK_GAINPREV(addr, key, reg) GainPrev<addr, key, reg>::InstallAtOffset(addr);
#define HOOK_YAWUNITS(addr, key, sign) YawUnits<addr, key, sign>::InstallAtOffset(addr);
#define HOOK_YAWREC(addr, owner) YawRecord<owner>::InstallAtOffset(addr);
#define HOOK_CALSEAM(addr, dreg, abase, aoff, goff, gain, stash) CalSeam<addr, dreg, abase, aoff, goff, std::bit_cast<u32>(gain), stash>::InstallAtOffset(addr);
#define HOOK_TARGETSTASH(addr) TargetStash::InstallAtOffset(addr);
#define HOOK_GAINANCHOR(addr) GainAnchor<addr>::InstallAtOffset(addr);
#define HOOK_CALMOVE(addr) CalMove::InstallAtOffset(addr);
#define HOOK_FSUBGATE(addr) FSubGate::InstallAtOffset(addr);
#define HOOK_AMPSTASH(addr, sign) AmpStash<sign>::InstallAtOffset(addr);
#define HOOK_RAMP(addr, base, off) RampP<addr, base, off>::InstallAtOffset(addr);
#define HOOK_RAMPY(addr) RampY<addr>::InstallAtOffset(addr);
#include "hooks_float.inc"
#include "hooks_float_manual.inc"
#undef HOOK_FCLOSED
#undef HOOK_FSUBVAR
#undef HOOK_RAMP
#undef HOOK_RAMPY
#undef HOOK_FADD
#undef HOOK_FSMOOTH

    // C4b handler gates, generated: HANDLER_GATE(addr)
#define HANDLER_GATE(addr) HandlerGate<addr>::InstallAtOffset(addr);
#include "handler_gate.inc"
#undef HANDLER_GATE
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
