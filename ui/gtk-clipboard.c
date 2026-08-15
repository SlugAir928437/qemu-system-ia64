/*
 * GTK UI -- clipboard support (GTK4 port)
 *
 * Copyright (C) 2021 Gerd Hoffmann <kraxel@redhat.com>
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
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"

#include "ui/gtk.h"

static QemuClipboardSelection gd_find_selection(GtkDisplayState *gd,
                                                GdkClipboard *clipboard)
{
    QemuClipboardSelection s;

    for (s = 0; s < QEMU_CLIPBOARD_SELECTION__COUNT; s++) {
        if (gd->gtkcb[s] == clipboard) {
            return s;
        }
    }
    return QEMU_CLIPBOARD_SELECTION_CLIPBOARD;
}

static void gd_clipboard_update_info(GtkDisplayState *gd,
                                     QemuClipboardInfo *info)
{
    QemuClipboardSelection s = info->selection;
    bool self_update = info->owner == &gd->cbpeer;

    if (info != qemu_clipboard_info(s)) {
        gd->cbpending[s] = 0;
        if (!self_update) {
            if (gd->gtkcb[s] &&
                info->types[QEMU_CLIPBOARD_TYPE_TEXT].available &&
                info->types[QEMU_CLIPBOARD_TYPE_TEXT].data) {
                gd->cbowner[s] = true;
                gdk_clipboard_set_text(
                    gd->gtkcb[s], info->types[QEMU_CLIPBOARD_TYPE_TEXT].data);
            } else if (gd->gtkcb[s]) {
                gd->cbowner[s] = false;
                gdk_clipboard_set(gd->gtkcb[s], G_TYPE_NONE, NULL);
            }
        }
        return;
    }

    if (self_update) {
        return;
    }

    /*
     * Clipboard got updated, with data probably.  No action here, we
     * are waiting for updates in gd_clipboard_read_cb().
     */
}

static void gd_clipboard_notify(Notifier *notifier, void *data)
{
    GtkDisplayState *gd =
        container_of(notifier, GtkDisplayState, cbpeer.notifier);
    QemuClipboardNotify *notify = data;

    switch (notify->type) {
    case QEMU_CLIPBOARD_UPDATE_INFO:
        gd_clipboard_update_info(gd, notify->info);
        return;
    case QEMU_CLIPBOARD_RESET_SERIAL:
        /* ignore */
        return;
    }
}

static void gd_clipboard_read_cb(GObject *source, GAsyncResult *res,
                                 gpointer data)
{
    GtkDisplayState *gd = data;
    GdkClipboard *clipboard = GDK_CLIPBOARD(source);
    QemuClipboardSelection s = gd_find_selection(gd, clipboard);
    g_autoptr(GError) err = NULL;
    g_autofree char *text = NULL;

    text = gdk_clipboard_read_text_finish(clipboard, res, &err);
    if (err) {
        return;
    }
    if (text) {
        QemuClipboardInfo *info = qemu_clipboard_info_new(&gd->cbpeer, s);
        qemu_clipboard_set_data(&gd->cbpeer, info, QEMU_CLIPBOARD_TYPE_TEXT,
                                strlen(text), text, true);
        qemu_clipboard_info_unref(info);
    }
}

static void gd_clipboard_request(QemuClipboardInfo *info,
                                 QemuClipboardType type)
{
    GtkDisplayState *gd = container_of(info->owner, GtkDisplayState, cbpeer);

    switch (type) {
    case QEMU_CLIPBOARD_TYPE_TEXT:
        if (gd->gtkcb[info->selection]) {
            gdk_clipboard_read_text_async(gd->gtkcb[info->selection], NULL,
                                          gd_clipboard_read_cb, gd);
        }
        break;
    default:
        break;
    }
}

void gd_clipboard_init(GtkDisplayState *gd)
{
    GdkDisplay *display;

    gd->cbpeer.name = "gtk";
    gd->cbpeer.notifier.notify = gd_clipboard_notify;
    gd->cbpeer.request = gd_clipboard_request;
    qemu_clipboard_peer_register(&gd->cbpeer);

    display = gdk_display_get_default();
    gd->gtkcb[QEMU_CLIPBOARD_SELECTION_CLIPBOARD] =
        gdk_display_get_clipboard(display);
    gd->gtkcb[QEMU_CLIPBOARD_SELECTION_PRIMARY] =
        gdk_display_get_primary_clipboard(display);
    gd->gtkcb[QEMU_CLIPBOARD_SELECTION_SECONDARY] = NULL;
}
