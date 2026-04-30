/* GStreamer
 * Copyright (C) <2006> Andre Moreira Magalhaes <andre.magalhaes@indt.org.br>
 * Copyright (C) <2004> David A. Schleef <ds@schleef.org>
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef GST_PLUGINS_BAD_GST_LIBRFB_GSTRFBSRC_H_
#define GST_PLUGINS_BAD_GST_LIBRFB_GSTRFBSRC_H_

#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

typedef struct _rfbClient rfbClient;

#define GST_TYPE_RFB_SRC (gst_rfb_src_get_type())
#define GST_RFB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_RFB_SRC, GstRfbSrc))
#define GST_RFB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_RFB_SRC, GstRfbSrcClass))
#define GST_IS_RFB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_RFB_SRC))
#define GST_IS_RFB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_RFB_SRC))

typedef struct _GstRfbSrc GstRfbSrc;
typedef struct _GstRfbSrcClass GstRfbSrcClass;

typedef enum {
  GST_RFB_SRC_CURSOR_MODE_AUTO,
  GST_RFB_SRC_CURSOR_MODE_CLIENT,
  GST_RFB_SRC_CURSOR_MODE_SERVER,
  GST_RFB_SRC_CURSOR_MODE_NONE
} GstRfbSrcCursorMode;

struct _GstRfbSrcClass {
  GstPushSrcClass parent_class;
};

struct _GstRfbSrc {
  GstPushSrc element;

  /* --- Connection parameters (set before PLAYING, read-only while running) ---
   */
  GstUri* uri;
  gchar* host;
  gint port;
  gchar* username;
  gchar* password;
  gchar* encodings;
  gchar* active_encodings;  /* built from encodings + use_copyrect at connect
                               time */
  gchar* last_libvnc_error; /* last error string from libvncclient */

  /* --- Capture geometry --- */
  gint offset_x; /* top-left of capture rect in server coordinates */
  gint offset_y;
  gint requested_width;  /* 0 = use remaining server width */
  gint requested_height; /* 0 = use remaining server height */
  gint output_width;     /* actual clamped output dimensions */
  gint output_height;
  gint server_width; /* reported framebuffer size */
  gint server_height;

  /* --- Options --- */
  gboolean incremental_update;
  gboolean use_copyrect;
  gboolean shared;
  gboolean view_only;
  GstRfbSrcCursorMode cursor_mode;

  /* --- Timing --- */
  gint max_framerate;
  guint frame_timeout_ms;
  guint connect_timeout;
  guint read_timeout;

  /* --- VNC client (protected by client_lock) --- */
  rfbClient* client;
  GRecMutex client_lock;

  /* --- Streaming state (protected by client_lock unless noted) --- */
  gboolean connected;
  gint unlocked; /* atomic: non-zero when flushing */
  gboolean frame_valid;
  gboolean frame_dirty;
  gboolean cursor_dirty;
  gboolean geometry_changed;
  gboolean have_caps;
  gboolean update_request_pending;

  /* --- Pending pointer (protected by pending_pointer_lock) --- */
  GMutex pending_pointer_lock;
  gboolean pending_pointer_valid;
  gint pending_pointer_x;
  gint pending_pointer_y;
  guint pending_pointer_button_mask;

  /* --- Input state (protected by client_lock) --- */
  guint button_mask;
  GHashTable* pressed_keys; /* DOM code → keysym for currently pressed keys */

  /* --- Cursor state (protected by client_lock) --- */
  gboolean cursor_client_requested;
  gboolean cursor_shape_valid;
  gboolean cursor_position_valid;
  gint cursor_auto_fallback_frames;
  gint cursor_x; /* absolute server coordinates */
  gint cursor_y;
  gint cursor_hot_x;
  gint cursor_hot_y;
  gint cursor_width;
  gint cursor_height;
  gint cursor_bpp;
  guint8* cursor_source; /* pixel data; size = width * height * cursor_bpp */
  guint8*
      cursor_mask; /* 1 byte per pixel (0=transparent); size = width * height */

  /* --- Output --- */
  GstBufferPool* pool;
  GstVideoInfo vinfo;
  GstClockTime frame_duration;
  GstClockTime last_frame_time;
  GstClockTime next_pts;
};

GType gst_rfb_src_get_type(void);
GST_ELEMENT_REGISTER_DECLARE(rfbsrc);

G_END_DECLS

#endif  // GST_PLUGINS_BAD_GST_LIBRFB_GSTRFBSRC_H_
