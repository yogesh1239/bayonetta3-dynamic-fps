HOOK_FADD(0x6E25BC, 2, 0.0166666675f, +1, 9, 0) // manual: fadd age, age, 1/60 at 0x6E25B8, f_tail (IMPL-7 item D)
HOOK_FADD(0xCAEF04, 1, 0.0166666675f, +1, 19, 16) // manual: fadd s1,s1,1/60 at 0xCAEF00, hook on the fcmp after it (IMPL-20 item 3)
HOOK_FADD(0x814228, 0, 0.0166666675f, +1, 19, 3092) // manual: fadd s0,s0,1/60 at 0x814224 (IMPL-20 item 3)
HOOK_FADD(0x814260, 0, 0.0166666675f, -1, 19, 3092) // manual: fadd s0,s0,-1/60 at 0x81425C; Sign -1 = the literal is negative (IMPL-20 item 3)
HOOK_FADD(0x3D38F8, 1, 0.00833333377f, +1, 19, 6208) // manual: fadd s1,s1,1/120 at 0x3D38F4; half a tick scaled by f (IMPL-20 item 3)
HOOK_GAINCLOSED(0x2BD90) // IMPL-23 SMOOTH: x=a*x+b, a=0.85, A=1; before fadd s1,s0,s1; stashes f and g0 for the movement
HOOK_GAINMOVE(0x2BDAC) // IMPL-23: movement gain factor = S(f)/f (sum of the per-unit gains), before fmul v1,v2,v1.s[0]
HOOK_GAINANCHOR(0x385F68) // IMPL-27 anchor gain (was HOOK_FCLOSED(0x385F68, 1, 0, 0.95, 1.0, 22, -0x34, false)) IMPL-21 SMOOTH: x+=0.05*(1-x), a=0.95, A=1; before fadd s0,s1,s0; x in s1
HOOK_GAINPREV(0x385E48, 0x385F68, 1) // IMPL-23: output at 0x385E50 reads the gain after f-1 units, before fmul s3,s0,s1
HOOK_RAMP(0x9437C, 22, 0x8B4) // IMPL-21 CLAMP+stash: p=min(1,p+0.1f) (hook behind fadd at 0x94378), stashes f and p0 for 0x943BC
HOOK_RAMPY(0x943BC) // IMPL-21 SMOOTH: y ladder over whole ticks + fractional tail (H1 0x943A8)
HOOK_FADD(0x11AAD0, 0, 0.100000001f, +1, 26, 0x8CC) // IMPL-21 CLAMP: x=min(1,x+0.1f), hook behind fadd at 0x11AACC; native fmin after
HOOK_FADD(0x11B480, 0, 0.100000001f, +1, 26, 0x8CC) // IMPL-21 CLAMP: same, fadd at 0x11B47C
HOOK_FADD(0x124C64, 0, 0.100000001f, +1, 25, 0x8CC) // IMPL-21 CLAMP: same, fadd at 0x124C60
HOOK_FSUBUNITS(0x4E824C, 20, 0x130) // IMPL-23 CLAMP: unit replay of x-=q (eligibility 0x4E8074/78 re-tested per unit); before fsub s0,s1,s0
HOOK_FADD(0x4AB9F0, 0, 0.174532920f, +1, 21, 0x2D70) // IMPL-21 FLOAT-ADD: angle+=10deg*f, hook behind fadd at 0x4AB9EC; native wrap after
HOOK_FADD(0x4ABB84, 0, 0.174532920f, +1, 21, 0x2D70) // IMPL-21 FLOAT-ADD: same, fadd at 0x4ABB80
HOOK_FADD(0x56B608, 1, 0.0872664601f, +1, 19, 0x1900) // IMPL-21 FLOAT-ADD: angle+=5deg*f, hook behind fadd s1 at 0x56B604; native wrap after
HOOK_YAWUNITS(0x4AB9DC, 0x4AB9F0, -1) // IMPL-23 FLOAT-ADD: yaw -= sum of per-unit sin(phase0+j*10deg)*10deg*amp; before str s0,[x19,#0x94]
HOOK_YAWUNITS(0x4ABB70, 0x4ABB84, 1) // IMPL-23 FLOAT-ADD: same, yaw +=
HOOK_CALSEAM(0x490B8, 5, 19, 0x15F58, 0x178, 0.0799999982f, false) // IMPL-23 SMOOTH: calibrated map x += 0.08 g (1-x), target 1; before fadd s2,s1,s5
HOOK_TARGETSTASH(0x1158AC) // IMPL-23: stash the target T before fsub s0,s0,s1
HOOK_CALMOVE(0x1158BC) // IMPL-27 (CalSeam fallback inside; was HOOK_CALSEAM(0x1158BC, 3, 21, 0x15F14, 0x1BC, 0.100000001f, true)) IMPL-23 SMOOTH: calibrated map y += 0.1 g (T-y); before fadd s0,s1,s3
HOOK_FSUBGATE(0x4E8074) // IMPL-27: the native ratio gate tests the anchor value while a fractional unit is active
HOOK_AMPSTASH(0x4AB9A0, -1) // IMPL-27: stash amp0 and the ramp step before `fadd s1,s1,s2`
HOOK_AMPSTASH(0x4ABB34, 1) // IMPL-27: same, second yaw site
HOOK_GAINPREV(0x385E68, 0x385F68, 2) // IMPL-30 item 2: the second output reader (ldp s1,s2,[x22,#-56] at 0x385E60, gain in s2), before fmul s1,s0,s2
HOOK_YAWREC(0x4AB9FC, 0x4AB9DC) // IMPL-30 item 5: records the phase bits behind the wrap store (anchor validity of YawUnits 0x4AB9DC)
HOOK_YAWREC(0x4ABB90, 0x4ABB70) // IMPL-30 item 5: same for the second yaw site
