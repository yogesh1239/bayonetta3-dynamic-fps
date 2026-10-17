# Bayonetta 3 — Dynamic Unlocked FPS: architecture v10 (agreed in review round 13, 2026-10-08)

The v10 section at the END of this file overrides older text below where they differ. Older sections are kept for history.

Date: 2026-10-08. Status: v10 AGREED (review round 13). Build in progress (IMPL-9). Nothing installed.

## Plain summary

| Term | Meaning |
|---|---|
| Step | Game time of one frame, in 60 FPS frames. 1.0 = 1/60 s. 0.5 = 1/120 s. |
| Tick | One whole 1/60 s of game time. |
| Tick count (T) | How many whole ticks finished during this frame: 0, 1 or 2. |
| Module | A small extra program (`subsdk9`) that Eden loads next to the game. It can change game code when the game starts. The Super Mario Odyssey mod uses the same kind of module. |
| Hook | A small change that makes the game run a piece of our code at one exact place. |

What the mod does:

1. **Real step.** The game already measures each frame. The mod removes the lower limit (1.0), so above 60 FPS the step goes below 1.0. The upper limit stays 2.0, as in the stock game. Below 30 FPS the game goes into slow motion, as now.
2. **No frame cap.** The game asks the screen for "no cap" in every mode.
3. **Cutscenes use real time.** Cutscenes run one logic pass per picture with the real step, as in the Astral Chain mod. The stock "two passes per 30 FPS picture" stays off, as in theboy181's mod.
4. **Counters count ticks, not frames.** About 350 places add 1 (or subtract 1) on every frame. The mod changes each one to add T instead. At 120 FPS, T is 1 on every second frame and 0 on the other, so the counter still moves 60 times per second. At 40 FPS, T is sometimes 2.
5. **One fix for the per-character "1.0".** Each character keeps a fixed 1.0 that 162 places read as "one tick". The mod writes the real step there on every update.
6. **Decimal timers and smoothing.** Places that add a fixed decimal amount per frame (for example 1/60) or smooth by a fixed factor get a fix each. Each fix uses the step.

What this will not fix:

- Below 30 FPS the game still goes into slow motion. That is the stock limit and it protects the physics.
- Movie files play at their own speed.
- The Bayonetta Origins prologue is a separate program. It is not included.

What could break, and how we check it:

- A counter that waits for an exact number (for example "equals 40") can skip that number when T is 2. Every such place gets a second change (for example "equals 40" becomes "40 or more"). A script finds them; a helper checks each one.
- A hook must not run where the game still needs the result of an earlier comparison. A script checks this for every hook.

## Technical section

### Why a module (exlaunch) and not a pchtxt

- ~350 integer sites + float sites. Per-site cave stubs at 16–24 B exceed the 0xC00 cave at 0x1323400.
- Shared `bl` stubs clobber x30/x16/x17, which LLVM can keep live mid-function.
- exlaunch inline hooks save all GPRs (InlineCtx) and FP registers (InlineFloatCtx), run a C++ callback, then execute the relocated original instruction. Toolchain and template already work on this PC (Super Mario Odyssey/devkitpro, mod-src, build.sh). B3 ships only subsdk0, so subsdk9 is free. Pool size: raise `setting::InlinePoolSize` for ~500 hooks.
- NZCV is NOT saved by the inline trampoline. Every hook point must have dead flags (checked by script: no flag reader before the next flag setter, including across branches in the same basic-block chain).

### C1 Clock (replace 0xC1F600 body via exlaunch hook or in-place patch)

- step = this frame ms / [+0xEC] (1000/60). No averaging, no carry. Override `0xBA3300` (>0) multiplies step (ratio per 60 Hz frame; V1 in round 2).
- Clamp [0.125, 2.0]. 2.0 keeps the stock ceiling (an earlier draft proposed 4.0; 2.0 keeps physics in its tested range and limits T to 2).
- Tick accumulator in module memory: `acc += step; T = floor(acc); acc -= T;` once per timer call. Store T in a module global `g_T`, and step in `g_step`.
- Keep stock fields: +0xD8 last tick, +0xF0 frame ms, +0xE0 step. +0xE4/+0xE8/+0xF4 stay as stock writes them (zeroed) unless a reader is found (ticket).

### C2 Presentation

- Interval 0 at the two producers 0xD0D038 and 0xCFD994 (`mov w1, wzr`, review round 1 VERIFIED). Do not use theboy181 0xCFD96C.

### C3 Modes

- 0x670C20 `and w1, w21, w8` -> `mov w1, #1`: measured step in every mode (cutscene pair and mode 2).
- Keep theboy181 0x670D00 NOP (bit 27 never set -> no second logic pass). Same result as the tested theboy181 + Astral Chain behaviour.
- Open for review: pass-2-only jobs (0x671600 path) are dropped, as in theboy181's mod. Show any that must still run.

### C4 Integer per-update counters (FIX-COUNTER ledger) — "smart tick" rule (v3.1)

Plain: each counter remembers when it last moved. If the game also ran it in the previous update (a continuous counter), it moves by the 1/60-second ticks that passed since then. If it did not run in the previous update (a one-off event, or the first update of a run), it moves by 1, exactly as in the stock game. So a held counter keeps the right speed, and a one-off event is never lost.

- Global: G = total ticks so far (G += T per update), N = update number (+1 per timer call).
- Per counter key = (site index, field address = X[base] + off). Small open-addressing table in the module: {key -> (S = G at last credit, U = N at last call)}.
- Callback before the original `add/sub/subs wN, wN, #k`: if U == N-1 (ran last update) then d = G - S else d = 1; then W[N] += sign*(d-1)*k; S = G; U = N.
- Results: held at 120 FPS: 1,0,1,0 -> 60/s. Held at 30 FPS: 2 per update -> 60/s. Event at any FPS: +1. 60 FPS: identical to stock.
- Known limit: the same site and the same field twice in one update (a loop) credits only once. Such sites are item loops (NOT-TIMER) and are not hooked.
- Because events stay exact, OPEN rows that are "held or edge?" can be hooked safely. Misclassifying an event counter as FIX no longer breaks it.
- Exact-compare follow-ups still apply when d can be 2 (below 60 FPS).

- v3.2 floor rule (2026-10-08): for sign -1 (countdowns) the extra part never takes the counter below 0: extra = min((d-1)*k, max(W_before - k, 0)). The stock step (-k) still runs, so a counter at 0 still goes to -1 exactly as stock. Result: a countdown never skips 0, so `subs; b.ne/b.eq/b.lt` against 0 works unchanged and the old `subs; b.lt` skip-store trap (IMPL-3 doubt 1) cannot happen. All 16 sign -1 compare patches are dropped (patching ne->gt would change the stock -1 case).
- Up-counter exact compares: the only listed one (0x371CA8, +0xa24 == 5) advances once per 6 updates, so d is always 1 there. No compare patch is applied. Rule kept for the future: patch only when the site can run in consecutive updates.
- Wrap counters `add w9,w8,#1; cmp w9,#N; csinc w8,wzr,w8,hi` (0x371C8C): hook BEFORE on the source register w8 (w8 += d-1). Then w9 and the csinc both see the advanced value; a wrap loses at most the fractional tick.

### C4b Handler call gate (E24C emission, v3.2)

- obj+0xE24C is a spawn/pattern ordinal (+1 once per handler call right after a spawn; readers use mod 3/4/5/8, a cap of 7, 60-F). 59 handler functions write it (research/scan/e24c-family.tsv). The ordinal is NOT a timer: all its counter rows are NOT-TIME.
- The time dependence is the call rate: dispatcher 0x8B44C re-runs an action-record case every update while its window is open, so a per-update emission spawns twice as often at 120 FPS.
- Fix: trampoline hook on each handler function. Key (handler, x0). If the handler ran in the previous update, call the original n = G - S times (0, 1 or 2; clamp 4); else n = 1 (event, stock). Always record U = N, S = G. Timer-gated callers (for example 0x8BA0C, a 10.0 float timer) never call in consecutive updates, so they get n = 1.
- Conditions checked by script per handler: callers ignore the return value; no float arguments; integer args only in x0..x7. Integer counter hooks inside these handlers are excluded (else double correction).

(Original v3 text follows.)

- Hook at the `add/sub/subs wN, wN, #k` instruction. Callback: `W[N] += (T-1)*k` (add) or `W[N] -= (T-1)*k` (sub/subs). The original instruction then runs, so the final value is `old ± T*k` and `subs` sets flags from the final value.
- Exact-compare follow-ups: if the counter is later tested with `b.eq/b.ne/cset eq` against a limit, patch the test to `>=` / `<=` (direction from the counter sign). Script lists them; a ticket checks each.
- STEP-CHAIN rows (state word advanced once per update through 2–5 states): T-gate the same way (`+= T`), with the exact-compare rule; chains only advance on ticks.
- Per-update emission (E24C spawn case 0x8DF4C -> 0x2431E4): gate the spawn call on T>0. Other action cases stay ungated.
- Token countdowns 0xC1F874 / 0xC1F900: same T rule (replaces the fixed-point design V4).

### C5 Per-actor step actor+0x160D0 — REMOVED (v3.2)

- Review round 3, verified: 0x7BCA8 and 0x84B28 store the token step (0xC1F09C) to actor+0x160D0 at runtime. The game already publishes the actor step; the module writes nothing there. Old text below is history.


- Hook at the actor update entry (slot 46, 0x83744): `*(float*)(x0+0x160D0) = g_step` before the body runs. Ticket confirms x0 there is the same object class that setup 0x72188 writes.
- Covers 162 reads (Q-10, Q-16). Not covered: actor+0x1748 mixed writers (float ledger).

### C6 Float items (float ledger)

- Fixed float adds `+= c` per update: hook after the add, `S[d] += c*(g_step-1)`.
- Fixed factor smoothing `x *= a` or `x += (t-x)*a`: hook before the multiply, replace the factor register value with `1-(1-a)^g_step` only where that register is dead after (script check); else hook after and recompute.
- Euler integration (0x2978A4 growing-speed move): position add uses `acc - 0.5*a*step` (midpoint) so distance is exact.
- One-tick delta clamps found in review: list in the float ledger, fix per site.

### C7 Sound

- Per-update sound countdowns 0x1213168, 0x12131D8, 0x1209E60, 0x120A4A0: C4 rule. A review says a worker thread also reaches them: if so, T must not apply on that thread. Hook checks the thread (module records the main logic thread id at the first timer call).
- Slot-35 voice countdowns (+0x54): audio clock, no change (pending proof).

### Build and verify

1. Sonnet ticket: module skeleton for B3 (copy SMO mod-src, title id 0x01004A4010FEA000, npdm from game-code-dump/1.2.0/main.npdm), C1–C3, C5.
2. Generator script: reads the merged ledger (address, kind, register, k, exact-compare flag) and emits the hook table (one line per site, no per-site C++).
3. Checker script: for every hook address, verify the instruction bytes in main.flat match the expected `add/sub/subs wN,wN,#k`, flags dead at the hook point, and no two hooks overlap.
4. Unit test: run the callback math with T sequences for 30/45/60/90/120/240 FPS and assert counters advance 60 per second ±1.
5. Install only after the review agrees. Move "60fps v4" to `_disabled-mods-backup`. Keep LOD and Film Grain.


## v10 — agreed design (review rounds 5-13)

Plain summary: the mod measures real time per frame and gives the game exactly that much game time, up to 4 game ticks per frame (correct speed at 15 frames per second and above; user decision). Spare time is kept and given later, in order, with the game speed that applied when it was measured. Counters that move once per frame move by the number of ticks that are due. Where one tick can start an event, the leftover ticks go to the next stage of that event in the same frame. No time is invented and none is lost, except one deliberate rule: after a freeze longer than 250 ms only 4 ticks are kept (as the unmodded game does).

Technical:
1. Clock (0xC1F600 hook). raw = ms / [timer+0xEC]. FIFO of pieces (raw, override_at_origin), no ceiling. Admit up to 4.0 raw per update from the front. E0 = sum(admitted_raw_i x override_i), stored exactly, 0 allowed (zero-length interval). G ticks, token segments b_i and float f use admitted raw. Stall: raw > 15 ticks keeps 4.0 and clears the FIFO.
2. Counters (C4). Smart tick d per (site, field); carry per key (positive only). Crossing: land on L, leftover goes to the successor transaction in the same update (manual adapters where a successor phase exists). Zero-tick at the event value: (a) branch-before-store -> native d = 1, no debt; (b) store-before-branch -> flags changed after the FINAL flag producer so the event branch is not taken; cbz/cbnz -> manual. No borrow.
3. Owner gates: removed. Manual adapters only: 0x1B1A24 (complete state transactions incl. 0xCC4950/0xAE8F8 calls, successor dispatch, damping q = (1 - 0.1 sigma)^r), 0x2978A4 (segmented kick-drift: v += a s; x += v s - 0.5 a s sigma (r - 1); collision 0x297E4C and 0.85 0x297E8C between segments; phase successor), 0x371814 (pre-run owner+caller 0x3717E8 transactions, native last; needs indirect-call contracts first).
4. TOKEN (0xC1F798): budget once per update per token+generation; publication-only visit via X15 = 0 after 0xC1F868; segments b_i, f_i; published = sum(b_i f_i) on both paths (after 0xC1F890 and after 0xC1F070); generation reset 0xC1F134/0xC1F13C.
5. RELOAD: x = P + r for store and register shapes; overlapping wait init (0x914B30, 0x914B8C, 0x8BD374) x = P - 1 + r; guard tests the initialised value; P < 4 or multiplied step source -> manual ordered processing.
6. Tail (paired mode, 0x670C1C X21 == 0): scoped context 0x671520..0x696A64, getters 0xC1F09C/0xC1F0CC return x 0.5 in scope; async context captured at 0xD04FD0, installed around 0xD04E88 (record X8 + generation); tail closure includes 18 roots.
7. Zero-interval adapters on every native reciprocal of a step (0x5A09A8, 0x594788, 0x59A240, 0xBE30A8, 0xBE3160 via wrapper 0x70A050, + script list): keep the previous finite stored value when the step is 0.
8. Exclusions: audio + voice closure (roots incl. 0x1271274, 0x121BCA4, 0x12722CC) except main-thread-only hooks; ordinals / jump-table indices; spawn/pattern ordinals.
9. Thread guard: main-logic-thread-only hooks for shared code (0x1209E60, 0x120A4A0, 0x1213168, 0x12131D8).
10. Bridge: 0x300 frame, LR copy, NZCV in context, far edges abort at install.
Remaining work (not design): finding jobs (indirect-call contracts 0x371814/0x1A7B1C, reciprocal list), manual adapters' build, build verification, fix review. Install only after the fix review agrees.
