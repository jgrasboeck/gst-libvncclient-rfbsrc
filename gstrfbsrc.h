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

#ifndef __GST_RFB_SRC_H__
#define __GST_RFB_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

typedef struct _rfbClient rfbClient;

#define GST_TYPE_RFB_SRC \
  (gst_rfb_src_get_type())
#define GST_RFB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RFB_SRC,GstRfbSrc))
#define GST_RFB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RFB_SRC,GstRfbSrcClass))
#define GST_IS_RFB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RFB_SRC))
#define GST_IS_RFB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RFB_SRC))

typedef struct _GstRfbSrc GstRfbSrc;
typedef struct _GstRfbSrcClass GstRfbSrcClass;

typedef enum
{
  GST_RFB_SRC_CURSOR_MODE_AUTO,
  GST_RFB_SRC_CURSOR_MODE_CLIENT,
  GST_RFB_SRC_CURSOR_MODE_SERVER,
  GST_RFB_SRC_CURSOR_MODE_NONE
} GstRfbSrcCursorMode;

struct _GstRfbSrcClass
{
  GstPushSrcClass parent_class;
};

struct _GstRfbSrc
{
  GstPushSrc element;

  GstUri *uri;
  gchar *host;
  gint port;
  gchar *username;
  gchar *password;
  gchar *version;
  gchar *encodings;
  gchar *active_encodings;
  gchar *last_libvnc_error;

  gint offset_x;
  gint offset_y;
  gint requested_width;
  gint requested_height;
  gint output_x;
  gint output_y;
  gint output_width;
  gint output_height;
  gint server_width;
  gint server_height;

  gboolean incremental_update;
  gboolean use_copyrect;
  gboolean shared;
  gboolean view_only;
  GstRfbSrcCursorMode cursor_mode;

  gint max_framerate;
  guint frame_timeout_ms;
  guint connect_timeout;
  guint read_timeout;

  rfbClient *client;
  GRecMutex client_lock;

  gboolean connected;
  gint unlocked;
  gboolean frame_valid;
  gboolean frame_dirty;
  gboolean geometry_changed;
  gboolean have_caps;
  gboolean update_request_pending;

  guint button_mask;

  gboolean cursor_client_requested;
  gboolean cursor_shape_valid;
  gboolean cursor_position_valid;
  gint cursor_auto_fallback_frames;
  gint cursor_x;
  gint cursor_y;
  gint cursor_hot_x;
  gint cursor_hot_y;
  gint cursor_width;
  gint cursor_height;
  gint cursor_bpp;
  guint8 *cursor_source;
  guint8 *cursor_mask;
  gsize cursor_source_size;
  gsize cursor_mask_size;

  GstVideoInfo vinfo;
  GstClockTime frame_duration;
  GstClockTime last_frame_time;
  GstClockTime next_pts;
};

GType gst_rfb_src_get_type (void);
GST_ELEMENT_REGISTER_DECLARE (rfbsrc);

G_END_DECLS

#endif
