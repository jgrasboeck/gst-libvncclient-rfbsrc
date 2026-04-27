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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstrfbsrc.h"
#include "gstrfb-utils.h"

#include <gst/video/video.h>
#include <glib/gi18n-lib.h>

#include <string.h>
#include <stdlib.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#endif

/* Tag used as key for rfbClientSetClientData / rfbClientGetClientData */
static gint rfb_client_key;

#define DEFAULT_PROP_HOST         "127.0.0.1"
#define DEFAULT_PROP_PORT         5900
#define DEFAULT_PROP_URI          "rfb://"DEFAULT_PROP_HOST":"G_STRINGIFY(DEFAULT_PROP_PORT)

enum
{
  PROP_0,
  PROP_URI,
  PROP_HOST,
  PROP_PORT,
  PROP_PASSWORD,
  PROP_OFFSET_X,
  PROP_OFFSET_Y,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_INCREMENTAL,
  PROP_SHARED,
  PROP_VIEWONLY
};

GST_DEBUG_CATEGORY_STATIC (rfbsrc_debug);
#define GST_CAT_DEFAULT rfbsrc_debug

/* Always produce BGRx (32-bit, B=byte0 G=byte1 R=byte2 x=byte3, LE).
 * libvncclient is told to use this exact layout via client->format before
 * connecting, so no runtime format detection is needed. */
static GstStaticPadTemplate gst_rfb_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("BGRx")));

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static void gst_rfb_src_finalize (GObject * object);
static void gst_rfb_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rfb_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_rfb_src_negotiate (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_stop (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_event (GstBaseSrc * bsrc, GstEvent * event);
static gboolean gst_rfb_src_unlock (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_unlock_stop (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_decide_allocation (GstBaseSrc * bsrc,
    GstQuery * query);
static GstFlowReturn gst_rfb_src_fill (GstPushSrc * psrc, GstBuffer * outbuf);

static void gst_rfb_src_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gboolean gst_rfb_src_uri_set_uri (GstURIHandler * handler,
    const gchar * uri, GError ** error);

#define gst_rfb_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstRfbSrc, gst_rfb_src, GST_TYPE_PUSH_SRC,
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER, gst_rfb_src_uri_handler_init));
GST_ELEMENT_REGISTER_DEFINE (rfbsrc, "rfbsrc", GST_RANK_NONE,
    GST_TYPE_RFB_SRC);

/* ------------------------------------------------------------------ */
/* GObject boilerplate                                                  */
/* ------------------------------------------------------------------ */

static void
gst_rfb_src_class_init (GstRfbSrcClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBaseSrcClass *gstbasesrc_class = (GstBaseSrcClass *) klass;
  GstPushSrcClass *gstpushsrc_class = (GstPushSrcClass *) klass;
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (rfbsrc_debug, "rfbsrc", 0, "rfb src element");

  gobject_class->finalize = gst_rfb_src_finalize;
  gobject_class->set_property = gst_rfb_src_set_property;
  gobject_class->get_property = gst_rfb_src_get_property;

  /**
   * GstRfbSrc:uri:
   *
   * URI to an RFB server. All GStreamer parameters can be encoded in the URI.
   *
   * Since: 1.22
   */
  g_object_class_install_property (gobject_class, PROP_URI,
      g_param_spec_string ("uri", "URI",
          "URI in the form of rfb://host:port?query", DEFAULT_PROP_URI,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HOST,
      g_param_spec_string ("host", "Host", "VNC server hostname",
          DEFAULT_PROP_HOST, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PORT,
      g_param_spec_int ("port", "Port", "VNC server port",
          1, 65535, DEFAULT_PROP_PORT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PASSWORD,
      g_param_spec_string ("password", "Password", "VNC authentication password",
          "", G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_OFFSET_X,
      g_param_spec_int ("offset-x", "Offset X", "Horizontal crop offset", 0,
          65535, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_OFFSET_Y,
      g_param_spec_int ("offset-y", "Offset Y", "Vertical crop offset", 0,
          65535, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_int ("width", "Width", "Capture width (0 = full)", 0, 65535,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_int ("height", "Height", "Capture height (0 = full)", 0,
          65535, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_INCREMENTAL,
      g_param_spec_boolean ("incremental", "Incremental updates",
          "Request incremental framebuffer updates", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SHARED,
      g_param_spec_boolean ("shared", "Shared desktop",
          "Allow concurrent connections to the VNC server", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VIEWONLY,
      g_param_spec_boolean ("view-only", "View only",
          "Disable sending input events to the VNC server", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstbasesrc_class->negotiate = GST_DEBUG_FUNCPTR (gst_rfb_src_negotiate);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_rfb_src_stop);
  gstbasesrc_class->event = GST_DEBUG_FUNCPTR (gst_rfb_src_event);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_rfb_src_unlock);
  gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_rfb_src_unlock_stop);
  gstbasesrc_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_rfb_src_decide_allocation);
  gstpushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_rfb_src_fill);

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_rfb_src_template);
  gst_element_class_set_static_metadata (gstelement_class, "Rfb source",
      "Source/Video",
      "Connects to a VNC/RFB server and produces a video stream",
      "David A. Schleef <ds@schleef.org>, "
      "Andre Moreira Magalhaes <andre.magalhaes@indt.org.br>, "
      "Thijs Vermeir <thijsvermeir@gmail.com>");
}

static void
gst_rfb_src_init (GstRfbSrc * src)
{
  GstBaseSrc *bsrc = GST_BASE_SRC (src);

  gst_pad_use_fixed_caps (GST_BASE_SRC_PAD (bsrc));
  gst_base_src_set_live (bsrc, TRUE);
  gst_base_src_set_format (bsrc, GST_FORMAT_TIME);

  src->uri = gst_uri_from_string (DEFAULT_PROP_URI);
  src->host = g_strdup (DEFAULT_PROP_HOST);
  src->port = DEFAULT_PROP_PORT;
  src->incremental_update = TRUE;
  src->shared = TRUE;
  src->view_only = FALSE;

  g_mutex_init (&src->frame_mutex);
  g_cond_init (&src->frame_cond);
}

static void
gst_rfb_src_finalize (GObject * object)
{
  GstRfbSrc *src = GST_RFB_SRC (object);

  if (src->uri)
    gst_uri_unref (src->uri);
  g_free (src->host);
  g_free (src->password);
  g_free (src->frame_snapshot);
  g_mutex_clear (&src->frame_mutex);
  g_cond_clear (&src->frame_cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_rfb_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRfbSrc *src = GST_RFB_SRC (object);

  switch (prop_id) {
    case PROP_URI:
      gst_rfb_src_uri_set_uri ((GstURIHandler *) src,
          g_value_get_string (value), NULL);
      break;
    case PROP_HOST:
      g_free (src->host);
      src->host = g_value_dup_string (value);
      break;
    case PROP_PORT:
      src->port = g_value_get_int (value);
      break;
    case PROP_PASSWORD:
      g_free (src->password);
      src->password = g_value_dup_string (value);
      break;
    case PROP_OFFSET_X:
      src->offset_x = g_value_get_int (value);
      break;
    case PROP_OFFSET_Y:
      src->offset_y = g_value_get_int (value);
      break;
    case PROP_WIDTH:
      src->req_width = g_value_get_int (value);
      break;
    case PROP_HEIGHT:
      src->req_height = g_value_get_int (value);
      break;
    case PROP_INCREMENTAL:
      src->incremental_update = g_value_get_boolean (value);
      break;
    case PROP_SHARED:
      src->shared = g_value_get_boolean (value);
      break;
    case PROP_VIEWONLY:
      src->view_only = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rfb_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRfbSrc *src = GST_RFB_SRC (object);

  switch (prop_id) {
    case PROP_URI:
      GST_OBJECT_LOCK (object);
      g_value_take_string (value,
          src->uri ? gst_uri_to_string (src->uri) : NULL);
      GST_OBJECT_UNLOCK (object);
      break;
    case PROP_HOST:
      g_value_set_string (value, src->host);
      break;
    case PROP_PORT:
      g_value_set_int (value, src->port);
      break;
    case PROP_OFFSET_X:
      g_value_set_int (value, src->offset_x);
      break;
    case PROP_OFFSET_Y:
      g_value_set_int (value, src->offset_y);
      break;
    case PROP_WIDTH:
      g_value_set_int (value, src->req_width);
      break;
    case PROP_HEIGHT:
      g_value_set_int (value, src->req_height);
      break;
    case PROP_INCREMENTAL:
      g_value_set_boolean (value, src->incremental_update);
      break;
    case PROP_SHARED:
      g_value_set_boolean (value, src->shared);
      break;
    case PROP_VIEWONLY:
      g_value_set_boolean (value, src->view_only);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* ------------------------------------------------------------------ */
/* Buffer pool allocation                                               */
/* ------------------------------------------------------------------ */

static gboolean
gst_rfb_src_decide_allocation (GstBaseSrc * bsrc, GstQuery * query)
{
  GstBufferPool *pool = NULL;
  guint size, min = 1, max = 0;
  GstStructure *config;
  GstCaps *caps;
  GstVideoInfo info;
  gboolean ret;

  gst_query_parse_allocation (query, &caps, NULL);
  if (!caps || !gst_video_info_from_caps (&info, caps))
    return FALSE;

  /* Require exact buffer size - we memcpy directly into the buffer */
  while (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    if (size == info.size)
      break;
    gst_query_remove_nth_allocation_pool (query, 0);
    gst_object_unref (pool);
    pool = NULL;
  }

  if (pool == NULL) {
    pool = gst_video_buffer_pool_new ();
    size = info.size;
    min = 1;
    max = 0;
    if (gst_query_get_n_allocation_pools (query) > 0)
      gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
    else
      gst_query_add_allocation_pool (query, pool, size, min, max);
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, min, max);
  ret = gst_buffer_pool_set_config (pool, config);
  gst_object_unref (pool);

  return ret;
}

/* ------------------------------------------------------------------ */
/* libvncclient callbacks                                               */
/* ------------------------------------------------------------------ */

/* Called by libvncclient to (re)allocate the remote framebuffer.
 * On initial connect this runs on the negotiation thread before the
 * background receiver thread is started, so mutex use is safe. */
static rfbBool
malloc_frame_buffer_cb (rfbClient * client)
{
  GstRfbSrc *src = rfbClientGetClientData (client, &rfb_client_key);
  gsize new_size = (gsize) client->width * client->height * 4;

  /* Server resize after initial connect - not fully supported yet */
  if (src->fb_width > 0 &&
      (src->fb_width != client->width || src->fb_height != client->height)) {
    GST_WARNING_OBJECT (src,
        "Server resize to %dx%d not supported; ignoring",
        client->width, client->height);
    g_free (client->frameBuffer);
    client->frameBuffer = g_malloc (new_size);
    return client->frameBuffer != NULL ? TRUE : FALSE;
  }

  g_free (client->frameBuffer);
  client->frameBuffer = g_malloc (new_size);
  if (!client->frameBuffer)
    return FALSE;

  g_mutex_lock (&src->frame_mutex);
  src->fb_width = client->width;
  src->fb_height = client->height;
  src->fb_size = new_size;
  g_free (src->frame_snapshot);
  src->frame_snapshot = g_malloc (new_size);
  g_mutex_unlock (&src->frame_mutex);

  return src->frame_snapshot != NULL ? TRUE : FALSE;
}

/* Called by libvncclient when an individual rectangle has been updated.
 * Used only for debug logging; the snapshot happens in finished_cb. */
static void
got_frame_buffer_update_cb (rfbClient * client, int x, int y, int w, int h)
{
  GstRfbSrc *src = rfbClientGetClientData (client, &rfb_client_key);
  GST_LOG_OBJECT (src, "rect updated: %dx%d at (%d,%d)", w, h, x, y);
}

/* Called by libvncclient when the server has finished sending a complete
 * FramebufferUpdate message (all rectangles received).  We snapshot the
 * framebuffer under the mutex, signal fill(), and immediately request the
 * next update so server-to-client latency stays minimal.
 *
 * Input events (SendPointerEvent / SendKeyEvent) are sent from
 * gst_rfb_src_event on a different thread and never touch this path,
 * so they are never blocked by frame reception. */
static void
finished_frame_buffer_update_cb (rfbClient * client)
{
  GstRfbSrc *src = rfbClientGetClientData (client, &rfb_client_key);
  guint out_w, out_h;

  if (!g_atomic_int_get (&src->running))
    return;

  out_w = src->req_width ? src->req_width : src->fb_width;
  out_h = src->req_height ? src->req_height : src->fb_height;

  g_mutex_lock (&src->frame_mutex);

  if (src->frame_snapshot) {
    if (out_w == (guint) src->fb_width && out_h == (guint) src->fb_height
        && src->offset_x == 0 && src->offset_y == 0) {
      /* Fast path: full framebuffer, single memcpy */
      memcpy (src->frame_snapshot, client->frameBuffer, src->fb_size);
    } else {
      /* Sub-region crop: row-by-row copy */
      guint stride = src->fb_width * 4;
      const guint8 *s =
          (const guint8 *) client->frameBuffer
          + src->offset_y * stride + src->offset_x * 4;
      guint8 *d = src->frame_snapshot;
      guint row_bytes = out_w * 4;
      for (guint y = 0; y < out_h; y++) {
        memcpy (d, s, row_bytes);
        s += stride;
        d += row_bytes;
      }
    }
    src->frame_ready = TRUE;
    g_cond_signal (&src->frame_cond);
  }

  g_mutex_unlock (&src->frame_mutex);

  /* Request the next update immediately for lowest end-to-end latency */
  SendFramebufferUpdateRequest (client,
      src->offset_x, src->offset_y,
      out_w, out_h,
      src->incremental_update ? TRUE : FALSE);
}

/* libvncclient calls this when VNC authentication requires a password */
static char *
get_password_cb (rfbClient * client)
{
  GstRfbSrc *src = rfbClientGetClientData (client, &rfb_client_key);
  return strdup (src->password ? src->password : "");
}

/* ------------------------------------------------------------------ */
/* Background receiver thread                                           */
/* ------------------------------------------------------------------ */

/* This thread owns all rfbProcessServerMessage calls.  Input events
 * (SendPointerEvent / SendKeyEvent) are written to the socket from the
 * event thread using libvncclient's internal send mutex, so they never
 * stall waiting for a frame decode to finish. */
static gpointer
receiver_thread_func (gpointer data)
{
  GstRfbSrc *src = data;
  rfbClient *client = src->client;

  GST_DEBUG_OBJECT (src, "receiver thread started");

  while (g_atomic_int_get (&src->running)) {
    /* 50 ms timeout so we can react to running=0 within half a frame */
    if (!rfbProcessServerMessage (client, 50 * 1000)) {
      if (g_atomic_int_get (&src->running)) {
        GST_ELEMENT_ERROR (src, RESOURCE, READ,
            ("VNC connection lost to %s:%d", src->host, src->port), (NULL));
        g_atomic_int_set (&src->running, 0);
      }
      break;
    }
  }

  /* Unblock fill() if it is waiting for a frame */
  g_mutex_lock (&src->frame_mutex);
  g_cond_signal (&src->frame_cond);
  g_mutex_unlock (&src->frame_mutex);

  GST_DEBUG_OBJECT (src, "receiver thread exiting");
  return NULL;
}

/* ------------------------------------------------------------------ */
/* GstBaseSrc / GstPushSrc vfuncs                                       */
/* ------------------------------------------------------------------ */

static gboolean
gst_rfb_src_negotiate (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);
  rfbClient *client;
  GstVideoInfo vinfo;
  GstCaps *caps;
  gchar *stream_id;
  guint out_w, out_h;
  int fake_argc = 0;

  if (src->client)
    return TRUE;

  GST_DEBUG_OBJECT (src, "connecting to %s:%d", src->host, src->port);

  /* rfbGetClient(bitsPerSample=8, samplesPerPixel=3, bytesPerPixel=4) */
  client = rfbGetClient (8, 3, 4);
  if (!client) {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        ("Failed to allocate VNC client"), (NULL));
    return FALSE;
  }

  /* Back-pointer so callbacks can reach the GstRfbSrc */
  rfbClientSetClientData (client, &rfb_client_key, src);

  client->MallocFrameBuffer = malloc_frame_buffer_cb;
  client->GotFrameBufferUpdate = got_frame_buffer_update_cb;
  client->FinishedFrameBufferUpdate = finished_frame_buffer_update_cb;
  client->GetPassword = get_password_cb;

  client->serverHost = g_strdup (src->host);
  client->serverPort = src->port;
  client->appData.shareDesktop = src->shared ? TRUE : FALSE;
  /* Disable server-side cursor - we want it composited into the framebuffer */
  client->appData.useRemoteCursor = FALSE;
  client->programName = "gst-rfbsrc";

  /* Force BGRx pixel format so we always produce a known GstVideoFormat.
   * Little-endian: B=byte0 (shift 0), G=byte1 (shift 8), R=byte2 (shift 16). */
  client->format.bitsPerPixel = 32;
  client->format.depth = 24;
  client->format.bigEndian = FALSE;
  client->format.trueColour = TRUE;
  client->format.redMax = 255;
  client->format.greenMax = 255;
  client->format.blueMax = 255;
  client->format.redShift = 16;
  client->format.greenShift = 8;
  client->format.blueShift = 0;

  src->client = client;

  /* rfbInitClient does the full handshake (version, auth, server init,
   * SetPixelFormat, SetEncodings).  It calls malloc_frame_buffer_cb which
   * sets src->fb_width / fb_height / fb_size / frame_snapshot. */
  if (!rfbInitClient (client, &fake_argc, NULL)) {
    /* rfbInitClient frees the client on failure */
    src->client = NULL;
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        ("Could not connect to VNC server %s:%d", src->host, src->port),
        (NULL));
    return FALSE;
  }

  /* Determine output dimensions (may be cropped) */
  out_w = src->req_width ? MIN (src->req_width,
      src->fb_width - (gint) src->offset_x) : src->fb_width - src->offset_x;
  out_h = src->req_height ? MIN (src->req_height,
      src->fb_height - (gint) src->offset_y) : src->fb_height - src->offset_y;

  if (out_w == 0 || out_h == 0) {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        ("Invalid crop: offset (%u,%u) exceeds desktop %dx%d",
            src->offset_x, src->offset_y, src->fb_width, src->fb_height),
        (NULL));
    rfbClientCleanup (client);
    src->client = NULL;
    return FALSE;
  }

  GST_DEBUG_OBJECT (src, "desktop %dx%d, output %ux%u at offset (%u,%u)",
      src->fb_width, src->fb_height, out_w, out_h,
      src->offset_x, src->offset_y);

  stream_id = gst_pad_create_stream_id_printf (GST_BASE_SRC_PAD (bsrc),
      GST_ELEMENT (src), "%s:%d", src->host, src->port);
  gst_pad_push_event (GST_BASE_SRC_PAD (bsrc),
      gst_event_new_stream_start (stream_id));
  g_free (stream_id);

  gst_video_info_init (&vinfo);
  gst_video_info_set_format (&vinfo, GST_VIDEO_FORMAT_BGRx, out_w, out_h);
  caps = gst_video_info_to_caps (&vinfo);
  gst_base_src_set_caps (bsrc, caps);
  gst_caps_unref (caps);

  /* Kick off the first update request (non-incremental = full frame) */
  SendFramebufferUpdateRequest (client,
      src->offset_x, src->offset_y, out_w, out_h, FALSE);

  /* Start the background receiver thread */
  g_atomic_int_set (&src->running, 1);
  src->flushing = FALSE;
  src->frame_ready = FALSE;
  src->receiver_thread =
      g_thread_new ("vnc-receiver", receiver_thread_func, src);

  return TRUE;
}

static gboolean
gst_rfb_src_stop (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  /* Signal the receiver thread to stop */
  g_atomic_int_set (&src->running, 0);

  /* Unblock fill() in case it is waiting on the condvar */
  g_mutex_lock (&src->frame_mutex);
  g_cond_signal (&src->frame_cond);
  g_mutex_unlock (&src->frame_mutex);

  if (src->receiver_thread) {
    g_thread_join (src->receiver_thread);
    src->receiver_thread = NULL;
  }

  if (src->client) {
    rfbClientCleanup (src->client);
    src->client = NULL;
  }

  g_mutex_lock (&src->frame_mutex);
  g_free (src->frame_snapshot);
  src->frame_snapshot = NULL;
  src->fb_width = 0;
  src->fb_height = 0;
  src->fb_size = 0;
  src->frame_ready = FALSE;
  src->flushing = FALSE;
  g_mutex_unlock (&src->frame_mutex);

  return TRUE;
}

/* gst_rfb_src_fill is the GStreamer streaming thread.
 * It blocks on the condvar until the receiver thread (which runs
 * rfbProcessServerMessage) completes a full FramebufferUpdate and signals.
 * Because input events are sent directly via libvncclient's socket-write
 * path (which has its own internal mutex), they are never serialised
 * behind this wait. */
static GstFlowReturn
gst_rfb_src_fill (GstPushSrc * psrc, GstBuffer * outbuf)
{
  GstRfbSrc *src = GST_RFB_SRC (psrc);
  GstMapInfo info;
  guint out_w, out_h;
  gsize expected;

  g_mutex_lock (&src->frame_mutex);

  while (!src->frame_ready && !src->flushing
      && g_atomic_int_get (&src->running))
    g_cond_wait (&src->frame_cond, &src->frame_mutex);

  if (src->flushing) {
    g_mutex_unlock (&src->frame_mutex);
    return GST_FLOW_FLUSHING;
  }

  if (!g_atomic_int_get (&src->running) && !src->frame_ready) {
    g_mutex_unlock (&src->frame_mutex);
    return GST_FLOW_EOS;
  }

  out_w = src->req_width ? MIN (src->req_width,
      src->fb_width - (gint) src->offset_x) : src->fb_width - src->offset_x;
  out_h = src->req_height ? MIN (src->req_height,
      src->fb_height - (gint) src->offset_y) : src->fb_height - src->offset_y;
  expected = (gsize) out_w * out_h * 4;

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    g_mutex_unlock (&src->frame_mutex);
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Could not map output buffer"), (NULL));
    return GST_FLOW_ERROR;
  }

  if (G_LIKELY (info.size >= expected && src->frame_snapshot)) {
    memcpy (info.data, src->frame_snapshot, expected);
  } else {
    GST_WARNING_OBJECT (src, "buffer size mismatch: have %" G_GSIZE_FORMAT
        " need %" G_GSIZE_FORMAT, info.size, expected);
  }

  src->frame_ready = FALSE;
  g_mutex_unlock (&src->frame_mutex);

  GST_BUFFER_PTS (outbuf) =
      gst_clock_get_time (GST_ELEMENT_CLOCK (src)) -
      GST_ELEMENT_CAST (src)->base_time;

  gst_buffer_unmap (outbuf, &info);
  return GST_FLOW_OK;
}

/* ------------------------------------------------------------------ */
/* Input event handling                                                 */
/* ------------------------------------------------------------------ */

#ifndef HAVE_X11
/* Map GstNavigation key strings to X11 keysyms (used by the RFB protocol).
 * For single printable ASCII/Latin-1 characters the keysym equals the
 * Unicode codepoint; for special keys we use a static lookup table.
 * libvncclient's own SendKeyEvent uses these same values. */
static guint32
key_string_to_keysym (const gchar * key)
{
  static const struct
  {
    const gchar *name;
    guint32 keysym;
  } table[] = {
    /* clang-format off */
    { "Return",      0xff0d }, { "BackSpace",   0xff08 }, { "Tab",         0xff09 },
    { "Escape",      0xff1b }, { "Delete",      0xffff }, { "Insert",      0xff63 },
    { "Home",        0xff50 }, { "End",         0xff57 }, { "Page_Up",     0xff55 },
    { "Page_Down",   0xff56 }, { "Up",          0xff52 }, { "Down",        0xff54 },
    { "Left",        0xff51 }, { "Right",       0xff53 }, { "space",       0x0020 },
    { "F1",  0xffbe }, { "F2",  0xffbf }, { "F3",  0xffc0 }, { "F4",  0xffc1 },
    { "F5",  0xffc2 }, { "F6",  0xffc3 }, { "F7",  0xffc4 }, { "F8",  0xffc5 },
    { "F9",  0xffc6 }, { "F10", 0xffc7 }, { "F11", 0xffc8 }, { "F12", 0xffc9 },
    { "Shift_L",     0xffe1 }, { "Shift_R",     0xffe2 },
    { "Control_L",   0xffe3 }, { "Control_R",   0xffe4 },
    { "Alt_L",       0xffe9 }, { "Alt_R",       0xffea },
    { "Meta_L",      0xffe7 }, { "Meta_R",      0xffe8 },
    { "Super_L",     0xffeb }, { "Super_R",     0xffec },
    { "Caps_Lock",   0xffe5 }, { "Num_Lock",    0xff7f },
    { "minus",       0x002d }, { "equal",       0x003d },
    { "bracketleft", 0x005b }, { "bracketright",0x005d },
    { "backslash",   0x005c }, { "semicolon",   0x003b },
    { "apostrophe",  0x0027 }, { "grave",       0x0060 },
    { "comma",       0x002c }, { "period",      0x002e }, { "slash", 0x002f },
    { "Print",       0xff61 }, { "Scroll_Lock", 0xff14 }, { "Pause", 0xff13 },
    /* clang-format on */
    { NULL, 0 }
  };

  /* Single printable character: keysym == codepoint for Latin-1 range */
  if (key[0] != '\0' && key[1] == '\0')
    return (guchar) key[0];

  for (int i = 0; table[i].name; i++) {
    if (strcmp (key, table[i].name) == 0)
      return table[i].keysym;
  }

  return 0;                     /* no match */
}
#endif /* !HAVE_X11 */

/* gst_rfb_src_event is called from GStreamer's navigation infrastructure.
 * SendPointerEvent and SendKeyEvent acquire libvncclient's internal send
 * mutex and write directly to the socket, so they complete immediately
 * regardless of what the receiver thread is doing. */
static gboolean
gst_rfb_src_event (GstBaseSrc * bsrc, GstEvent * event)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);
  GstNavigationEventType event_type;
  gdouble x, y;
  gint button;

  if (GST_EVENT_TYPE (event) != GST_EVENT_NAVIGATION)
    return TRUE;

  if (src->view_only || !src->client)
    return TRUE;

  event_type = gst_navigation_event_get_type (event);

  switch (event_type) {
    case GST_NAVIGATION_EVENT_KEY_PRESS:
    case GST_NAVIGATION_EVENT_KEY_RELEASE:{
      const gchar *key = NULL;
      guint32 keysym = 0;

      gst_navigation_event_parse_key_event (event, &key);
      if (!key)
        break;

#ifdef HAVE_X11
      keysym = XStringToKeysym (key);
#else
      keysym = key_string_to_keysym (key);
#endif
      if (keysym != 0) {
        GST_LOG_OBJECT (src, "key %s keysym=0x%x %s",
            key, keysym,
            event_type == GST_NAVIGATION_EVENT_KEY_PRESS ? "press" : "release");
        SendKeyEvent (src->client, keysym,
            event_type == GST_NAVIGATION_EVENT_KEY_PRESS ? TRUE : FALSE);
      }
      break;
    }

    case GST_NAVIGATION_EVENT_MOUSE_BUTTON_PRESS:
      gst_navigation_event_parse_mouse_button_event (event, &button, &x, &y);
      x += src->offset_x;
      y += src->offset_y;
      src->button_mask |= (1 << (button - 1));
      GST_LOG_OBJECT (src, "mouse press btn=%d mask=%u x=%d y=%d",
          button, src->button_mask, (gint) x, (gint) y);
      SendPointerEvent (src->client, (gint) x, (gint) y, src->button_mask);
      break;

    case GST_NAVIGATION_EVENT_MOUSE_BUTTON_RELEASE:
      gst_navigation_event_parse_mouse_button_event (event, &button, &x, &y);
      x += src->offset_x;
      y += src->offset_y;
      src->button_mask &= ~(1 << (button - 1));
      GST_LOG_OBJECT (src, "mouse release btn=%d mask=%u x=%d y=%d",
          button, src->button_mask, (gint) x, (gint) y);
      SendPointerEvent (src->client, (gint) x, (gint) y, src->button_mask);
      break;

    case GST_NAVIGATION_EVENT_MOUSE_MOVE:
      gst_navigation_event_parse_mouse_move_event (event, &x, &y);
      x += src->offset_x;
      y += src->offset_y;
      GST_LOG_OBJECT (src, "mouse move mask=%u x=%d y=%d",
          src->button_mask, (gint) x, (gint) y);
      SendPointerEvent (src->client, (gint) x, (gint) y, src->button_mask);
      break;

    default:
      break;
  }

  return TRUE;
}

static gboolean
gst_rfb_src_unlock (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  g_mutex_lock (&src->frame_mutex);
  src->flushing = TRUE;
  g_cond_signal (&src->frame_cond);
  g_mutex_unlock (&src->frame_mutex);

  return TRUE;
}

static gboolean
gst_rfb_src_unlock_stop (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  g_mutex_lock (&src->frame_mutex);
  src->flushing = FALSE;
  g_mutex_unlock (&src->frame_mutex);

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* URI handler                                                          */
/* ------------------------------------------------------------------ */

static GstURIType
gst_rfb_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
gst_rfb_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { "rfb", NULL };
  return protocols;
}

static gchar *
gst_rfb_src_uri_get_uri (GstURIHandler * handler)
{
  GstRfbSrc *src = (GstRfbSrc *) handler;
  gchar *str;

  GST_OBJECT_LOCK (src);
  str = src->uri ? gst_uri_to_string (src->uri) : NULL;
  GST_OBJECT_UNLOCK (src);

  return str;
}

static gboolean
gst_rfb_src_uri_set_uri (GstURIHandler * handler, const gchar * str_uri,
    GError ** error)
{
  GstRfbSrc *src = (GstRfbSrc *) handler;
  GstUri *uri;
  const gchar *userinfo;

  g_return_val_if_fail (str_uri != NULL, FALSE);

  if (GST_STATE (src) >= GST_STATE_PAUSED) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_STATE,
        _("Changing the URI on rfbsrc while running is not supported"));
    return FALSE;
  }

  if (!(uri = gst_uri_from_string (str_uri))) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        _("Invalid URI: %s"), str_uri);
    return FALSE;
  }

  if (g_strcmp0 (gst_uri_get_scheme (uri), "rfb") != 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        _("URI scheme must be 'rfb': %s"), str_uri);
    gst_uri_unref (uri);
    return FALSE;
  }

  g_object_set (src, "host", gst_uri_get_host (uri), NULL);
  g_object_set (src, "port", gst_uri_get_port (uri), NULL);

  userinfo = gst_uri_get_userinfo (uri);
  if (userinfo) {
    gchar **split = g_strsplit (userinfo, ":", 2);
    if (split && split[0] && split[1]) {
      gchar *pass = g_uri_unescape_string (split[1], NULL);
      g_object_set (src, "password", pass, NULL);
      g_free (pass);
    }
    g_strfreev (split);
  }

  GST_OBJECT_LOCK (src);
  if (src->uri)
    gst_uri_unref (src->uri);
  src->uri = gst_uri_ref (uri);
  GST_OBJECT_UNLOCK (src);

  gst_rfb_utils_set_properties_from_uri_query (G_OBJECT (src), uri);
  gst_uri_unref (uri);

  return TRUE;
}

static void
gst_rfb_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_rfb_src_uri_get_type;
  iface->get_protocols = gst_rfb_src_uri_get_protocols;
  iface->get_uri = gst_rfb_src_uri_get_uri;
  iface->set_uri = gst_rfb_src_uri_set_uri;
}

/* ------------------------------------------------------------------ */
/* Plugin registration                                                  */
/* ------------------------------------------------------------------ */

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (rfbsrc, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rfbsrc,
    "Connects to a VNC server and decodes RFB stream",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
