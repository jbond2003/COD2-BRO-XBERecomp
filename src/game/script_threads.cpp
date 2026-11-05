// Script threads. The level scripts are native code; a script that waits copies its part of the guest stack out
// (Scr_SaveThreadLocals) and long jumps back to Scr_ResumeThread, which later copies the stack back to the same
// address and returns into the middle of the script. Recompiled code also keeps frames on the host stack, so each
// script thread runs on a host fiber of its own, suspended while the script waits. The guest still copies its stack.
//
//   Scr_ResumeThread         0x2FE100  (eax thread) first run, or resume of a woken thread
//   Scr_SaveContext          0x2F5690  setjmp into 0x92709C, only from Scr_ResumeThread
//   Scr_RestoreContext       0x2F5660  longjmp, the tail of the wait functions (iface slots 15 to 17)
//   Scr_SuspendWaitTime      0x11D950  \
//   Scr_SuspendWaitFrames    0x11D990   } pushfw, pusha, push fs:[0], save esp, Scr_SaveThreadLocals, wait
//   Scr_SuspendWaitTill      0x11D9D0  /
//   Scr_DestroyThreadContents 0x2FC540 (edi thread)

#include <unordered_map>
#include <vector>

#include "core/fiber.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

using kernel::R32;
using kernel::W32;

namespace {

constexpr uint32_t kCurrentThread = 0x8DDA2C;
constexpr uint32_t kIface = 0x95F49C;  // pointer to the engine interface the scripts call
constexpr uint32_t kResumeContext = 0x92709C;
constexpr uint32_t kUpdateThreadWaits = 0x2FC440;  // esi thread
constexpr uint32_t kThreadEntry = 0x1FA400;        // ecx thread object
constexpr uint32_t kRestoreThreadStack = 0x2F6600; // eax saved blocks, ecx destination
constexpr uint32_t kSaveThreadLocalsSlot = 0x922174;

// thread flags (+0x10)
constexpr uint32_t kFirstRun = 1, kWaiting = 2, kWoken = 4, kDone = 8, kSuspended = 0x10, kKilled = 0x40;

struct Script {
    uint32_t thread = 0;
    fiber::Fiber* f = nullptr;
    uint32_t start_esp = 0, object = 0;
    bool done = false, dead = false;
};

std::unordered_map<uint32_t, Script*> g_scripts;
std::vector<fiber::Fiber*> g_pool;
fiber::Fiber* g_sched = nullptr;
Script* g_running = nullptr;
X86Context* g_ctx = nullptr;
uint8_t* g_mem = nullptr;

void FiberMain(void*) {
    for (;;) {
        Script* s = g_running;
        X86Context& ctx = *g_ctx;
        ctx.esp = s->start_esp - 4;
        W32(ctx.esp, 0x2FE17B);  // where the original calls it
        ctx.ecx = s->object;
        x86_icall(ctx, g_mem, kThreadEntry);
        s->done = true;
        fiber::Switch(g_sched);
    }
}

void Release(Script* s, bool reuse) {
    if (s->f) {
        if (reuse) g_pool.push_back(s->f);
        else fiber::Destroy(s->f);  // suspended mid script: its host frames go with it
    }
    g_scripts.erase(s->thread);
    delete s;
}

// runs the script until it waits or ends; the scheduler's registers come back as they were
void Run(X86Context& ctx, Script* s) {
    X86Context saved = ctx;
    g_sched = fiber::Current();
    g_running = s;
    fiber::Switch(s->f);
    g_running = nullptr;
    ctx = saved;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_002F5660);
X86_EXTERN_FUNC(__imp__sub_002FC540);

// Scr_ResumeThread
X86_FUNC(sub_002FE100) {
    g_ctx = &ctx;
    g_mem = base;
    const uint32_t t = ctx.eax;
    const uint32_t s_esp = ctx.esp - 0x14;  // after its five pushes, where the original keeps [ebp-4]
    W32(kCurrentThread, t);
    {
        uint32_t esi = ctx.esi;
        ctx.esi = t;
        kernel::CallGuest(ctx, kUpdateThreadWaits, {});
        ctx.esi = esi;
    }
    uint32_t flags = R32(t + 0x10);
    base[R32(kIface) + 0x24] = uint8_t((flags >> 6) & 1);

    if ((flags & kFirstRun) && !(flags & kSuspended)) {
        flags &= ~(kFirstRun | kWoken);
        W32(t + 0x10, flags);
        if (!(flags & kKilled)) {
            W32(t + 0x28, s_esp - 0x2000);
            if (auto it = g_scripts.find(t); it != g_scripts.end()) Release(it->second, false);  // stale
            Script* s = new Script;
            s->thread = t;
            s->start_esp = s_esp - 0x2000;
            s->object = R32(t + 0xC);
            if (!g_pool.empty()) {
                s->f = g_pool.back();
                g_pool.pop_back();
            } else {
                s->f = fiber::Create(FiberMain, nullptr);
                if (!s->f) {
                    LOGE("script thread %08X: no host stack for it", t);
                    x86_trap(ctx, 0x2FE100, "script fiber");
                }
            }
            g_scripts[t] = s;
            Run(ctx, s);
            if (s->done || s->dead) {
                bool done = s->done;
                Release(s, done);
                if (done) W32(t + 0x10, R32(t + 0x10) | kDone);
                W32(kCurrentThread, 0);
                kernel::Return(ctx, ctx.eax, 0);
                return;
            }
        } else {
            W32(t + 0x10, flags | kDone);
            W32(kCurrentThread, 0);
            kernel::Return(ctx, ctx.eax, 0);
            return;
        }
    }

    // after a wait, as the original does once its long jump lands: a thread already woken runs on at once
    for (;;) {
        flags = R32(t + 0x10);
        if (!((flags & kWaiting) && (flags & kWoken))) break;
        W32(t + 0x10, flags & ~(kWaiting | kWoken));
        auto it = g_scripts.find(t);
        if (it == g_scripts.end()) {
            LOGE("script thread %08X woke with no suspended host stack", t);
            x86_trap(ctx, 0x2FE100, "script resume");
        }
        Script* s = it->second;
        W32(t + 0x28, s_esp - 0x2000);
        uint32_t dest = s_esp - 0x2000 - R32(t + 0x30);
        {
            uint32_t eax = ctx.eax, ecx = ctx.ecx;
            ctx.eax = t + 0x2C;
            ctx.ecx = dest;
            kernel::CallGuest(ctx, kRestoreThreadStack, {});
            ctx.eax = eax;
            ctx.ecx = ecx;
        }
        s->start_esp = dest;  // where the suspended script expects its stack
        Run(ctx, s);
        if (s->done || s->dead) {
            bool done = s->done;
            Release(s, done);
            if (done) W32(t + 0x10, R32(t + 0x10) | kDone);
            W32(kCurrentThread, 0);
            kernel::Return(ctx, ctx.eax, 0);
            return;
        }
    }
    W32(kCurrentThread, 0);
    kernel::Return(ctx, ctx.eax, 0);
}

// Scr_RestoreContext: the wait functions end here
X86_FUNC(sub_002F5660) {
    if (R32(ctx.esp + 4) == kResumeContext && g_running && fiber::Current() == g_running->f) {
        fiber::Switch(g_sched);
        kernel::Return(ctx, ctx.eax, 0);  // resumed: back to the suspend function, which restores the rest
        return;
    }
    __imp__sub_002F5660(ctx, base);
}

namespace {

// the suspend functions, as the original: the frame they push is what Scr_ResumeThread pops on resume
void Suspend(X86Context& ctx, uint32_t esp_slot, uint32_t wait_slot, std::initializer_list<uint32_t> args,
             uint32_t eax, uint32_t ecx, uint32_t edx) {
    Script* s = g_running;
    if (!s || fiber::Current() != s->f) {
        LOGE("script wait outside a script thread");
        x86_trap(ctx, R32(ctx.esp), "script wait");
    }
    const uint32_t entry = ctx.esp;
    uint32_t esp = entry - 2;  // pushfw
    kernel::W16(esp, 0x0202);
    const uint32_t regs[8] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, esp, ctx.ebp, ctx.esi, ctx.edi};
    for (uint32_t r : regs) {
        esp -= 4;
        W32(esp, r);
    }
    esp -= 4;
    W32(esp, R32(ctx.fs_base));
    W32(esp_slot, esp);
    X86Context snap = ctx;
    ctx.esp = esp - 0x80;
    kernel::CallGuest(ctx, R32(kSaveThreadLocalsSlot), {esp});
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    kernel::CallGuest(ctx, R32(wait_slot), args);  // returns once the thread is resumed
    // Scr_ResumeThread put the stack back at the same place
    if (s->start_esp != esp) {
        LOGE("script thread %08X resumed with its stack at %08X, suspended at %08X", s->thread, s->start_esp, esp);
        x86_trap(ctx, R32(entry), "script resume");
    }
    ctx = snap;
    W32(ctx.fs_base, R32(esp));
    ctx.edi = R32(esp + 4);
    ctx.esi = R32(esp + 8);
    ctx.ebp = R32(esp + 12);
    ctx.ebx = R32(esp + 20);
    ctx.edx = R32(esp + 24);
    ctx.ecx = R32(esp + 28);
    ctx.eax = R32(esp + 32);
    ctx.esp = entry;
    kernel::Return(ctx, ctx.eax, 0);
}

}  // namespace

// Scr_SuspendWaitTime
X86_FUNC(sub_0011D950) {
    (void)base;
    uint32_t t = R32(0x922124);
    Suspend(ctx, 0x922ED4, 0x92217C, {t}, ctx.eax, t, ctx.edx);
}

// Scr_SuspendWaitFrames
X86_FUNC(sub_0011D990) {
    (void)base;
    uint32_t n = R32(0x922128);
    Suspend(ctx, 0x922ED8, 0x922180, {n}, ctx.eax, n, ctx.edx);
}

// Scr_SuspendWaitTill
X86_FUNC(sub_0011D9D0) {
    (void)base;
    uint32_t a0 = R32(0x922134), a1 = R32(0x922E6C), a2 = R32(0x922130), a3 = R32(0x922120), a4 = R32(0x92212C),
             a5 = R32(0x922E60), a6 = base[0x922E5C], a7 = R32(0x922138);
    Suspend(ctx, 0x922EDC, 0x922184, {a0, a1, a2, a3, a4, a5, a6, a7}, a2, a1, a0);
}

// Scr_DestroyThreadContents: a thread killed while it waits leaves its host stack behind
X86_FUNC(sub_002FC540) {
    if (auto it = g_scripts.find(ctx.edi); it != g_scripts.end()) {
        Script* s = it->second;
        if (s == g_running) s->dead = true;  // itself: the scheduler drops it when it switches back
        else Release(s, false);
    }
    __imp__sub_002FC540(ctx, base);
}
