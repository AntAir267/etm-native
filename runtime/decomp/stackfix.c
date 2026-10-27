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

void stackfix_install(void)
{
    hook_sf_64a2e9 = sf_64a2e9;
}
