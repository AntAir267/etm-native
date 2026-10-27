/* The switch for the decompiled functions (decomp.h). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "decomp.h"
#include "options.h"

/* option decomp = on | off; ETM_DECOMP_OFF=addr,addr,... (hex) or all */
int decomp_enabled(uint32_t fn)
{
    static int all = -1;
    if (all < 0)
        all = strcasecmp(option_str("decomp", "on"), "off") != 0;
    if (!all) return 0;
    const char *p = getenv("ETM_DECOMP_OFF");
    if (p && !strcasecmp(p, "all")) return 0;
    while (p && *p) {
        char *q;
        if (strtoul(p, &q, 16) == fn) return 0;
        if (q == p) break;
        while (*q == ',' || *q == ' ') q++;
        p = q;
    }
    return 1;
}

extern const DecompEntry __start_decomp_table[] __attribute__((weak)), __stop_decomp_table[] __attribute__((weak));

void decomp_install(void)
{
    int n = 0;
    for (const DecompEntry *d = __start_decomp_table; d < __stop_decomp_table; d++)   /* DECOMP */
        if (decomp_enabled(d->addr)) {
            *d->hook = d->fn;
            n++;
        }
    if (n)
        LOG(LOG_ALWAYS, "decomp: %d rewritten functions in", n);
}
