/* GStreamer
 * Copyright (C) <2007> Thijs Vermeir <thijsvermeir@gmail.com>
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

  /* connection properties */
  GstUri *uri;
  gchar *host;
  gint port;
  gchar *password;

  /* stream properties */
  gboolean incremental_update;
  gboolean view_only;
  gboolean shared;
  guint offset_x;
  guint offset_y;
  guint req_width;   /* 0 = full desktop width */
  guint req_height;  /* 0 = full desktop height */

  /* input state - only modified from the streaming/event thread */
  guint button_mask;

  /* libvncclient handle - valid between negotiate and stop */
  rfbClient *client;

  /* background receiver thread drives rfbProcessServerMessage;
   * input events (SendPointerEvent / SendKeyEvent) are sent directly
   * from gst_rfb_src_event and never blocked by frame reception */
  GThread   *receiver_thread;
  gint       running;   /* g_atomic_int, 1 while receiver loop is alive */

  /* frame double-buffer:
   *   receiver thread writes client->frameBuffer (libvncclient-managed),
   *   FinishedFrameBufferUpdate snapshots it into frame_snapshot under mutex,
   *   gst_rfb_src_fill reads frame_snapshot under mutex.
   *   Input events bypass this path entirely. */
  GMutex  frame_mutex;
  GCond   frame_cond;
  gboolean frame_ready;  /* protected by frame_mutex */
  gboolean flushing;     /* protected by frame_mutex */
  guint8  *frame_snapshot;
  gint     fb_width;
  gint     fb_height;
  gsize    fb_size;      /* fb_width * fb_height * 4 */
};

GType gst_rfb_src_get_type (void);
GST_ELEMENT_REGISTER_DECLARE (rfbsrc);

G_END_DECLS

#endif /* __GST_RFB_SRC_H__ */
