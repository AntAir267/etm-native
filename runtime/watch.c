/* WATCHPOINT (debugging): ETM_WATCH=addr[@frame] logs every write to the 4 bytes at addr (hex)
   from that frame on: the page is write-protected; a write faults, the handler logs the host
   backtrace (translated functions are F_<address>), lets that one instruction run (trap flag)
   and protects the page again. For finding who writes a value the A/B check sees differ. */
#define _GNU_SOURCE
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "runtime.h"

static uint32_t watch_addr;
static unsigned long watch_frame;
static int armed, stepping;
static void *watch_page;

static void protect(int on)
{
    mprotect(watch_page, 4096, on ? PROT_READ : PROT_READ | PROT_WRITE);
}

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uintptr_t a = (uintptr_t)si->si_addr;
    if (!armed || (a & ~4095ul) != (uintptr_t)watch_page) {
        signal(sig, SIG_DFL);
        return;
    }
    if (a >= watch_addr && a < watch_addr + 4u) {
        char head[96];
        int n = snprintf(head, sizeof head, "watch: frame %lu write to %08x (was %08x):\n", frame_count,
                         (uint32_t)a, R32(watch_addr));
        if (write(2, head, n) < 0) {}
        void *bt[16];
        int k = backtrace(bt, 16);
        backtrace_symbols_fd(bt, k, 2);
    }
    protect(0);
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;            /* one instruction, then SIGTRAP */
    stepping = 1;
}

static void on_trap(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    (void)si;
    if (!stepping) {
        signal(sig, SIG_DFL);
        return;
    }
    stepping = 0;
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    static uint32_t last;
    static int have;
    if (!have) { last = R32(watch_addr); have = 1; }
    if (R32(watch_addr) != last) {             /* (block copies start below the address) */
        char line[96];
        int n = snprintf(line, sizeof line, "watch: frame %lu %08x -> %08x by:\n", frame_count, last, R32(watch_addr));
        if (write(2, line, n) < 0) {}
        void *bt[16];
        int k = backtrace(bt, 16);
        backtrace_symbols_fd(bt, k, 2);
        last = R32(watch_addr);
    }
    protect(1);
}

void watch_frame_tick(void)
{
    static int init = -1;
    if (init < 0) {
        init = 0;
        const char *e = getenv("ETM_WATCH");
        if (!e) return;
        watch_addr = (uint32_t)strtoul(e, NULL, 16);
        const char *at = strchr(e, '@');
        watch_frame = at ? strtoul(at + 1, NULL, 10) : 0;
        watch_page = (void *)(uintptr_t)(watch_addr & ~4095u);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_flags = SA_SIGINFO;
        sa.sa_sigaction = on_segv;
        sigaction(SIGSEGV, &sa, NULL);
        sa.sa_sigaction = on_trap;
        sigaction(SIGTRAP, &sa, NULL);
        init = 1;
    }
    if (init == 1 && !armed && frame_count >= watch_frame) {
        armed = 1;
        protect(1);
        LOG(LOG_ALWAYS, "watch: %08x from frame %lu", watch_addr, frame_count);
    }
}
