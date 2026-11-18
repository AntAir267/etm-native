/* Enter the Matrix's side of the toolkit's Direct3D 9 bridge (runtime/d3d9 hooks): what was in
   etm-native's runtime/d3d9.c and is the game's (a port file: etm-native would keep this). */
#include <strings.h>
#include "d3d9_bridge.h"
#include "options.h"

void *vk_create9(void);                  /* vk_d3d9.c: our own Vulkan renderer's IDirect3D9 */
void vk_buf_host(void *buf, void *host); /* vk_d3d9.c: a buffer's host copy is the program's */
void ui_viewport(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);                   /* game.c */
int ui2d_layout(uint32_t caller, float minx, float maxx, float miny, float maxy, float *scale, float *offset,
                float *yscale, float *yoffset);                                          /* game.c */
float draw_distance_scale(void);                                                         /* decomp/frame.c */

/* option renderer = dxvk (the default: DXVK, until Ant has played ours: DESIGN.md 11) | vulkan (our
   own, VULKAN.md); ours falls back to DXVK if it cannot start */
void *port_d3d9_renderer(uint32_t sdk_version)
{
    static int vulkan = -1;
    (void)sdk_version;
    if (vulkan < 0)
        vulkan = !strcasecmp(option_str("renderer", "dxvk"), "vulkan");
    if (!vulkan)
        return NULL;
    void *d = vk_create9();
    if (!d) {
        LOG(LOG_ALWAYS, "renderer = vulkan: our renderer could not start; DXVK instead");
        vulkan = 0;
    }
    return d;
}

void port_d3d9_buffer_host(void *buffer, void *host_copy) { vk_buf_host(buffer, host_copy); }

void port_d3d9_viewport(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) { ui_viewport(x, y, w, h); }

int port_d3d9_ui2d_layout(uint32_t caller, float minx, float maxx, float miny, float maxy, float *xscale,
                          float *xoffset, float *yscale, float *yoffset)
{
    return ui2d_layout(caller, minx, maxx, miny, maxy, xscale, xoffset, yscale, yoffset);
}

/* option draw_distance (decomp/frame.c): the fog's distances scaled with the far plane */
uint32_t port_d3d9_render_state(uint32_t state, uint32_t value)
{
    if (state == D3DRS_FOGSTART || state == D3DRS_FOGEND) {
        float f, scale = draw_distance_scale();
        if (scale != 1.0f) {
            memcpy(&f, &value, 4);
            f *= scale;
            memcpy(&value, &f, 4);
        }
    }
    return value;
}
