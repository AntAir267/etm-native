/* STACK LEFTOVERS. Some of the game's functions copy part of their stack frame into game memory
   without writing it first, so the copy holds whatever an earlier call left at that depth (old
   return addresses). The rewrites (runtime/decomp/) use the stack differently, so those bytes
   differ between the translated and the rewritten runs though nothing behaves differently. Each
   such function's frame is cleared on entry in both modes (these hooks are always in, not
   switched by ETM_DECOMP_OFF), so the copies hold zeros and the A/B check (tests/ab.sh) compares
   exactly. Each entry says what leaks and how it was found. */
#include <string.h>
#include "decomp.h"

/* clear the frame a function with a standard prologue (push ebp; mov ebp, esp; sub esp, size)
   is about to use: [entry - 4 - size, entry - 4) */
static void clear_frame(CPU *c, uint32_t size)
{
    memset(gptr(c->esp - 4 - size), 0, size);
}

/* 0x64a2e9 (from 0x649f80, the last update's render setup): builds four 0x54-byte records in its
   0x1d0 frame and hands them to 0x6a9413, which copies them whole into the table at 0x9914a0
   (0x60 apart); the second record's +0x40..+0x47 are never written: 0x991540 held an old return
   address [verified: A/B memory diff at frame 4010 of the lobby route, ETM_WATCH backtrace
   0x6a9413 <- 0x64a2e9 <- 0x649f80, disassembly] */
static int sf_64a2e9(CPU *c) { clear_frame(c, 0x1d0); return 0; }

/* 0x8bbd20, the SSE-support test of the CPU detection (0x8bbd80, the Intel compiler's dispatch:
   run on the first call of a CPU-specific function, 0x7b1808 on the lobby route): signal(SIGILL,
   0x8bbd00), setjmp(0xe80780) (0x8adfdc), movaps. The jmp_buf keeps the registers and the stack
   position of that first call (ebp at +0, esp at +0x10), which differ when the callers above are
   rewritten; it is dead once the test has returned, so its register part (+0..+0x18: ebp, ebx,
   edi, esi, esp, return address, SEH frame) is cleared after it in both modes [verified: A/B
   memory diff at frame 2290 of the lobby route, 0xe80780 and 0xe80790 holding stack addresses;
   ETM_WATCH backtrace 0x8adfdc <- 0x8bbd20 <- 0x8bbd80 <- 0x7b1808 <- 0x7b1838; disassembly] */
#define SSE_TEST_JMPBUF  0xe80780u
static int sf_8bbd20(CPU *c)
{
    F_008bbd20_orig(c);
    memset(gptr(SSE_TEST_JMPBUF), 0, 0x1c);
    return 1;
}

void stackfix_install(void)
{
    hook_sf_64a2e9 = sf_64a2e9;
    hook_sf_8bbd20 = sf_8bbd20;
}
