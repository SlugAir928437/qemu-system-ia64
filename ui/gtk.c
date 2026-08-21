/*
 * GTK UI (GTK4 port)
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Portions from gtk-vnc (originally licensed under the LGPL v2+):
 *
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
 */

#define GETTEXT_PACKAGE "qemu"
#define LOCALEDIR "po"

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu-main.h"

#include "ui/console.h"
#include "ui/gtk.h"
#ifdef G_OS_WIN32
#include <gdk/gdkwin32.h>
#endif
#ifdef GDK_WINDOWING_ANDROID
#define __GDKANDROID_H_INSIDE__
#include <gdk/android/gdkandroiddisplay.h>
#undef __GDKANDROID_H_INSIDE__
#endif
#include "ui/win32-kbd-hook.h"

#include <glib/gi18n.h>
#include <locale.h>
#if defined(CONFIG_VTE)
#include <vte/vte.h>
#endif
#include <math.h>

#include "trace.h"
#include "ui/input.h"
#include "system/runstate.h"
#include "system/system.h"
#include "keymaps.h"
#include "chardev/char.h"
#include "qom/object.h"

#define VC_WINDOW_X_MIN  320
#define VC_WINDOW_Y_MIN  240
#define VC_TERM_X_MIN     80
#define VC_TERM_Y_MIN     25
#define VC_SCALE_MIN    0.25
#define VC_SCALE_MAX       4
#define VC_SCALE_STEP   0.25

#ifdef GDK_WINDOWING_X11
#include "x_keymap.h"
#endif

#if !defined(CONFIG_VTE)
# define VTE_CHECK_VERSION(a, b, c) 0
#endif

#define HOTKEY_MODIFIERS        (GDK_CONTROL_MASK | GDK_ALT_MASK)

static const guint16 *keycode_map;
static size_t keycode_maplen;
static bool gd_use_keyval;

struct VCChardev {
    Chardev parent;
    VirtualConsole *console;
    bool echo;
};
typedef struct VCChardev VCChardev;

#define TYPE_CHARDEV_VC "chardev-vc"
DECLARE_INSTANCE_CHECKER(VCChardev, VC_CHARDEV,
                         TYPE_CHARDEV_VC)

static struct touch_slot touch_slots[INPUT_EVENT_SLOTS_MAX];

bool gtk_use_gl_area;

static void gd_grab_pointer(VirtualConsole *vc, const char *reason);
static void gd_ungrab_pointer(GtkDisplayState *s);
static void gd_grab_keyboard(VirtualConsole *vc, const char *reason);
static void gd_ungrab_keyboard(GtkDisplayState *s);

static void gd_update_cursor(VirtualConsole *vc);
static void gd_menu_show_tabs(GtkDisplayState *s);
static void gd_menu_show_menubar(GtkDisplayState *s);
static void gd_menu_full_screen(GtkDisplayState *s);
static void gd_menu_zoom_in(GtkDisplayState *s);
static void gd_menu_zoom_out(GtkDisplayState *s);
static void gd_menu_zoom_fixed(GtkDisplayState *s);
static void gd_menu_zoom_fit(GtkDisplayState *s);
static void gd_menu_grab_input(GtkDisplayState *s);
static void gd_menu_switch_vc(GtkDisplayState *s, int idx);
static void gd_menu_untabify(GtkDisplayState *s);
static void gd_button_press(VirtualConsole *vc, double x, double y, bool down);
static void gd_connect_window_hotkeys(GtkWidget *window,
                                      GtkDisplayState *s,
                                      VirtualConsole *vc);

/** Utility Functions **/

static VirtualConsole *gd_vc_find_current(GtkDisplayState *s)
{
    gint page;

    page = gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook));
    if (page < 0 || page >= s->nb_vcs) {
        page = 0;
    }
    return &s->vc[page];
}

static bool gd_is_grab_active(GtkDisplayState *s)
{
    return s->grab_active;
}

static bool gd_grab_on_hover(GtkDisplayState *s)
{
    return s->grab_on_hover;
}

static GdkSurface *gd_widget_surface(GtkWidget *widget)
{
    GtkNative *native = gtk_widget_get_native(widget);

    return native ? gtk_native_get_surface(native) : NULL;
}

static void gd_update_cursor(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    GdkSurface *surface;

    if (vc->type != GD_VC_GFX ||
        !qemu_console_is_graphic(vc->gfx.dcl.con)) {
        return;
    }

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    surface = gd_widget_surface(vc->gfx.drawing_area);
    if (!surface) {
        return;
    }
    if (s->full_screen || qemu_input_is_absolute(vc->gfx.dcl.con) || s->ptr_owner == vc) {
        gdk_surface_set_cursor(surface, s->null_cursor);
    } else {
        gdk_surface_set_cursor(surface, NULL);
    }
}

static void gd_update_caption(GtkDisplayState *s)
{
    const char *status = "";
    gchar *prefix;
    gchar *title;
    const char *grab = "";
    bool is_paused = !runstate_is_running();
    int i;

    if (qemu_name) {
        prefix = g_strdup_printf("QEMU (%s)", qemu_name);
    } else {
        prefix = g_strdup_printf("QEMU");
    }

    if (s->ptr_owner != NULL &&
        s->ptr_owner->window == NULL) {
        grab = _(" - Press Ctrl+Alt+G to release grab");
    }

    if (is_paused) {
        status = _(" [Paused]");
    }
    s->external_pause_update = true;
    g_simple_action_set_state(G_SIMPLE_ACTION(g_action_map_lookup_action(
                                   G_ACTION_MAP(s->actions), "pause")),
                              g_variant_new_boolean(is_paused));
    s->external_pause_update = false;

    title = g_strdup_printf("%s%s%s", prefix, status, grab);
    gtk_window_set_title(GTK_WINDOW(s->window), title);
    g_free(title);

    for (i = 0; i < s->nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];

        if (!vc->window) {
            continue;
        }
        title = g_strdup_printf("%s: %s%s%s", prefix, vc->label,
                                vc == s->kbd_owner ? " +kbd" : "",
                                vc == s->ptr_owner ? " +ptr" : "");
        gtk_window_set_title(GTK_WINDOW(vc->window), title);
        g_free(title);
    }

    g_free(prefix);
}

static void gd_update_geometry_hints(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;

    if (vc->type == GD_VC_GFX) {
        if (!vc->gfx.ds) {
            return;
        }
        double scale_x = s->free_scale ? VC_SCALE_MIN : vc->gfx.scale_x;
        double scale_y = s->free_scale ? VC_SCALE_MIN : vc->gfx.scale_y;
        int min_width  = surface_width(vc->gfx.ds) * scale_x;
        int min_height = surface_height(vc->gfx.ds) * scale_y;

        gtk_widget_set_size_request(vc->gfx.drawing_area, min_width, min_height);

#if defined(CONFIG_VTE)
    } else if (vc->type == GD_VC_VTE) {
        VteTerminal *term = VTE_TERMINAL(vc->vte.terminal);
        int cw = vte_terminal_get_char_width(term);
        int ch = vte_terminal_get_char_height(term);

        gtk_widget_set_size_request(vc->vte.terminal,
                                    cw * VC_TERM_X_MIN, ch * VC_TERM_Y_MIN);
#endif
    }
}

void gd_update_windowsize(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;

    gd_update_geometry_hints(vc);

    if (vc->type == GD_VC_GFX && !s->full_screen && !s->free_scale) {
        gtk_window_set_default_size(
            GTK_WINDOW(vc->window ? vc->window : s->window),
            VC_WINDOW_X_MIN, VC_WINDOW_Y_MIN);
    }
}

static void gd_update_full_redraw(VirtualConsole *vc)
{
    GtkWidget *area = vc->gfx.drawing_area;

#if defined(CONFIG_OPENGL)
    if (vc->gfx.gls && gtk_use_gl_area) {
        gtk_gl_area_queue_render(GTK_GL_AREA(vc->gfx.drawing_area));
        return;
    }
#endif
    gtk_widget_queue_draw(area);
}

static void gtk_release_modifiers(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (vc->type != GD_VC_GFX ||
        !qemu_console_is_graphic(vc->gfx.dcl.con)) {
        return;
    }
    qkbd_state_lift_all_keys(vc->gfx.kbd);
}

static void *gd_win32_get_hwnd(VirtualConsole *vc)
{
#ifdef G_OS_WIN32
    GdkSurface *surface = gd_widget_surface(vc->window ? vc->window : vc->s->window);
    if (surface) {
        return gdk_win32_surface_get_handle(surface);
    }
#endif
    return NULL;
}

/** DisplayState Callbacks **/

static void gd_update(DisplayChangeListener *dcl,
                      int fbx, int fby, int fbw, int fbh)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    int wx_offset, wy_offset;
    int ww_surface, wh_surface;
    int ww_widget, wh_widget;

    trace_gd_update(vc->label, fbx, fby, fbw, fbh);

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    if (vc->gfx.convert) {
        pixman_image_composite(PIXMAN_OP_SRC, vc->gfx.ds->image,
                               NULL, vc->gfx.convert,
                               fbx, fby, 0, 0, fbx, fby, fbw, fbh);
    }

    ww_surface = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    wh_surface = surface_height(vc->gfx.ds) * vc->gfx.scale_y;

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }
    ww_widget = gtk_widget_get_width(vc->gfx.drawing_area);
    wh_widget = gtk_widget_get_height(vc->gfx.drawing_area);

    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    gtk_widget_queue_draw(vc->gfx.drawing_area);
}

static void gd_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static void gd_mouse_set(DisplayChangeListener *dcl,
                         int x, int y, bool visible)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (!gtk_widget_get_realized(vc->gfx.drawing_area) ||
        qemu_input_is_absolute(dcl->con)) {
        return;
    }

    /*
     * GTK4 removed gdk_device_warp() so we can not warp the host pointer
     * anymore.  Relative-mode pointer emulation therefore degrades to
     * delta based tracking only.
     */
    vc->s->last_x = x;
    vc->s->last_y = y;
}

static void gd_cursor_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkTexture *texture;
    GdkCursor *cursor;
    GdkSurface *surface;

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    GBytes *bytes = g_bytes_new(c->data, c->width * c->height * 4);

    texture = gdk_memory_texture_new(c->width, c->height,
                                     GDK_MEMORY_R8G8B8A8,
                                     bytes, c->width * 4);
    g_bytes_unref(bytes);
    cursor = gdk_cursor_new_from_texture(texture, c->hot_x, c->hot_y, NULL);
    surface = gd_widget_surface(vc->gfx.drawing_area);
    if (surface) {
        gdk_surface_set_cursor(surface, cursor);
    }
    g_object_unref(texture);
    g_object_unref(cursor);
}

static void gd_switch(DisplayChangeListener *dcl,
                      DisplaySurface *surface)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.surface) {
        cairo_surface_destroy(vc->gfx.surface);
        vc->gfx.surface = NULL;
    }
    if (vc->gfx.convert) {
        pixman_image_unref(vc->gfx.convert);
        vc->gfx.convert = NULL;
    }

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }
    vc->gfx.ds = surface;

    if (surface_format(surface) == PIXMAN_x8r8g8b8) {
        /*
         * PIXMAN_x8r8g8b8 == CAIRO_FORMAT_RGB24
         *
         * No need to convert, use surface directly.  Should be the
         * common case as this is qemu_default_pixelformat(32) too.
         */
        vc->gfx.surface = cairo_image_surface_create_for_data
            (surface_data(surface),
             CAIRO_FORMAT_RGB24,
             surface_width(surface),
             surface_height(surface),
             surface_stride(surface));
    } else {
        /* Must convert surface, use pixman to do it. */
        vc->gfx.convert = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                                   surface_width(surface),
                                                   surface_height(surface),
                                                   NULL, 0);
        vc->gfx.surface = cairo_image_surface_create_for_data
            ((void *)pixman_image_get_data(vc->gfx.convert),
             CAIRO_FORMAT_RGB24,
             pixman_image_get_width(vc->gfx.convert),
             pixman_image_get_height(vc->gfx.convert),
             pixman_image_get_stride(vc->gfx.convert));
        pixman_image_composite(PIXMAN_OP_SRC, vc->gfx.ds->image,
                               NULL, vc->gfx.convert,
                               0, 0, 0, 0, 0, 0,
                               pixman_image_get_width(vc->gfx.convert),
                               pixman_image_get_height(vc->gfx.convert));
    }

    if (resized) {
        gd_update_windowsize(vc);
    } else {
        gd_update_full_redraw(vc);
    }
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name             = "gtk",
    .dpy_gfx_update       = gd_update,
    .dpy_gfx_switch       = gd_switch,
    .dpy_gfx_check_format = qemu_pixman_check_format,
    .dpy_refresh          = gd_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,
};


#if defined(CONFIG_OPENGL)

static bool gd_has_dmabuf(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (gtk_use_gl_area && !gtk_widget_get_realized(vc->gfx.drawing_area)) {
        /* FIXME: Assume it will work, actual check done after realize */
        /* fixing this would require delaying listener registration */
        return true;
    }

    return vc->gfx.has_dmabuf;
}

static void gd_gl_release_dmabuf(DisplayChangeListener *dcl,
                                 QemuDmaBuf *dmabuf)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    egl_dmabuf_release_texture(dmabuf);
    if (vc->gfx.guest_fb.dmabuf == dmabuf) {
        vc->gfx.guest_fb.dmabuf = NULL;
    }
#endif
}

void gd_hw_gl_flushed(void *vcon)
{
    VirtualConsole *vc = vcon;
    QemuDmaBuf *dmabuf = vc->gfx.guest_fb.dmabuf;
    int fence_fd;

    fence_fd = qemu_dmabuf_get_fence_fd(dmabuf);
    if (fence_fd >= 0) {
        qemu_set_fd_handler(fence_fd, NULL, NULL, NULL);
        close(fence_fd);
        qemu_dmabuf_set_fence_fd(dmabuf, -1);
        graphic_hw_gl_block(vc->gfx.dcl.con, false);
    }
}

/** DisplayState Callbacks (opengl version) **/

static const DisplayChangeListenerOps dcl_gl_area_ops = {
    .dpy_name             = "gtk-egl",
    .dpy_gfx_update       = gd_gl_area_update,
    .dpy_gfx_switch       = gd_gl_area_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_refresh          = gd_gl_area_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,

    .dpy_gl_scanout_texture  = gd_gl_area_scanout_texture,
    .dpy_gl_scanout_disable  = gd_gl_area_scanout_disable,
    .dpy_gl_update           = gd_gl_area_scanout_flush,
    .dpy_gl_scanout_dmabuf   = gd_gl_area_scanout_dmabuf,
    .dpy_gl_release_dmabuf   = gd_gl_release_dmabuf,
    .dpy_has_dmabuf          = gd_has_dmabuf,
};

static bool
gd_gl_area_is_compatible_dcl(DisplayGLCtx *dgc,
                             DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_gl_area_ops;
}

static const DisplayGLCtxOps gl_area_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = gd_gl_area_is_compatible_dcl,
    .dpy_gl_ctx_create       = gd_gl_area_create_context,
    .dpy_gl_ctx_destroy      = gd_gl_area_destroy_context,
    .dpy_gl_ctx_make_current = gd_gl_area_make_current,
};

#ifdef CONFIG_X11
static const DisplayChangeListenerOps dcl_egl_ops = {
    .dpy_name             = "gtk-egl",
    .dpy_gfx_update       = gd_egl_update,
    .dpy_gfx_switch       = gd_egl_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_refresh          = gd_egl_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,

    .dpy_gl_scanout_disable  = gd_egl_scanout_disable,
    .dpy_gl_scanout_texture  = gd_egl_scanout_texture,
    .dpy_gl_scanout_dmabuf   = gd_egl_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = gd_egl_cursor_dmabuf,
    .dpy_gl_cursor_position  = gd_egl_cursor_position,
    .dpy_gl_update           = gd_egl_flush,
    .dpy_gl_release_dmabuf   = gd_gl_release_dmabuf,
    .dpy_has_dmabuf          = gd_has_dmabuf,
};

static bool
gd_egl_is_compatible_dcl(DisplayGLCtx *dgc,
                         DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_egl_ops;
}

static const DisplayGLCtxOps egl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = gd_egl_is_compatible_dcl,
    .dpy_gl_ctx_create       = gd_egl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = gd_egl_make_current,
};
#endif

#endif /* CONFIG_OPENGL */

/** QEMU Events **/

static void gd_change_runstate(void *opaque, bool running, RunState state)
{
    GtkDisplayState *s = opaque;

    gd_update_caption(s);
}

static void gd_mouse_mode_change(Notifier *notify, void *data)
{
    GtkDisplayState *s;
    int i;

    s = container_of(notify, GtkDisplayState, mouse_mode_notifier);
    /* release the grab at switching to absolute mode */
    if (s->ptr_owner && qemu_input_is_absolute(s->ptr_owner->gfx.dcl.con)) {
        if (!s->ptr_owner->window) {
            s->grab_active = false;
            g_simple_action_set_state(G_SIMPLE_ACTION(
                g_action_map_lookup_action(G_ACTION_MAP(s->actions), "grab")),
                g_variant_new_boolean(false));
        } else {
            gd_ungrab_pointer(s);
        }
    }
    for (i = 0; i < s->nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];
        gd_update_cursor(vc);
    }
}

/** GTK Events **/

static gboolean gd_window_close(GtkWidget *widget, void *opaque)
{
    GtkDisplayState *s = opaque;
    bool allow_close = true;

    if (s->opts->has_window_close && !s->opts->window_close) {
        allow_close = false;
    }

    if (allow_close) {
        qmp_quit(NULL);
    }

    return TRUE;
}

static void gd_set_ui_refresh_rate(VirtualConsole *vc, int refresh_rate)
{
    QemuUIInfo info;

    if (!dpy_ui_info_supported(vc->gfx.dcl.con)) {
        return;
    }

    info = *dpy_get_ui_info(vc->gfx.dcl.con);
    info.refresh_rate = refresh_rate;
    dpy_set_ui_info(vc->gfx.dcl.con, &info, true);
}

static void gd_set_ui_size(VirtualConsole *vc, gint width, gint height)
{
    QemuUIInfo info;

    if (!dpy_ui_info_supported(vc->gfx.dcl.con)) {
        return;
    }

    info = *dpy_get_ui_info(vc->gfx.dcl.con);
    info.width = width;
    info.height = height;
    dpy_set_ui_info(vc->gfx.dcl.con, &info, true);
}

#if defined(CONFIG_OPENGL)

static gboolean gd_render_event(GtkGLArea *area, GdkGLContext *context,
                                void *opaque)
{
    VirtualConsole *vc = opaque;

    if (vc->gfx.gls) {
        gd_gl_area_draw(vc);
    }
    return TRUE;
}

static void gd_resize_event(GtkGLArea *area,
                            gint width, gint height, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    double pw = width, ph = height;
    double sx = vc->gfx.scale_x, sy = vc->gfx.scale_y;
    const int gs = gtk_widget_get_scale_factor(GTK_WIDGET(area));

    if (!vc->s->free_scale && !vc->s->full_screen) {
        pw /= sx;
        ph /= sy;
    }

    /**
     * width and height here are in pixel coordinate, so we must divide it
     * by global window scale (gs)
     */
    gd_set_ui_size(vc, pw / gs, ph / gs);
}

#endif

void gd_update_monitor_refresh_rate(VirtualConsole *vc, GtkWidget *widget)
{
    GdkSurface *surface = gd_widget_surface(widget);
    int refresh_rate;

    if (surface) {
        GdkDisplay *dpy = gtk_widget_get_display(widget);
        GdkMonitor *monitor = gdk_display_get_monitor_at_surface(dpy, surface);
        refresh_rate = gdk_monitor_get_refresh_rate(monitor); /* [mHz] */
    } else {
        refresh_rate = 0;
    }

    gd_set_ui_refresh_rate(vc, refresh_rate);

    /* T = 1 / f = 1 [s*Hz] / f = 1000*1000 [ms*mHz] / f */
    vc->gfx.dcl.update_interval = refresh_rate ?
        MIN(1000 * 1000 / refresh_rate, GUI_REFRESH_INTERVAL_DEFAULT) :
        GUI_REFRESH_INTERVAL_DEFAULT;
}

#ifdef __LIMBO__
/*
 * Display scale mode selected from the app UI (Limbo):
 *   0 = stretch to fill the screen (ignore aspect ratio)
 *   1 = keep the guest aspect ratio (letterbox, default)
 *   2 = 1:1 pixel mapping (native guest resolution, centered)
 * Injected at VM start by vm-executor-jni.c via set_qemu_var(), so this
 * global must stay a dlsym-visible non-static symbol.
 */
int limbo_gtk_scale_mode = -1;
#endif

void gd_update_scale(VirtualConsole *vc, int ww, int wh, int fbw, int fbh)
{
    double sx, sy;

    if (!vc) {
        return;
    }

    sx = (double)ww / fbw;
    sy = (double)wh / fbh;
#ifdef __LIMBO__
    /* On Android the window always covers the whole screen, so the scale is
     * always recomputed from the widget size and the requested mode. */
    switch (limbo_gtk_scale_mode) {
    case 0: /* stretch: fill the whole screen, break the aspect ratio */
        vc->gfx.scale_x = sx;
        vc->gfx.scale_y = sy;
        break;
    case 2: /* 1:1: keep the native guest pixels, centered */
        vc->gfx.scale_x = 1.0;
        vc->gfx.scale_y = 1.0;
        break;
    case 1: /* aspect (default): uniform scale with letterboxing */
    default:
        vc->gfx.scale_x = vc->gfx.scale_y = MIN(sx, sy);
        break;
    }
#else
    if (vc->s->full_screen || vc->s->free_scale) {
        if (vc->s->keep_aspect_ratio) {
            /* Uniform scale: preserve the guest aspect ratio (letterbox). */
            vc->gfx.scale_x = vc->gfx.scale_y = MIN(sx, sy);
        } else {
            vc->gfx.scale_x = sx;
            vc->gfx.scale_y = sy;
        }
    }
#endif
}

static void gd_draw_func(GtkDrawingArea *area, cairo_t *cr,
                         int width, int height, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    int wx_offset, wy_offset;
    int ww_widget, wh_widget, ww_surface, wh_surface;
    int fbw, fbh;

#if defined(CONFIG_OPENGL)
    if (vc->gfx.gls) {
        if (gtk_use_gl_area) {
            /* GtkGLArea render callback handles this */
            return;
        } else {
#ifdef CONFIG_X11
            gd_egl_draw(vc);
            return;
#else
            abort();
#endif
        }
    }
#endif

    if (!gtk_widget_get_realized(GTK_WIDGET(area))) {
        return;
    }
    if (!vc->gfx.ds) {
        return;
    }
    if (!vc->gfx.surface) {
        return;
    }

    gd_update_monitor_refresh_rate(vc, vc->window ? vc->window : s->window);

    fbw = surface_width(vc->gfx.ds);
    fbh = surface_height(vc->gfx.ds);

    ww_widget = width;
    wh_widget = height;

    gd_update_scale(vc, ww_widget, wh_widget, fbw, fbh);

    ww_surface = fbw * vc->gfx.scale_x;
    wh_surface = fbh * vc->gfx.scale_y;

    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    /* report the guest-visible size back to the guest (if it cares) */
    if (!s->free_scale && !s->full_screen && vc->gfx.scale_x > 0) {
        gd_set_ui_size(vc, ww_widget / vc->gfx.scale_x,
                       wh_widget / vc->gfx.scale_y);
    }

    cairo_rectangle(cr, 0, 0, ww_widget, wh_widget);

    /* Optionally cut out the inner area where the pixmap
       will be drawn. This avoids 'flashing' since we're
       not double-buffering. Note we're using the undocumented
       behaviour of drawing the rectangle from right to left
       to cut out the whole */
    cairo_rectangle(cr, wx_offset + ww_surface, wy_offset,
                    -1 * ww_surface, wh_surface);
    cairo_fill(cr);

    cairo_scale(cr, vc->gfx.scale_x, vc->gfx.scale_y);
    cairo_set_source_surface(cr, vc->gfx.surface,
                             wx_offset / vc->gfx.scale_x,
                             wy_offset / vc->gfx.scale_y);
    cairo_paint(cr);
}

static void gd_motion_event(GtkEventControllerMotion *controller,
                            gdouble x, gdouble y, void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    int fbx, fby;
    int wx_offset, wy_offset;
    int wh_surface, ww_surface;
    int ww_widget, wh_widget;
    GtkWidget *widget = vc->gfx.drawing_area;

    if (!vc->gfx.ds) {
        return;
    }

    /* active touch drags are routed to the touch handling code */
    if (vc->gfx.touch_active) {
        return;
    }

    ww_surface = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    wh_surface = surface_height(vc->gfx.ds) * vc->gfx.scale_y;
    ww_widget = gtk_widget_get_width(widget);
    wh_widget = gtk_widget_get_height(widget);

    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    fbx = (x - wx_offset) / vc->gfx.scale_x;
    fby = (y - wy_offset) / vc->gfx.scale_y;

    trace_gd_motion_event(ww_widget, wh_widget,
                          gtk_widget_get_scale_factor(widget), fbx, fby);

    if (qemu_input_is_absolute(vc->gfx.dcl.con)) {
        if (fbx < 0 || fby < 0 ||
            fbx >= surface_width(vc->gfx.ds) ||
            fby >= surface_height(vc->gfx.ds)) {
            return;
        }
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_X, fbx,
                             0, surface_width(vc->gfx.ds));
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_Y, fby,
                             0, surface_height(vc->gfx.ds));
        qemu_input_event_sync();
    } else if (s->last_set && s->ptr_owner == vc) {
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_X, fbx - s->last_x);
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_Y, fby - s->last_y);
        qemu_input_event_sync();
    }
    s->last_x = fbx;
    s->last_y = fby;
    s->last_set = TRUE;
}

static gboolean gd_touch_handle(VirtualConsole *vc, int type,
                                double x, double y)
{
    Error *err = NULL;
    GtkWidget *widget = vc->gfx.drawing_area;
    double wx_offset, wy_offset;
    double ww_surface, wh_surface;
    double fbx, fby;
    int ww_widget, wh_widget;

    if (!vc->gfx.ds || vc->gfx.scale_x <= 0 || vc->gfx.scale_y <= 0) {
        return TRUE;
    }

    /* Map the widget (GTK logical) coordinates back into the guest
     * framebuffer, taking the letterbox offset and the uniform scale into
     * account (same math as gd_motion_event / gd_draw_func). */
    ww_surface = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    wh_surface = surface_height(vc->gfx.ds) * vc->gfx.scale_y;
    ww_widget = gtk_widget_get_width(widget);
    wh_widget = gtk_widget_get_height(widget);
    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }
    fbx = (x - wx_offset) / vc->gfx.scale_x;
    fby = (y - wy_offset) / vc->gfx.scale_y;

    console_handle_touch_event(vc->gfx.dcl.con, touch_slots,
                               0, surface_width(vc->gfx.ds),
                               surface_height(vc->gfx.ds), fbx,
                               fby, type, &err);
    if (err) {
        warn_report_err(err);
    }
    return TRUE;
}

static void gd_click_pressed(GtkGestureClick *gesture,
                             gint n_press, gdouble x, gdouble y,
                             gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    GdkDevice *device;
    GdkInputSource source;

    if (n_press > 1) {
        /* ignore double/triple clicks */
        return;
    }

    device = gtk_gesture_get_device(GTK_GESTURE(gesture));
    source = device ? gdk_device_get_source(device) : GDK_SOURCE_MOUSE;

    if (source == GDK_SOURCE_TOUCHSCREEN) {
        vc->gfx.touch_active = true;
        gd_touch_handle(vc, INPUT_MULTI_TOUCH_TYPE_BEGIN, x, y);
        return;
    }

    /* implicitly grab the input at the first click in the relative mode */
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == 1 &&
        !qemu_input_is_absolute(vc->gfx.dcl.con) && s->ptr_owner != vc) {
        if (!vc->window) {
            s->grab_active = true;
            g_simple_action_set_state(G_SIMPLE_ACTION(
                g_action_map_lookup_action(G_ACTION_MAP(s->actions), "grab")),
                g_variant_new_boolean(true));
        } else {
            gd_grab_pointer(vc, "relative-mode-click");
        }
        return;
    }

    gd_button_press(vc, x, y, true);
}

static void gd_click_released(GtkGestureClick *gesture,
                              gint n_press, gdouble x, gdouble y,
                              gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GdkDevice *device;
    GdkInputSource source;

    device = gtk_gesture_get_device(GTK_GESTURE(gesture));
    source = device ? gdk_device_get_source(device) : GDK_SOURCE_MOUSE;

    if (source == GDK_SOURCE_TOUCHSCREEN) {
        vc->gfx.touch_active = false;
        gd_touch_handle(vc, INPUT_MULTI_TOUCH_TYPE_END, x, y);
        return;
    }

    if (n_press > 1) {
        return;
    }
    gd_button_press(vc, x, y, false);
}

static void gd_button_press(VirtualConsole *vc, double x, double y, bool down)
{
    GtkDisplayState *s = vc->s;
    GtkWidget *widget = vc->gfx.drawing_area;
    InputButton btn = 0;
    int fbx, fby;
    int wx_offset, wy_offset;
    int wh_surface, ww_surface;
    int ww_widget, wh_widget;

    if (!vc->gfx.ds) {
        return;
    }

    ww_surface = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    wh_surface = surface_height(vc->gfx.ds) * vc->gfx.scale_y;
    ww_widget = gtk_widget_get_width(widget);
    wh_widget = gtk_widget_get_height(widget);

    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    fbx = (x - wx_offset) / vc->gfx.scale_x;
    fby = (y - wy_offset) / vc->gfx.scale_y;

    s->last_x = fbx;
    s->last_y = fby;
    s->last_set = TRUE;

    if (qemu_input_is_absolute(vc->gfx.dcl.con)) {
        if (fbx < 0 || fby < 0 ||
            fbx >= surface_width(vc->gfx.ds) ||
            fby >= surface_height(vc->gfx.ds)) {
            return;
        }
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_X, fbx,
                             0, surface_width(vc->gfx.ds));
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_Y, fby,
                             0, surface_height(vc->gfx.ds));
    }

    if (s->ptr_owner == vc) {
        qemu_input_queue_btn(vc->gfx.dcl.con, INPUT_BUTTON_LEFT, down);
    } else if (qemu_input_is_absolute(vc->gfx.dcl.con)) {
        qemu_input_queue_btn(vc->gfx.dcl.con, INPUT_BUTTON_LEFT, down);
    }
    qemu_input_event_sync();
}

static void gd_scroll_event(GtkEventControllerScroll *controller,
                            gdouble dx, gdouble dy, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    InputButton btn_vertical;
    InputButton btn_horizontal;
    bool has_vertical = false;
    bool has_horizontal = false;

    if (dy < 0) {
        btn_vertical = INPUT_BUTTON_WHEEL_UP;
        has_vertical = true;
    } else if (dy > 0) {
        btn_vertical = INPUT_BUTTON_WHEEL_DOWN;
        has_vertical = true;
    } else if (dx < 0) {
        btn_horizontal = INPUT_BUTTON_WHEEL_LEFT;
        has_horizontal = true;
    } else if (dx > 0) {
        btn_horizontal = INPUT_BUTTON_WHEEL_RIGHT;
        has_horizontal = true;
    } else {
        return;
    }

    if (has_vertical) {
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_vertical, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_vertical, false);
        qemu_input_event_sync();
    }

    if (has_horizontal) {
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_horizontal, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_horizontal, false);
        qemu_input_event_sync();
    }
}

static const guint16 *gd_get_keymap(size_t *maplen)
{
    GdkDisplay *dpy = gdk_display_get_default();

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("x11");
        return qemu_xkeymap_mapping_table(
            gdk_x11_display_get_xdisplay(dpy), maplen);
    }
#endif

#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("wayland");
        *maplen = qemu_input_map_xorgevdev_to_qcode_len;
        return qemu_input_map_xorgevdev_to_qcode;
    }
#endif

#ifdef GDK_WINDOWING_WIN32
    if (GDK_IS_WIN32_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("win32");
        *maplen = qemu_input_map_atset1_to_qcode_len;
        return qemu_input_map_atset1_to_qcode;
    }
#endif

#ifdef GDK_WINDOWING_QUARTZ
    if (GDK_IS_QUARTZ_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("quartz");
        *maplen = qemu_input_map_osx_to_qcode_len;
        return qemu_input_map_osx_to_qcode;
    }
#endif

#ifdef GDK_WINDOWING_BROADWAY
    if (GDK_IS_BROADWAY_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("broadway");
        g_warning("experimental: using broadway, x11 virtual keysym\n"
                  "mapping - with very limited support. See also\n"
                  "https://bugzilla.gnome.org/show_bug.cgi?id=700105");
        *maplen = qemu_input_map_x11_to_qcode_len;
        return qemu_input_map_x11_to_qcode;
    }
#endif

#ifdef GDK_WINDOWING_ANDROID
    if (GDK_IS_ANDROID_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("android");
        gd_use_keyval = true;
        *maplen = qemu_input_map_x11_to_qcode_len;
        return qemu_input_map_x11_to_qcode;
    }
#endif

    g_warning("Unsupported GDK Windowing platform.\n"
              "Disabling extended keycode tables.\n"
              "Please report to qemu-devel@nongnu.org\n"
              "including the following information:\n"
              "\n"
              "  - Operating system\n"
              "  - GDK Windowing system build\n");
    return NULL;
}


static int gd_map_keycode(int scancode)
{
    if (!keycode_map) {
        return 0;
    }
    if (scancode > keycode_maplen) {
        return 0;
    }

    return keycode_map[scancode];
}

static gboolean gd_text_key_down(GtkEventControllerKey *controller,
                                 guint keyval, guint keycode,
                                 GdkModifierType state, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    QemuTextConsole *con = QEMU_TEXT_CONSOLE(vc->gfx.dcl.con);

    if (keyval == GDK_KEY_Delete) {
        qemu_text_console_put_qcode(con, Q_KEY_CODE_DELETE, false);
    } else {
        gunichar uc = gdk_keyval_to_unicode(keyval);
        if (uc) {
            char buf[8];
            gint len = g_unichar_to_utf8(uc, buf);
            qemu_text_console_put_string(con, buf, len);
        } else {
            int qcode = gd_map_keycode(gd_use_keyval ? keyval : keycode);
            qemu_text_console_put_qcode(con, qcode, false);
        }
    }
    return TRUE;
}

static gboolean gd_key_event(GtkEventControllerKey *controller,
                             guint keyval, guint keycode,
                             GdkModifierType state, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    int qcode;

#ifdef G_OS_WIN32
    /* on windows, we ought to ignore the reserved key event? */
    if (keycode == 0xff) {
        return false;
    }

    if (!vc->s->kbd_owner) {
        if (keycode == VK_LWIN || keycode == VK_RWIN) {
            return FALSE;
        }
    }
#endif

    if (keyval == GDK_KEY_Pause
#ifdef G_OS_WIN32
        || keycode == VK_PAUSE
#endif
        ) {
        qkbd_state_key_event(vc->gfx.kbd, Q_KEY_CODE_PAUSE, true);
        return TRUE;
    }

    qcode = gd_map_keycode(gd_use_keyval ? keyval : keycode);
    trace_gd_key_event(vc->label, keycode, qcode, "down");

    qkbd_state_key_event(vc->gfx.kbd, qcode, true);

    return TRUE;
}

static gboolean gd_key_release_event(GtkEventControllerKey *controller,
                                     guint keyval, guint keycode,
                                     GdkModifierType state, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    int qcode;

    if (keyval == GDK_KEY_Pause) {
        qkbd_state_key_event(vc->gfx.kbd, Q_KEY_CODE_PAUSE, false);
        return TRUE;
    }

    qcode = gd_map_keycode(gd_use_keyval ? keyval : keycode);
    qkbd_state_key_event(vc->gfx.kbd, qcode, false);

    return TRUE;
}

/** Window Menu Actions **/

static void gd_menu_pause(GSimpleAction *action, GVariant *parameter,
                          void *opaque)
{
    GtkDisplayState *s = opaque;

    if (s->external_pause_update) {
        return;
    }
    if (runstate_is_running()) {
        qmp_stop(NULL);
    } else {
        qmp_cont(NULL);
    }
}

static void gd_menu_reset(GSimpleAction *action, GVariant *parameter,
                          void *opaque)
{
    qmp_system_reset(NULL);
}

static void gd_menu_powerdown(GSimpleAction *action, GVariant *parameter,
                              void *opaque)
{
    qmp_system_powerdown(NULL);
}

static void gd_menu_quit(GSimpleAction *action, GVariant *parameter,
                         void *opaque)
{
    qmp_quit(NULL);
}

static void gd_menu_switch_vc(GtkDisplayState *s, int idx)
{
    VirtualConsole *vc;
    GtkNotebook *nb = GTK_NOTEBOOK(s->notebook);
    gint page;

    gtk_release_modifiers(s);
    if (idx < 0 || idx >= s->nb_vcs) {
        return;
    }
    vc = &s->vc[idx];
    page = gtk_notebook_page_num(nb, vc->tab_item);
    if (page >= 0) {
        gtk_notebook_set_current_page(nb, page);
        gtk_widget_grab_focus(vc->focus);
    }
    s->current_vc = idx;
    g_simple_action_set_state(G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(s->actions), "switch-vc")),
        g_variant_new_int32(idx));
}

static void gd_action_switch_vc(GSimpleAction *action, GVariant *parameter,
                                void *opaque)
{
    GtkDisplayState *s = opaque;
    int idx;

    if (!parameter) {
        return;
    }
    idx = g_variant_get_int32(parameter);
    gd_menu_switch_vc(s, idx);
}

static void gd_menu_show_tabs(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), s->show_tabs);
    gd_update_windowsize(vc);
}

static void gd_action_show_tabs(GSimpleAction *action, GVariant *parameter,
                                void *opaque)
{
    GtkDisplayState *s = opaque;

    s->show_tabs = !s->show_tabs;
    gd_menu_show_tabs(s);
}

static gboolean gd_tab_window_close(GtkWidget *widget, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), vc->tab_item,
                             gtk_label_new(vc->label));
    gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(s->notebook),
                                    vc->tab_item, vc->label);
    gtk_window_destroy(GTK_WINDOW(vc->window));
    vc->window = NULL;
#if defined(CONFIG_OPENGL)
    if (vc->gfx.esurface) {
        eglDestroySurface(qemu_egl_display, vc->gfx.esurface);
        vc->gfx.esurface = NULL;
    }
    if (vc->gfx.ectx) {
        eglDestroyContext(qemu_egl_display, vc->gfx.ectx);
        vc->gfx.ectx = NULL;
    }
#endif
    return TRUE;
}

static gboolean gd_win_grab(void *opaque)
{
    VirtualConsole *vc = opaque;

    fprintf(stderr, "%s: %s\n", __func__, vc->label);
    if (vc->s->ptr_owner) {
        gd_ungrab_pointer(vc->s);
    } else {
        gd_grab_pointer(vc, "user-request-detached-tab");
    }
    return TRUE;
}

static void gd_menu_untabify(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (vc->type == GD_VC_GFX &&
        qemu_console_is_graphic(vc->gfx.dcl.con)) {
        s->grab_active = false;
    }
    if (!vc->window) {
        gint page = gtk_notebook_page_num(GTK_NOTEBOOK(s->notebook),
                                          vc->tab_item);
        vc->window = gtk_window_new();
#if defined(CONFIG_OPENGL)
        if (vc->gfx.esurface) {
            eglDestroySurface(qemu_egl_display, vc->gfx.esurface);
            vc->gfx.esurface = NULL;
        }
        if (vc->gfx.ectx) {
            eglDestroyContext(qemu_egl_display, vc->gfx.ectx);
            vc->gfx.ectx = NULL;
        }
#endif
        gtk_notebook_remove_page(GTK_NOTEBOOK(s->notebook), page);
        gtk_window_set_child(GTK_WINDOW(vc->window), vc->tab_item);

        g_signal_connect(vc->window, "close-request",
                         G_CALLBACK(gd_tab_window_close), vc);
        gtk_widget_set_visible(vc->window, true);

        if (qemu_console_is_graphic(vc->gfx.dcl.con)) {
            gd_connect_window_hotkeys(vc->window, s, vc);
        }

        gd_update_geometry_hints(vc);
        gd_update_caption(s);
    }
}

static void gd_menu_show_menubar(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (s->full_screen) {
        return;
    }

    gtk_widget_set_visible(s->menu_bar, s->show_menubar);
    gd_update_windowsize(vc);
}

static void gd_action_show_menubar(GSimpleAction *action, GVariant *parameter,
                                   void *opaque)
{
    GtkDisplayState *s = opaque;

    s->show_menubar = !s->show_menubar;
    gd_menu_show_menubar(s);
}

static void gd_menu_full_screen(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (!s->full_screen) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
        gtk_widget_set_visible(s->menu_bar, FALSE);
        if (vc->type == GD_VC_GFX) {
            gtk_widget_set_size_request(vc->gfx.drawing_area, -1, -1);
        }
        gtk_window_fullscreen(GTK_WINDOW(s->window));
        s->full_screen = TRUE;
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(s->window));
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), s->show_tabs);
        gtk_widget_set_visible(s->menu_bar, s->show_menubar);
        s->full_screen = FALSE;
        if (vc->type == GD_VC_GFX) {
            vc->gfx.scale_x = vc->gfx.preferred_scale;
            vc->gfx.scale_y = vc->gfx.preferred_scale;
            gd_update_windowsize(vc);
        }
    }

    gd_update_cursor(vc);
}

static void gd_action_full_screen(GSimpleAction *action, GVariant *parameter,
                                  void *opaque)
{
    gd_menu_full_screen(opaque);
}

static void gd_menu_zoom_in(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    s->zoom_fit_active = false;
    g_simple_action_set_state(G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-fit")),
        g_variant_new_boolean(false));

    vc->gfx.scale_x += VC_SCALE_STEP;
    vc->gfx.scale_y += VC_SCALE_STEP;

    gd_update_windowsize(vc);
}

static void gd_action_zoom_in(GSimpleAction *action, GVariant *parameter,
                              void *opaque)
{
    gd_menu_zoom_in(opaque);
}

static void gd_menu_zoom_out(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    s->zoom_fit_active = false;
    g_simple_action_set_state(G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-fit")),
        g_variant_new_boolean(false));

    vc->gfx.scale_x -= VC_SCALE_STEP;
    vc->gfx.scale_y -= VC_SCALE_STEP;

    vc->gfx.scale_x = MAX(vc->gfx.scale_x, VC_SCALE_MIN);
    vc->gfx.scale_y = MAX(vc->gfx.scale_y, VC_SCALE_MIN);

    gd_update_windowsize(vc);
}

static void gd_action_zoom_out(GSimpleAction *action, GVariant *parameter,
                               void *opaque)
{
    gd_menu_zoom_out(opaque);
}

static void gd_menu_zoom_fixed(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    vc->gfx.scale_x = vc->gfx.preferred_scale;
    vc->gfx.scale_y = vc->gfx.preferred_scale;

    gd_update_windowsize(vc);
}

static void gd_action_zoom_fixed(GSimpleAction *action, GVariant *parameter,
                                 void *opaque)
{
    gd_menu_zoom_fixed(opaque);
}

static void gd_menu_zoom_fit(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (s->zoom_fit_active) {
        s->free_scale = TRUE;
    } else {
        s->free_scale = FALSE;
        vc->gfx.scale_x = vc->gfx.preferred_scale;
        vc->gfx.scale_y = vc->gfx.preferred_scale;
    }

    gd_update_windowsize(vc);
    gd_update_full_redraw(vc);
}

static void gd_action_zoom_fit(GSimpleAction *action, GVariant *parameter,
                               void *opaque)
{
    GtkDisplayState *s = opaque;

    s->zoom_fit_active = !s->zoom_fit_active;
    gd_menu_zoom_fit(s);
}

static void gd_grab_update(VirtualConsole *vc, bool kbd, bool ptr)
{
    /*
     * GTK4 removed gdk_seat_grab().  Keyboard/pointer grabs are no longer
     * available; input is routed through the focused widget.
     */
}

static void gd_grab_keyboard(VirtualConsole *vc, const char *reason)
{
    if (vc->s->kbd_owner) {
        if (vc->s->kbd_owner == vc) {
            return;
        } else {
            gd_ungrab_keyboard(vc->s);
        }
    }

    win32_kbd_set_grab(true);
    gd_grab_update(vc, true, vc->s->ptr_owner == vc);
    vc->s->kbd_owner = vc;
    gd_update_caption(vc->s);
    trace_gd_grab(vc->label, "kbd", reason);
}

static void gd_ungrab_keyboard(GtkDisplayState *s)
{
    VirtualConsole *vc = s->kbd_owner;

    if (vc == NULL) {
        return;
    }
    s->kbd_owner = NULL;

    win32_kbd_set_grab(false);
    gd_grab_update(vc, false, vc->s->ptr_owner == vc);
    gd_update_caption(s);
    trace_gd_ungrab(vc->label, "kbd");
}

static void gd_grab_pointer(VirtualConsole *vc, const char *reason)
{
    if (vc->s->ptr_owner) {
        if (vc->s->ptr_owner == vc) {
            return;
        } else {
            gd_ungrab_pointer(vc->s);
        }
    }

    gd_grab_update(vc, vc->s->kbd_owner == vc, true);
    vc->s->ptr_owner = vc;
    gd_update_caption(vc->s);
    trace_gd_grab(vc->label, "ptr", reason);
}

static void gd_ungrab_pointer(GtkDisplayState *s)
{
    VirtualConsole *vc = s->ptr_owner;

    if (vc == NULL) {
        return;
    }
    s->ptr_owner = NULL;

    gd_grab_update(vc, vc->s->kbd_owner == vc, false);
    gd_update_caption(s);
    trace_gd_ungrab(vc->label, "ptr");
}

static void gd_menu_grab_input(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (gd_is_grab_active(s)) {
        gd_grab_keyboard(vc, "user-request-main-window");
        gd_grab_pointer(vc, "user-request-main-window");
    } else {
        gd_ungrab_keyboard(s);
        gd_ungrab_pointer(s);
    }

    gd_update_cursor(vc);
}

static void gd_action_grab(GSimpleAction *action, GVariant *parameter,
                           void *opaque)
{
    GtkDisplayState *s = opaque;

    s->grab_active = !s->grab_active;
    gd_menu_grab_input(s);
}

static void gd_action_grab_on_hover(GSimpleAction *action, GVariant *parameter,
                                    void *opaque)
{
    GtkDisplayState *s = opaque;

    s->grab_on_hover = !s->grab_on_hover;
}

static void gd_change_page(GtkNotebook *nb, GtkWidget *page, guint page_num,
                           gpointer data)
{
    GtkDisplayState *s = data;
    VirtualConsole *vc;
    gboolean on_vga;

    if (!gtk_widget_get_realized(s->notebook)) {
        return;
    }

    vc = &s->vc[page_num];
    s->current_vc = page_num;
    on_vga = (vc->type == GD_VC_GFX &&
              qemu_console_is_graphic(vc->gfx.dcl.con));
    if (!on_vga) {
        s->grab_active = false;
        g_simple_action_set_state(G_SIMPLE_ACTION(
            g_action_map_lookup_action(G_ACTION_MAP(s->actions), "grab")),
            g_variant_new_boolean(false));
    } else if (s->full_screen) {
        s->grab_active = true;
        g_simple_action_set_state(G_SIMPLE_ACTION(
            g_action_map_lookup_action(G_ACTION_MAP(s->actions), "grab")),
            g_variant_new_boolean(true));
    }
    g_simple_action_set_state(G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(s->actions), "switch-vc")),
        g_variant_new_int32(page_num));
#ifdef CONFIG_VTE
    g_simple_action_set_enabled(G_SIMPLE_ACTION(
        g_action_map_lookup_action(G_ACTION_MAP(s->actions), "copy")),
        vc->type == GD_VC_VTE);
#endif

    gd_update_windowsize(vc);
    gd_update_cursor(vc);
}

static void gd_enter_event(GtkEventControllerMotion *controller,
                           gdouble x, gdouble y, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    if (gd_grab_on_hover(s)) {
        gd_grab_keyboard(vc, "grab-on-hover");
    }
}

static void gd_leave_event(GtkEventControllerMotion *controller,
                           gdouble x, gdouble y, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    if (gd_grab_on_hover(s)) {
        gd_ungrab_keyboard(s);
    }
}

static void gd_focus_in_event(GtkEventControllerFocus *controller,
                              gpointer opaque)
{
    VirtualConsole *vc = opaque;

    win32_kbd_set_window(gd_win32_get_hwnd(vc));
}

static void gd_focus_out_event(GtkEventControllerFocus *controller,
                               gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    win32_kbd_set_window(NULL);
    gtk_release_modifiers(s);
}

/** Virtual Console Callbacks **/

#if defined(CONFIG_VTE)
static void gd_menu_copy(GSimpleAction *action, GVariant *parameter,
                         void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    vte_terminal_copy_clipboard_format(VTE_TERMINAL(vc->vte.terminal),
                                       VTE_FORMAT_TEXT);
}

static void gd_vc_adjustment_changed(GtkAdjustment *adjustment, void *opaque)
{
    VirtualConsole *vc = opaque;

    if (gtk_adjustment_get_upper(adjustment) >
        gtk_adjustment_get_page_size(adjustment)) {
        gtk_widget_set_visible(vc->vte.scrollbar, true);
    } else {
        gtk_widget_set_visible(vc->vte.scrollbar, false);
    }
}

static void gd_vc_send_chars(VirtualConsole *vc)
{
    uint32_t len, avail;

    len = qemu_chr_be_can_write(vc->vte.chr);
    avail = fifo8_num_used(&vc->vte.out_fifo);
    while (len > 0 && avail > 0) {
        const uint8_t *buf;
        uint32_t size;

        buf = fifo8_pop_bufptr(&vc->vte.out_fifo, MIN(len, avail), &size);
        qemu_chr_be_write(vc->vte.chr, buf, size);
        len = qemu_chr_be_can_write(vc->vte.chr);
        avail -= size;
    }
}

static int gd_vc_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    vte_terminal_feed(VTE_TERMINAL(vc->vte.terminal), (const char *)buf, len);
    return len;
}

static void gd_vc_chr_accept_input(Chardev *chr)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    if (vc) {
        gd_vc_send_chars(vc);
    }
}

static void gd_vc_chr_set_echo(Chardev *chr, bool echo)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    if (vc) {
        vc->vte.echo = echo;
    } else {
        vcd->echo = echo;
    }
}

static int nb_vcs;
static Chardev *vcs[MAX_VCS];
static void gd_vc_open(Chardev *chr,
                       ChardevBackend *backend,
                       bool *be_opened,
                       Error **errp)
{
    if (nb_vcs == MAX_VCS) {
        error_setg(errp, "Maximum number of consoles reached");
        return;
    }

    vcs[nb_vcs++] = chr;

    /* console/chardev init sometimes completes elsewhere in a 2nd
     * stage, so defer OPENED events until they are fully initialized
     */
    *be_opened = false;
}

static void char_gd_vc_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->open = gd_vc_open;
    cc->chr_write = gd_vc_chr_write;
    cc->chr_accept_input = gd_vc_chr_accept_input;
    cc->chr_set_echo = gd_vc_chr_set_echo;
}

static const TypeInfo char_gd_vc_type_info = {
    .name = TYPE_CHARDEV_VC,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(VCChardev),
    .class_init = char_gd_vc_class_init,
};

static gboolean gd_vc_in(VteTerminal *terminal, gchar *text, guint size,
                         gpointer user_data)
{
    VirtualConsole *vc = user_data;
    uint32_t free;

    if (vc->vte.echo) {
        VteTerminal *term = VTE_TERMINAL(vc->vte.terminal);
        int i;
        for (i = 0; i < size; i++) {
            uint8_t c = text[i];
            if (c >= 128 || isprint(c)) {
                /* 8-bit characters are considered printable.  */
                vte_terminal_feed(term, &text[i], 1);
            } else if (c == '\r' || c == '\n') {
                vte_terminal_feed(term, "\r\n", 2);
            } else {
                char ctrl[2] = { '^', 0};
                ctrl[1] = text[i] ^ 64;
                vte_terminal_feed(term, ctrl, 2);
            }
        }
    }

    free = fifo8_num_free(&vc->vte.out_fifo);
    fifo8_push_all(&vc->vte.out_fifo, (uint8_t *)text, MIN(free, size));
    gd_vc_send_chars(vc);

    return TRUE;
}

static GSList *gd_vc_vte_init(GtkDisplayState *s, VirtualConsole *vc,
                              Chardev *chr, int idx, GSList *group)
{
    char buffer[32];
    GtkWidget *box;
    GtkWidget *scrollbar;
    GtkAdjustment *vadjustment;
    VCChardev *vcd = VC_CHARDEV(chr);

    vc->s = s;
    vc->vte.echo = vcd->echo;
    vc->vte.chr = chr;
    fifo8_create(&vc->vte.out_fifo, 4096);
    vcd->console = vc;

    snprintf(buffer, sizeof(buffer), "vc%d", idx);
    vc->label = g_strdup(vc->vte.chr->label ? : buffer);

    vc->vte.terminal = vte_terminal_new();
    g_signal_connect(vc->vte.terminal, "commit", G_CALLBACK(gd_vc_in), vc);

    vte_terminal_set_scrollback_lines(VTE_TERMINAL(vc->vte.terminal), -1);
    vte_terminal_set_size(VTE_TERMINAL(vc->vte.terminal),
                          VC_TERM_X_MIN, VC_TERM_Y_MIN);

    vadjustment = gtk_scrollable_get_vadjustment
        (GTK_SCROLLABLE(vc->vte.terminal));

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, vadjustment);

    gtk_box_append(GTK_BOX(box), vc->vte.terminal);
    gtk_box_append(GTK_BOX(box), scrollbar);

    vc->vte.box = box;
    vc->vte.scrollbar = scrollbar;

    g_signal_connect(vadjustment, "changed",
                     G_CALLBACK(gd_vc_adjustment_changed), vc);

    vc->type = GD_VC_VTE;
    vc->tab_item = box;
    vc->focus = vc->vte.terminal;
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), vc->tab_item,
                             gtk_label_new(vc->label));

    qemu_chr_be_event(vc->vte.chr, CHR_EVENT_OPENED);

    return group;
}

static void gd_vcs_init(GtkDisplayState *s, GSList *group)
{
    int i;

    for (i = 0; i < nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[s->nb_vcs];
        group = gd_vc_vte_init(s, vc, vcs[i], s->nb_vcs, group);
        s->nb_vcs++;
    }
}
#endif /* CONFIG_VTE */

/** Event controller setup **/

static void gd_connect_vc_gfx_controllers(VirtualConsole *vc)
{
    GtkWidget *area = vc->gfx.drawing_area;
    GtkEventController *controller;

#if defined(CONFIG_OPENGL)
    if (gtk_use_gl_area) {
        /* wire up GtkGLArea events */
        g_signal_connect(area, "render",
                         G_CALLBACK(gd_render_event), vc);
        g_signal_connect(area, "resize",
                         G_CALLBACK(gd_resize_event), vc);
    } else
#endif
    {
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), gd_draw_func,
                                       vc, NULL);
    }
    if (qemu_console_is_graphic(vc->gfx.dcl.con)) {
        GtkGesture *click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
        g_signal_connect(click, "pressed", G_CALLBACK(gd_click_pressed), vc);
        g_signal_connect(click, "released", G_CALLBACK(gd_click_released), vc);
        gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));

        controller = gtk_event_controller_motion_new();
        g_signal_connect(controller, "motion", G_CALLBACK(gd_motion_event), vc);
        g_signal_connect(controller, "enter", G_CALLBACK(gd_enter_event), vc);
        g_signal_connect(controller, "leave", G_CALLBACK(gd_leave_event), vc);
        gtk_widget_add_controller(area, controller);

        controller = gtk_event_controller_scroll_new(
            GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES |
            GTK_EVENT_CONTROLLER_SCROLL_KINETIC);
        g_signal_connect(controller, "scroll", G_CALLBACK(gd_scroll_event), vc);
        gtk_widget_add_controller(area, controller);

        controller = gtk_event_controller_key_new();
        g_signal_connect(controller, "key-pressed",
                         G_CALLBACK(gd_key_event), vc);
        g_signal_connect(controller, "key-released",
                         G_CALLBACK(gd_key_release_event), vc);
        gtk_widget_add_controller(area, controller);

        controller = gtk_event_controller_focus_new();
        g_signal_connect(controller, "enter",
                         G_CALLBACK(gd_focus_in_event), vc);
        g_signal_connect(controller, "leave",
                         G_CALLBACK(gd_focus_out_event), vc);
        gtk_widget_add_controller(area, controller);
    } else {
        controller = gtk_event_controller_key_new();
        g_signal_connect(controller, "key-pressed",
                         G_CALLBACK(gd_text_key_down), vc);
        gtk_widget_add_controller(area, controller);
    }
}

/** Window hotkeys (Ctrl+Alt+...) **/

static gboolean gd_hotkey_press(GtkEventControllerKey *controller,
                                guint keyval, guint keycode,
                                GdkModifierType state, gpointer opaque)
{
    GtkDisplayState *s = opaque;

    if ((state & HOTKEY_MODIFIERS) != HOTKEY_MODIFIERS) {
        return FALSE;
    }

    switch (keyval) {
    case GDK_KEY_g:
        s->grab_active = !s->grab_active;
        gd_menu_grab_input(s);
        return TRUE;
    case GDK_KEY_f:
        gd_menu_full_screen(s);
        return TRUE;
    case GDK_KEY_m:
        s->show_menubar = !s->show_menubar;
        gd_menu_show_menubar(s);
        return TRUE;
    case GDK_KEY_plus:
    case GDK_KEY_equal:
        gd_menu_zoom_in(s);
        return TRUE;
    case GDK_KEY_minus:
        gd_menu_zoom_out(s);
        return TRUE;
    case GDK_KEY_0:
        gd_menu_zoom_fixed(s);
        return TRUE;
    case GDK_KEY_q:
        qmp_quit(NULL);
        return TRUE;
    default:
        break;
    }

    /* Ctrl+Alt+1..N: switch vc */
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
        int idx = keyval - GDK_KEY_1;
        if (idx < s->nb_vcs) {
            gd_menu_switch_vc(s, idx);
        }
        return TRUE;
    }

    return FALSE;
}

static void gd_connect_window_hotkeys(GtkWidget *window,
                                      GtkDisplayState *s,
                                      VirtualConsole *vc)
{
    GtkEventController *controller = gtk_event_controller_key_new();
    g_object_set_data(G_OBJECT(controller), "gtk-vc", vc);
    g_signal_connect(controller, "key-pressed",
                     G_CALLBACK(gd_hotkey_press), s);
    gtk_widget_add_controller(window, controller);
}

/** Menu construction (GMenu + GActionGroup) **/

static GSimpleAction *gd_add_action(GtkDisplayState *s, const char *name,
                                    GCallback activate, bool stateful,
                                    bool initial_state)
{
    GSimpleAction *action;

    if (stateful) {
        action = g_simple_action_new_stateful(name, NULL,
                                              g_variant_new_boolean(initial_state));
    } else {
        action = g_simple_action_new(name, NULL);
    }
    if (activate) {
        g_signal_connect(action, "activate", activate, s);
    }
    g_action_map_add_action(G_ACTION_MAP(s->actions), G_ACTION(action));
    return action;
}

static void gd_add_vc_actions(GtkDisplayState *s)
{
    GSimpleAction *action = g_simple_action_new_stateful(
        "switch-vc", G_VARIANT_TYPE_INT32, g_variant_new_int32(0));
    g_signal_connect(action, "activate", G_CALLBACK(gd_action_switch_vc), s);
    g_action_map_add_action(G_ACTION_MAP(s->actions), G_ACTION(action));
}

static GMenu *gd_create_menu_machine(GtkDisplayState *s)
{
    GMenu *machine_menu = g_menu_new();

    gd_add_action(s, "pause", G_CALLBACK(gd_menu_pause), true, false);
    g_menu_append(machine_menu, _("_Pause"), "gtk.pause");
    g_menu_append(machine_menu, NULL, NULL);

    gd_add_action(s, "reset", G_CALLBACK(gd_menu_reset), false, false);
    g_menu_append(machine_menu, _("_Reset"), "gtk.reset");

    gd_add_action(s, "powerdown", G_CALLBACK(gd_menu_powerdown), false, false);
    g_menu_append(machine_menu, _("Power _Down"), "gtk.powerdown");

    g_menu_append(machine_menu, NULL, NULL);

    gd_add_action(s, "quit", G_CALLBACK(gd_menu_quit), false, false);
    g_menu_append(machine_menu, _("_Quit"), "gtk.quit");

    return machine_menu;
}

#if defined(CONFIG_OPENGL)
static void gl_area_realize(GtkGLArea *area, VirtualConsole *vc)
{
    gtk_gl_area_make_current(area);
    qemu_egl_display = eglGetCurrentDisplay();
    vc->gfx.has_dmabuf = qemu_egl_has_dmabuf();
    if (!vc->gfx.has_dmabuf) {
        error_report("GtkGLArea console lacks DMABUF support.");
    }
}
#endif

static bool gd_scale_valid(double scale)
{
    return scale >= VC_SCALE_MIN && scale <= VC_SCALE_MAX;
}

static void gd_vc_gfx_init(GtkDisplayState *s, VirtualConsole *vc,
                           QemuConsole *con, int idx)
{
    bool zoom_to_fit = false;
    int i;

    vc->label = qemu_console_get_label(con);
    vc->s = s;
    vc->gfx.preferred_scale = 1.0;
    if (s->opts->u.gtk.has_scale) {
        if (gd_scale_valid(s->opts->u.gtk.scale)) {
            vc->gfx.preferred_scale = s->opts->u.gtk.scale;
        } else {
            error_report("Invalid scale value %lf given, being ignored",
                         s->opts->u.gtk.scale);
            s->opts->u.gtk.has_scale = false;
        }
    }
    vc->gfx.scale_x = vc->gfx.preferred_scale;
    vc->gfx.scale_y = vc->gfx.preferred_scale;

#if defined(CONFIG_OPENGL)
    if (display_opengl) {
        if (gtk_use_gl_area) {
            vc->gfx.drawing_area = gtk_gl_area_new();
            g_signal_connect(vc->gfx.drawing_area, "realize",
                             G_CALLBACK(gl_area_realize), vc);
            vc->gfx.dcl.ops = &dcl_gl_area_ops;
            vc->gfx.dgc.ops = &gl_area_ctx_ops;
        } else {
#ifdef CONFIG_X11
            vc->gfx.drawing_area = gtk_drawing_area_new();
            vc->gfx.dcl.ops = &dcl_egl_ops;
            vc->gfx.dgc.ops = &egl_ctx_ops;
            vc->gfx.has_dmabuf = qemu_egl_has_dmabuf();
#else
            abort();
#endif
        }
    } else
#endif
    {
        vc->gfx.drawing_area = gtk_drawing_area_new();
        vc->gfx.dcl.ops = &dcl_ops;
    }

    gtk_widget_set_can_focus(vc->gfx.drawing_area, TRUE);

    vc->type = GD_VC_GFX;
    vc->tab_item = vc->gfx.drawing_area;
    vc->focus = vc->gfx.drawing_area;
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook),
                             vc->tab_item, gtk_label_new(vc->label));

    vc->gfx.kbd = qkbd_state_init(con);
    vc->gfx.dcl.con = con;

    if (display_opengl) {
        qemu_console_set_display_gl_ctx(con, &vc->gfx.dgc);
    }
    register_displaychangelistener(&vc->gfx.dcl);

    gd_connect_vc_gfx_controllers(vc);

    if (dpy_ui_info_supported(vc->gfx.dcl.con)) {
        zoom_to_fit = true;
    }
    if (s->opts->u.gtk.has_zoom_to_fit) {
        zoom_to_fit = s->opts->u.gtk.zoom_to_fit;
    }
    if (zoom_to_fit) {
        s->zoom_fit_active = true;
        s->free_scale = true;
        g_simple_action_set_state(G_SIMPLE_ACTION(
            g_action_map_lookup_action(G_ACTION_MAP(s->actions), "zoom-fit")),
            g_variant_new_boolean(true));
    }

    s->keep_aspect_ratio = true;
    if (s->opts->u.gtk.has_keep_aspect_ratio)
        s->keep_aspect_ratio = s->opts->u.gtk.keep_aspect_ratio;

#ifdef __LIMBO__
    /* Limbo (Android): the GTK window always covers the whole screen, so
     * force proportional zoom-to-fit scaling regardless of the graphics
     * device's ui_info support. */
    s->free_scale = true;
    s->keep_aspect_ratio = true;
    s->zoom_fit_active = true;
#endif

    for (i = 0; i < INPUT_EVENT_SLOTS_MAX; i++) {
        struct touch_slot *slot = &touch_slots[i];
        slot->tracking_id = -1;
    }
}

static void gd_create_menu_view(GtkDisplayState *s, DisplayOptions *opts)
{
    GMenu *view_menu = g_menu_new();
    QemuConsole *con;
    int vc;

    gd_add_action(s, "fullscreen", G_CALLBACK(gd_action_full_screen), false, false);
    g_menu_append(view_menu, _("_Fullscreen"), "gtk.fullscreen");

#if defined(CONFIG_VTE)
    gd_add_action(s, "copy", G_CALLBACK(gd_menu_copy), false, false);
    g_menu_append(view_menu, _("_Copy"), "gtk.copy");
#endif

    g_menu_append(view_menu, NULL, NULL);

    gd_add_action(s, "zoom-in", G_CALLBACK(gd_action_zoom_in), false, false);
    g_menu_append(view_menu, _("Zoom _In"), "gtk.zoom-in");

    gd_add_action(s, "zoom-out", G_CALLBACK(gd_action_zoom_out), false, false);
    g_menu_append(view_menu, _("Zoom _Out"), "gtk.zoom-out");

    gd_add_action(s, "zoom-fixed", G_CALLBACK(gd_action_zoom_fixed), false, false);
    g_menu_append(view_menu, _("Best _Fit"), "gtk.zoom-fixed");

    gd_add_action(s, "zoom-fit", G_CALLBACK(gd_action_zoom_fit), true, false);
    g_menu_append(view_menu, _("Zoom To _Fit"), "gtk.zoom-fit");

    g_menu_append(view_menu, NULL, NULL);

    gd_add_action(s, "grab-on-hover", G_CALLBACK(gd_action_grab_on_hover),
                  true, false);
    g_menu_append(view_menu, _("Grab On _Hover"), "gtk.grab-on-hover");

    gd_add_action(s, "grab", G_CALLBACK(gd_action_grab), true, false);
    g_menu_append(view_menu, _("_Grab Input"), "gtk.grab");

    g_menu_append(view_menu, NULL, NULL);

    /* gfx vcs */
    for (vc = 0;; vc++) {
        con = qemu_console_lookup_by_index(vc);
        if (!con) {
            break;
        }
        gd_vc_gfx_init(s, &s->vc[vc], con, vc);
        s->nb_vcs++;
    }

#if defined(CONFIG_VTE)
    /* vte */
    gd_vcs_init(s, NULL);
#endif

    gd_add_vc_actions(s);
    if (s->nb_vcs > 1) {
        GMenu *vc_section = g_menu_new();
        int i;

        for (i = 0; i < s->nb_vcs; i++) {
            GMenuItem *item = g_menu_item_new(s->vc[i].label, "gtk.switch-vc");
            g_menu_item_set_attribute_value(item, "target",
                                            g_variant_new_int32(i));
            g_menu_append_item(vc_section, item);
            g_object_unref(item);
        }
        g_menu_append_section(view_menu, NULL, G_MENU_MODEL(vc_section));
        g_object_unref(vc_section);
    }

    g_menu_append(view_menu, NULL, NULL);

    s->show_tabs = false;
    gd_add_action(s, "show-tabs", G_CALLBACK(gd_action_show_tabs), true, false);
    g_menu_append(view_menu, _("Show _Tabs"), "gtk.show-tabs");

    gd_add_action(s, "untabify", G_CALLBACK(gd_menu_untabify), false, false);
    g_menu_append(view_menu, _("Detach Tab"), "gtk.untabify");

    s->show_menubar = !opts->u.gtk.has_show_menubar ||
                      opts->u.gtk.show_menubar;
    gd_add_action(s, "show-menubar", G_CALLBACK(gd_action_show_menubar),
                  true, s->show_menubar);
    g_menu_append(view_menu, _("Show Menubar"), "gtk.show-menubar");

    s->view_menu = view_menu;
}

static void gd_create_menus(GtkDisplayState *s, DisplayOptions *opts)
{
    GMenu *menu_bar;
    GMenu *machine_menu;

    s->actions = G_ACTION_GROUP(g_simple_action_group_new());

    machine_menu = gd_create_menu_machine(s);
    gd_create_menu_view(s, opts);

    menu_bar = g_menu_new();
    g_menu_append_submenu(menu_bar, _("_Machine"), G_MENU_MODEL(machine_menu));
    g_menu_append_submenu(menu_bar, _("_View"), G_MENU_MODEL(s->view_menu));

    s->menu_bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menu_bar));
    g_object_unref(menu_bar);
    g_object_unref(machine_menu);
    g_object_unref(s->view_menu);
}


static gboolean gtkinit;

static void gtk_display_init(DisplayState *ds, DisplayOptions *opts)
{
    GtkDisplayState *s;
    GtkIconTheme *theme;
    char *dir;
    int idx;

    if (!gtkinit) {
        fprintf(stderr, "gtk initialization failed\n");
        exit(1);
    }
    assert(opts->type == DISPLAY_TYPE_GTK);
    s = g_malloc0(sizeof(*s));
    s->opts = opts;

    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    dir = get_relocated_path(CONFIG_QEMU_ICONDIR);
    gtk_icon_theme_add_search_path(theme, dir);
    g_free(dir);
    g_set_prgname("qemu");

    s->window = gtk_window_new();
    s->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    s->notebook = gtk_notebook_new();

    s->free_scale = FALSE;

    /* Mostly LC_MESSAGES only. See early_gtk_display_init() for details. For
     * LC_CTYPE, we need to make sure that non-ASCII characters are considered
     * printable, but without changing any of the character classes to make
     * sure that we don't accidentally break implicit assumptions.  */
    setlocale(LC_MESSAGES, "");
    setlocale(LC_CTYPE, "C.UTF-8");
    dir = get_relocated_path(CONFIG_QEMU_LOCALEDIR);
    bindtextdomain("qemu", dir);
    g_free(dir);
    bind_textdomain_codeset("qemu", "UTF-8");
    textdomain("qemu");

    if (s->opts->has_show_cursor && s->opts->show_cursor) {
        s->null_cursor = NULL; /* default pointer */
    } else {
        s->null_cursor = gdk_cursor_new_from_name("none", NULL);
    }

    s->mouse_mode_notifier.notify = gd_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&s->mouse_mode_notifier);
    qemu_add_vm_change_state_handler(gd_change_runstate, s);

    gd_create_menus(s, opts);

    g_signal_connect(s->window, "close-request",
                     G_CALLBACK(gd_window_close), s);
    g_signal_connect(s->notebook, "switch-page",
                     G_CALLBACK(gd_change_page), s);

    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s->notebook), FALSE);

    gd_update_caption(s);

    gtk_widget_insert_action_group(s->window, "gtk", s->actions);
    gtk_box_append(GTK_BOX(s->vbox), s->menu_bar);
    gtk_box_append(GTK_BOX(s->vbox), s->notebook);

    gtk_window_set_child(GTK_WINDOW(s->window), s->vbox);

    gd_connect_window_hotkeys(s->window, s, NULL);

    gtk_widget_set_visible(s->window, true);

    for (idx = 0;; idx++) {
        QemuConsole *con = qemu_console_lookup_by_index(idx);
        if (!con) {
            break;
        }
        gtk_widget_realize(s->vc[idx].gfx.drawing_area);
    }

    if (!s->show_menubar) {
        gtk_widget_set_visible(s->menu_bar, false);
    }

    if (opts->has_full_screen &&
        opts->full_screen) {
        gd_menu_full_screen(s);
    }
    if (opts->u.gtk.has_grab_on_hover &&
        opts->u.gtk.grab_on_hover) {
        s->grab_on_hover = true;
    }
    if (opts->u.gtk.has_show_tabs &&
        opts->u.gtk.show_tabs) {
        s->show_tabs = true;
        gd_menu_show_tabs(s);
    }
#ifdef CONFIG_GTK_CLIPBOARD
    gd_clipboard_init(s);
#endif /* CONFIG_GTK_CLIPBOARD */

    /* GTK's event polling must happen on the main thread. */
    qemu_main = NULL;
}

static void early_gtk_display_init(DisplayOptions *opts)
{
    /*
     * GTK4 no longer calls setlocale() on its own, so there is nothing to
     * disable.  The QEMU code relies on the C locale; LC_MESSAGES is
     * imported later in gtk_display_init().
     */
    gtk_init();
    gtkinit = true;

    assert(opts->type == DISPLAY_TYPE_GTK);
    if (opts->has_gl && opts->gl != DISPLAY_GL_MODE_OFF) {
#if defined(CONFIG_OPENGL)
#if defined(GDK_WINDOWING_WAYLAND)
        if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
            gtk_use_gl_area = true;
            gtk_gl_area_init();
        } else
#endif
#if defined(GDK_WINDOWING_WIN32)
        if (GDK_IS_WIN32_DISPLAY(gdk_display_get_default())) {
            gtk_use_gl_area = true;
            gtk_gl_area_init();
        } else
#endif
        {
#ifdef CONFIG_X11
            DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAY_GL_MODE_ON;
            gtk_egl_init(mode);
#endif
        }
#endif
    }

    keycode_map = gd_get_keymap(&keycode_maplen);

#if defined(CONFIG_VTE)
    type_register_static(&char_gd_vc_type_info);
#endif
}

static QemuDisplay qemu_display_gtk = {
    .type       = DISPLAY_TYPE_GTK,
    .early_init = early_gtk_display_init,
    .init       = gtk_display_init,
    .vc         = "vc",
};

static void register_gtk(void)
{
    qemu_display_register(&qemu_display_gtk);
}

type_init(register_gtk);

#ifdef CONFIG_OPENGL
module_dep("ui-opengl");
#endif
