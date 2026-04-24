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
#include <gst/video/gstvideopool.h>
#include <rfb/rfbclient.h>

G_BEGIN_DECLS
#define GST_TYPE_RFB_SRC \
  (gst_rfb_src_get_type())
#define GST_RFB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RFB_SRC,GstRfbSrc))
#define GST_RFB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RFB_SRC,GstRfbSrc))
#define GST_IS_RFB_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RFB_SRC))
#define GST_IS_RFB_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RFB_SRC))
typedef struct _GstRfbSrc GstRfbSrc;
typedef struct _GstRfbSrcClass GstRfbSrcClass;

struct _GstRfbSrcClass
{
  GstPushSrcClass parent_class;
};

struct _GstRfbSrc
{
  GstPushSrc element;

  /* Connection parameters — authoritative storage, applied to decoder in start() */
  gchar *host;
  gint port;
  gchar *user;
  gchar *pass;
  gint offset_x;
  gint offset_y;
  gint width;
  gint height;
  gboolean shared;

  rfbClient *decoder;   /* non-NULL only between start() and stop() */

  volatile gint unlocked; /* set by unlock(), cleared by unlock_stop() */

  /* Client-side cursor blending: populated by GotCursorShape / HandleCursorPos.
   * Only used when the server sends cursor shape/position encodings
   * (useRemoteCursor = 1).  When the server doesn't support those encodings it
   * falls back to drawing the cursor into the framebuffer directly, so these
   * fields are never touched and blending is simply skipped. */
  gint cursor_x;
  gint cursor_y;
  gint cursor_hot_x;
  gint cursor_hot_y;
  gint cursor_width;
  gint cursor_height;

  /* Pre-computed at GotCursorShape time: 1-byte-per-pixel mask of transparent
   * pixels adjacent (4-connected) to opaque cursor pixels.  Drawn in a
   * contrasting colour before the cursor pixels to guarantee visibility
   * regardless of cursor colour or background colour. */
  guint8 *cursor_outline;
  guint8  cursor_outline_pixel[4];
};

GType gst_rfb_src_get_type (void);

G_END_DECLS
#endif
