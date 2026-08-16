/*
 * QEMU SDL display driver
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"

#ifdef __LIMBO__
/* Injected at VM start by vm-executor-jni.c (set_qemu_var). */
extern int limbo_sdl_scale_mode;

/* Re-apply the Limbo display scale mode's logical size to the SDL renderer.
 * Called from sdl2_2d_switch() (guest resolution change) and from sdl2.c on
 * SDL_WINDOWEVENT_RESIZED (device orientation / screen size change), because
 * SDL only refreshes the logical-size viewport on SDL_WINDOWEVENT_SIZE_CHANGED,
 * which the Android backend never delivers.
 *
 *   aspect (1, default): logical size == guest resolution, SDL scales it to
 *     the window preserving the aspect ratio (letterbox) and maps mouse events
 *     into guest coordinates automatically;
 *   stretch (0) / 1:1 (2): logical size == renderer output, the guest
 *     framebuffer is stretched or blitted 1:1 manually in sdl2_2d_update(),
 *     mouse coordinates are converted in sdl2.c (sdl2_map_to_guest). */
void sdl2_2d_update_logical_size(struct sdl2_console *scon)
{
    int mode = limbo_sdl_scale_mode;
    int ow = 0, oh = 0;

    if (!scon->real_renderer) {
        return;
    }
    if (mode < 0) {
        mode = 1;
    }
    if (mode != 1 &&
        SDL_GetRendererOutputSize(scon->real_renderer, &ow, &oh) == 0 &&
        ow > 0 && oh > 0) {
        SDL_RenderSetLogicalSize(scon->real_renderer, ow, oh);
    } else {
        SDL_RenderSetLogicalSize(scon->real_renderer,
                                 surface_width(scon->surface),
                                 surface_height(scon->surface));
    }
}
#endif

void sdl2_2d_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *surf = scon->surface;
    SDL_Rect rect;
    size_t surface_data_offset;
    assert(!scon->opengl);

    if (!scon->texture) {
        return;
    }

    surface_data_offset = surface_bytes_per_pixel(surf) * x +
                          surface_stride(surf) * y;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    SDL_UpdateTexture(scon->texture, &rect,
                      surface_data(surf) + surface_data_offset,
                      surface_stride(surf));
    SDL_RenderClear(scon->real_renderer);
#ifdef __LIMBO__
    /* 1:1 pixels: blit the guest framebuffer at its native size, centered
     * in the window (the surrounding area stays black). */
    if (limbo_sdl_scale_mode == 2) {
        SDL_Rect dst;
        int ow = 0, oh = 0;
        if (SDL_GetRendererOutputSize(scon->real_renderer, &ow, &oh) == 0 &&
            ow > 0 && oh > 0) {
            dst.w = surface_width(surf);
            dst.h = surface_height(surf);
            dst.x = (ow - dst.w) / 2;
            dst.y = (oh - dst.h) / 2;
            SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, &dst);
            SDL_RenderPresent(scon->real_renderer);
            return;
        }
    }
#endif
    /* The logical size set in sdl2_2d_switch() makes SDL scale the guest
     * framebuffer to the window while preserving its aspect ratio
     * (letterboxing), so a NULL destination rect is scaled correctly. */
    SDL_RenderCopy(scon->real_renderer, scon->texture, NULL, NULL);
    SDL_RenderPresent(scon->real_renderer);
}

void sdl2_2d_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *old_surface = scon->surface;
    int format = 0;

    assert(!scon->opengl);

    scon->surface = new_surface;

    if (scon->texture) {
        SDL_DestroyTexture(scon->texture);
        scon->texture = NULL;
    }

    if (surface_is_placeholder(new_surface) && qemu_console_get_index(dcl->con)) {
        sdl2_window_destroy(scon);
        return;
    }

    if (!scon->real_window) {
        sdl2_window_create(scon);
    } else if (old_surface &&
               ((surface_width(old_surface)  != surface_width(new_surface)) ||
                (surface_height(old_surface) != surface_height(new_surface)))) {
        sdl2_window_resize(scon);
    }

#ifdef __ANDROID__
    /* The SDL window size on Android is driven by the Java surface, not by
     * the guest framebuffer.  Keep it in sync with the real renderer output
     * so the letterboxing math and the input mapping always use the surface
     * size (SDL_SetWindowSize only updates the logical window size here, the
     * actual Android surface is unaffected). */
    {
        int w = 0, h = 0;
        if (SDL_GetRendererOutputSize(scon->real_renderer, &w, &h) == 0 &&
            w > 0 && h > 0) {
            SDL_SetWindowSize(scon->real_window, w, h);
        }
    }
#endif

#ifdef __LIMBO__
    /* Limbo display scale modes (see sdl2_2d_update_logical_size()). */
    sdl2_2d_update_logical_size(scon);
#else
    /* Let SDL scale the guest framebuffer to the window while preserving its
     * aspect ratio (letterboxing) and convert mouse events into logical
     * (guest) coordinates.  SDL provides this on the compat layer, keeping
     * the rendering and the input mapping consistent on every renderer. */
    SDL_RenderSetLogicalSize(scon->real_renderer,
                             surface_width(new_surface),
                             surface_height(new_surface));
#endif

    switch (surface_format(scon->surface)) {
    case PIXMAN_x1r5g5b5:
        format = SDL_PIXELFORMAT_ARGB1555;
        break;
    case PIXMAN_r5g6b5:
        format = SDL_PIXELFORMAT_RGB565;
        break;
    case PIXMAN_a8r8g8b8:
    case PIXMAN_x8r8g8b8:
        format = SDL_PIXELFORMAT_ARGB8888;
        break;
    case PIXMAN_a8b8g8r8:
    case PIXMAN_x8b8g8r8:
        format = SDL_PIXELFORMAT_ABGR8888;
        break;
    case PIXMAN_r8g8b8a8:
    case PIXMAN_r8g8b8x8:
        format = SDL_PIXELFORMAT_RGBA8888;
        break;
    case PIXMAN_b8g8r8x8:
        format = SDL_PIXELFORMAT_BGRX8888;
        break;
    case PIXMAN_b8g8r8a8:
        format = SDL_PIXELFORMAT_BGRA8888;
        break;
    default:
        g_assert_not_reached();
    }
    scon->texture = SDL_CreateTexture(scon->real_renderer, format,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      surface_width(new_surface),
                                      surface_height(new_surface));
    sdl2_2d_redraw(scon);

#ifdef __LIMBO__
    //TODO: Need to send the resolution to Limbo
    Android_JNI_SetVMResolution(surface_width(new_surface), surface_height(new_surface));
#endif //__ANDROID__

}

void sdl2_2d_refresh(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(!scon->opengl);
    graphic_hw_update(dcl->con);
    sdl2_poll_events(scon);
}

void sdl2_2d_redraw(struct sdl2_console *scon)
{
    assert(!scon->opengl);

    if (!scon->surface) {
        return;
    }
    sdl2_2d_update(&scon->dcl, 0, 0,
                   surface_width(scon->surface),
                   surface_height(scon->surface));
}

bool sdl2_2d_check_format(DisplayChangeListener *dcl,
                          pixman_format_code_t format)
{
    /*
     * We let SDL convert for us a few more formats than,
     * the native ones. These are the ones I have tested.
     */
    return (format == PIXMAN_x8r8g8b8 ||
            format == PIXMAN_a8r8g8b8 ||
            format == PIXMAN_a8b8g8r8 ||
            format == PIXMAN_x8b8g8r8 ||
            format == PIXMAN_b8g8r8x8 ||
            format == PIXMAN_b8g8r8a8 ||
            format == PIXMAN_r8g8b8x8 ||
            format == PIXMAN_r8g8b8a8 ||
            format == PIXMAN_x1r5g5b5 ||
            format == PIXMAN_r5g6b5);
}
