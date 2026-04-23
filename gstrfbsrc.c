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

#include <gst/video/video.h>

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_X11
#include <X11/Xlib.h>
#endif

enum
{
  PROP_0,
  PROP_HOST,
  PROP_PORT,
  PROP_USER,
  PROP_PASSWORD,
  PROP_OFFSET_X,
  PROP_OFFSET_Y,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_SHARED,
};

GST_DEBUG_CATEGORY_STATIC (rfbsrc_debug);
GST_DEBUG_CATEGORY (rfbdecoder_debug);
#define GST_CAT_DEFAULT rfbsrc_debug

static GstStaticPadTemplate gst_rfb_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("RGB")
        "; " GST_VIDEO_CAPS_MAKE ("BGR")
        "; " GST_VIDEO_CAPS_MAKE ("RGBx")
        "; " GST_VIDEO_CAPS_MAKE ("BGRx")
        "; " GST_VIDEO_CAPS_MAKE ("xRGB")
        "; " GST_VIDEO_CAPS_MAKE ("xBGR")));

/*
 * Unique address used as a key for rfbClientSetClientData / rfbClientGetClientData
 * to store the GstRfbSrc* pointer so auth callbacks can retrieve it.
 */
static gint rfb_src_client_data_key;

/* Forward declaration needed by stop() and finalize() which retrieve the
 * received_update pointer using process_update as the key. */
static void process_update (rfbClient * cl, int x, int y, int w, int h);

static void gst_rfb_src_finalize (GObject * object);
static void gst_rfb_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rfb_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_rfb_src_start (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_negotiate (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_stop (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_unlock (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_unlock_stop (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_event (GstBaseSrc * bsrc, GstEvent * event);
static gboolean gst_rfb_src_decide_allocation (GstBaseSrc * bsrc,
    GstQuery * query);
static GstFlowReturn gst_rfb_src_fill (GstPushSrc * psrc, GstBuffer * outbuf);
static void gst_rfb_got_cursor_shape (rfbClient * cl, int xhot, int yhot,
    int width, int height, int bpp);
static rfbBool gst_rfb_handle_cursor_pos (rfbClient * cl, int x, int y);

#define gst_rfb_src_parent_class parent_class
G_DEFINE_TYPE (GstRfbSrc, gst_rfb_src, GST_TYPE_PUSH_SRC);

/* ---------------------------------------------------------------------------
 * libvncclient log redirection
 *
 * rfbClientLog / rfbClientErr are global function pointers in libvncclient.
 * We replace them in plugin_init() so all VNC library output goes through
 * GStreamer's debug system under the "rfbdecoder" category, consistent with
 * how this plugin's own messages are handled.
 * --------------------------------------------------------------------------- */

static void
gst_rfb_vnc_log (const char *format, ...)
{
  va_list args;
  gchar *msg;

  va_start (args, format);
  msg = g_strdup_vprintf (format, args);
  va_end (args);

  g_strchomp (msg);
  GST_CAT_DEBUG (rfbdecoder_debug, "%s", msg);
  g_free (msg);
}

static void
gst_rfb_vnc_err (const char *format, ...)
{
  va_list args;
  gchar *msg;

  va_start (args, format);
  msg = g_strdup_vprintf (format, args);
  va_end (args);

  g_strchomp (msg);
  GST_CAT_WARNING (rfbdecoder_debug, "%s", msg);
  g_free (msg);
}

/* ---------------------------------------------------------------------------
 * Authentication callbacks
 *
 * libvncclient calls these during InitialiseRFBConnection() when the server
 * requires a password or username+password.  We retrieve the credentials
 * from the GstRfbSrc that was stored via rfbClientSetClientData().
 * --------------------------------------------------------------------------- */

static char *
gst_rfb_src_get_password (rfbClient * cl)
{
  GstRfbSrc *src = rfbClientGetClientData (cl, &rfb_src_client_data_key);
  return g_strdup (src->pass ? src->pass : "");
}

static rfbCredential *
gst_rfb_src_get_credential (rfbClient * cl, int credentialType)
{
  GstRfbSrc *src = rfbClientGetClientData (cl, &rfb_src_client_data_key);
  rfbCredential *cred;

  if (credentialType != rfbCredentialTypeUser) {
    GST_ELEMENT_ERROR (src, RESOURCE, NOT_AUTHORIZED,
        ("Unsupported VNC credential type %d", credentialType), (NULL));
    return NULL;
  }

  cred = g_new0 (rfbCredential, 1);
  cred->userCredential.username = g_strdup (src->user ? src->user : "");
  cred->userCredential.password = g_strdup (src->pass ? src->pass : "");
  return cred;
}

/* ---------------------------------------------------------------------------
 * Cursor callbacks (client-side software cursor blending)
 *
 * When the server supports cursor-shape encodings (XCursor / RichCursor),
 * it stops drawing the cursor into the framebuffer and instead sends shape
 * and position updates.  We store the received data and blend the cursor
 * onto every output frame in fill().  Both callbacks also increment
 * received_update so that fill()'s wait loop wakes up on cursor-only events.
 *
 * When the server does NOT support these encodings it draws the cursor
 * directly into the framebuffer and fires GotFrameBufferUpdate normally —
 * the fields below stay zero-initialised and the blending path is skipped.
 * --------------------------------------------------------------------------- */

static void
gst_rfb_got_cursor_shape (rfbClient * cl, int xhot, int yhot,
    int width, int height, int bpp)
{
  GstRfbSrc *src = rfbClientGetClientData (cl, &rfb_src_client_data_key);
  src->cursor_hot_x = xhot;
  src->cursor_hot_y = yhot;
  src->cursor_width = width;
  src->cursor_height = height;
  gint *received_update = rfbClientGetClientData (cl, process_update);
  g_atomic_int_inc (received_update);
}

static rfbBool
gst_rfb_handle_cursor_pos (rfbClient * cl, int x, int y)
{
  GstRfbSrc *src = rfbClientGetClientData (cl, &rfb_src_client_data_key);
  src->cursor_x = x;
  src->cursor_y = y;
  gint *received_update = rfbClientGetClientData (cl, process_update);
  g_atomic_int_inc (received_update);
  return TRUE;
}

/* ---------------------------------------------------------------------------
 * GObject / GstElement boilerplate
 * --------------------------------------------------------------------------- */

static void
gst_rfb_src_class_init (GstRfbSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstElementClass *gstelement_class;
  GstPushSrcClass *gstpushsrc_class;

  GST_DEBUG_CATEGORY_INIT (rfbsrc_debug, "rfbsrc", 0, "rfb src element");

  gobject_class = (GObjectClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_rfb_src_finalize;
  gobject_class->set_property = gst_rfb_src_set_property;
  gobject_class->get_property = gst_rfb_src_get_property;

  g_object_class_install_property (gobject_class, PROP_HOST,
      g_param_spec_string ("host", "Host to connect to", "Host to connect to",
          "127.0.0.1", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PORT,
      g_param_spec_int ("port", "Port", "Port",
          1, 65535, 5900, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_USER,
      g_param_spec_string ("user", "Username for authentication",
          "Username for authentication", "",
          G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PASSWORD,
      g_param_spec_string ("password", "Password for authentication",
          "Password for authentication", "",
          G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_OFFSET_X,
      g_param_spec_int ("offset-x", "x offset for screen scrapping",
          "x offset for screen scrapping", 0, 65535, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_OFFSET_Y,
      g_param_spec_int ("offset-y", "y offset for screen scrapping",
          "y offset for screen scrapping", 0, 65535, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_int ("width", "width of screen", "width of screen", 0, 65535,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_int ("height", "height of screen", "height of screen", 0,
          65535, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SHARED,
      g_param_spec_boolean ("shared", "Share desktop with other clients",
          "Share desktop with other clients", 1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_rfb_src_start);
  gstbasesrc_class->negotiate = GST_DEBUG_FUNCPTR (gst_rfb_src_negotiate);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_rfb_src_stop);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_rfb_src_unlock);
  gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_rfb_src_unlock_stop);
  gstbasesrc_class->event = GST_DEBUG_FUNCPTR (gst_rfb_src_event);
  gstpushsrc_class->fill = GST_DEBUG_FUNCPTR (gst_rfb_src_fill);
  gstbasesrc_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_rfb_src_decide_allocation);

  gstelement_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_rfb_src_template);

  gst_element_class_set_static_metadata (gstelement_class, "Rfb source",
      "Source/Video",
      "Creates a rfb video stream",
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

  /* Authoritative defaults; applied to decoder in start() */
  src->host = g_strdup ("127.0.0.1");
  src->port = 5900;
  src->shared = TRUE;
  /* offset_x, offset_y, width, height default to 0 — full framebuffer */
}

static void
gst_rfb_src_finalize (GObject * object)
{
  GstRfbSrc *src = GST_RFB_SRC (object);

  /* stop() should have been called by GStreamer before finalize, but guard. */
  if (src->decoder) {
    gint *received_update = rfbClientGetClientData (src->decoder, process_update);
    g_free (received_update);
    /* rfbClientCleanup() frees decoder->serverHost internally */
    rfbClientCleanup (src->decoder);
  }

  g_free (src->host);
  g_free (src->user);
  g_free (src->pass);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* ---------------------------------------------------------------------------
 * Property accessors
 *
 * All property values are stored in the GstRfbSrc struct so they can be set
 * before the element reaches PLAYING (i.e. before start() creates the decoder).
 * --------------------------------------------------------------------------- */

static void
gst_rfb_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRfbSrc *src = GST_RFB_SRC (object);

  switch (prop_id) {
    case PROP_HOST:
      g_free (src->host);
      src->host = g_strdup (g_value_get_string (value));
      break;
    case PROP_PORT:
      src->port = g_value_get_int (value);
      break;
    case PROP_USER:
      g_free (src->user);
      src->user = g_strdup (g_value_get_string (value));
      break;
    case PROP_PASSWORD:
      g_free (src->pass);
      src->pass = g_strdup (g_value_get_string (value));
      break;
    case PROP_OFFSET_X:
      src->offset_x = g_value_get_int (value);
      break;
    case PROP_OFFSET_Y:
      src->offset_y = g_value_get_int (value);
      break;
    case PROP_WIDTH:
      src->width = g_value_get_int (value);
      break;
    case PROP_HEIGHT:
      src->height = g_value_get_int (value);
      break;
    case PROP_SHARED:
      src->shared = g_value_get_boolean (value);
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
      g_value_set_int (value, src->width);
      break;
    case PROP_HEIGHT:
      g_value_set_int (value, src->height);
      break;
    case PROP_SHARED:
      g_value_set_boolean (value, src->shared);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* ---------------------------------------------------------------------------
 * Buffer pool allocation
 * --------------------------------------------------------------------------- */

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

  while (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    /* TODO We restrict to the exact size as we don't support strides or
     * special padding */
    if (size == info.size)
      break;

    gst_query_remove_nth_allocation_pool (query, 0);
    gst_object_unref (pool);
    pool = NULL;
  }

  if (pool == NULL) {
    /* we did not get a pool, make one ourselves then */
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

/* ---------------------------------------------------------------------------
 * Framebuffer update callback
 *
 * Called by libvncclient (from within HandleRFBServerMessage) each time the
 * server sends an updated rectangle.  We use an atomic counter as a simple
 * signal to fill() that new data is available.  Using g_atomic_int_inc avoids
 * the data race between the libvncclient call site and fill()'s reader.
 * --------------------------------------------------------------------------- */

static void
process_update (rfbClient * cl, int x, int y, int w, int h)
{
  gint *received_update = rfbClientGetClientData (cl, process_update);
  g_atomic_int_inc (received_update);
}

/* ---------------------------------------------------------------------------
 * GstBaseSrc vmethod implementations
 * --------------------------------------------------------------------------- */

static gboolean
gst_rfb_src_unlock (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);
  g_atomic_int_set (&src->unlocked, 1);
  return TRUE;
}

static gboolean
gst_rfb_src_unlock_stop (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);
  g_atomic_int_set (&src->unlocked, 0);
  return TRUE;
}

/*
 * start() — called on NULL→READY.
 *
 * Creates a fresh rfbClient from the stored property values.  This also makes
 * stop()+start() cycles safe: stop() destroys the decoder, start() recreates
 * it from the struct fields that survived.
 */
static gboolean
gst_rfb_src_start (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  /* 8 bits/sample, 3 samples (RGB), 4 bytes/pixel */
  src->decoder = rfbGetClient (8, 3, 4);
  src->decoder->serverHost = g_strdup (src->host);
  src->decoder->serverPort = src->port;
  src->decoder->canHandleNewFBSize = 0;
  src->decoder->appData.useRemoteCursor = 1;
  src->decoder->appData.shareDesktop = src->shared;

  /* Override auth callbacks immediately after rfbGetClient() so the library's
   * default (which may call getpass() on some versions) is never invoked. */
  rfbClientSetClientData (src->decoder, &rfb_src_client_data_key, src);
  src->decoder->GetPassword = gst_rfb_src_get_password;
  src->decoder->GetCredential = gst_rfb_src_get_credential;

  /* Request cursor shape/position encodings.  The server blends the cursor
   * into the framebuffer only when it doesn't support these encodings, so
   * we implement software blending ourselves as a complement. */
  src->decoder->appData.useRemoteCursor = 1;
  src->decoder->GotCursorShape = gst_rfb_got_cursor_shape;
  src->decoder->HandleCursorPos = gst_rfb_handle_cursor_pos;

  /* Reset software-cursor state for clean start / restart. */
  src->cursor_x = src->cursor_y = 0;
  src->cursor_hot_x = src->cursor_hot_y = 0;
  src->cursor_width = src->cursor_height = 0;

  /* Apply capture region only if the user set an explicit size.
   * If not, updateRect stays zero and negotiate() will default to full frame. */
  if (src->width > 0 && src->height > 0) {
    src->decoder->updateRect.x = src->offset_x;
    src->decoder->updateRect.y = src->offset_y;
    src->decoder->updateRect.w = src->width;
    src->decoder->updateRect.h = src->height;
  }

  return TRUE;
}

/*
 * negotiate() — called on READY→PAUSED.
 *
 * Connects to the VNC server, performs the RFB handshake (including
 * authentication), allocates the framebuffer, detects the pixel format, and
 * advertises the resulting GstCaps downstream.
 */
static gboolean
gst_rfb_src_negotiate (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);
  rfbClient *decoder;
  GstCaps *caps;
  GstVideoInfo vinfo;
  GstVideoFormat vformat;
  guint32 red_mask, green_mask, blue_mask;
  gchar *stream_id = NULL;
  GstEvent *stream_start = NULL;

  decoder = src->decoder;

  if (decoder->sock >= 0)
    return TRUE;

  GST_DEBUG_OBJECT (src, "connecting to host %s on port %d",
      decoder->serverHost, decoder->serverPort);

  if (!ConnectToRFBServer (decoder, decoder->serverHost, decoder->serverPort)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("Could not connect to VNC server %s on port %d",
            decoder->serverHost, decoder->serverPort), (NULL));
    return FALSE;
  }

  if (!InitialiseRFBConnection (decoder)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("Failed to setup VNC connection to host %s on port %d",
            decoder->serverHost, decoder->serverPort), (NULL));
    return FALSE;
  }

  decoder->width = decoder->si.framebufferWidth;
  decoder->height = decoder->si.framebufferHeight;
  if (!decoder->MallocFrameBuffer (decoder)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("Failed to allocate VNC framebuffer for host %s on port %d",
            decoder->serverHost, decoder->serverPort), (NULL));
    return FALSE;
  }

  /* Default to the full server framebuffer if no explicit region was set.
   * The original code used (x < 0) as a sentinel, but rfbRectangle fields are
   * unsigned so that check never triggered.  We test w/h == 0 instead, which
   * is the zero-initialised default from rfbGetClient. */
  if (decoder->updateRect.w == 0 || decoder->updateRect.h == 0) {
    decoder->updateRect.x = 0;
    decoder->updateRect.y = 0;
    decoder->updateRect.w = decoder->width;
    decoder->updateRect.h = decoder->height;
  }

  stream_id = gst_pad_create_stream_id_printf (GST_BASE_SRC_PAD (bsrc),
      GST_ELEMENT (src), "%s:%d", decoder->serverHost, decoder->serverPort);
  stream_start = gst_event_new_stream_start (stream_id);
  g_free (stream_id);
  gst_pad_push_event (GST_BASE_SRC_PAD (bsrc), stream_start);

  GST_DEBUG_OBJECT (src, "setting caps width to %d and height to %d",
      decoder->updateRect.w, decoder->updateRect.h);

  red_mask = decoder->si.format.redMax << decoder->si.format.redShift;
  green_mask = decoder->si.format.greenMax << decoder->si.format.greenShift;
  blue_mask = decoder->si.format.blueMax << decoder->si.format.blueShift;

  vformat = gst_video_format_from_masks (decoder->si.format.depth,
      decoder->si.format.bitsPerPixel,
      decoder->si.format.bigEndian ? G_BIG_ENDIAN : G_LITTLE_ENDIAN,
      red_mask, green_mask, blue_mask, 0);
  /* Lock in the server's native pixel format so fill() knows how to copy */
  decoder->format = decoder->si.format;

  gst_video_info_init (&vinfo);
  gst_video_info_set_format (&vinfo, vformat,
      decoder->updateRect.w, decoder->updateRect.h);
  caps = gst_video_info_to_caps (&vinfo);
  gst_base_src_set_caps (bsrc, caps);
  gst_caps_unref (caps);

  if (!SetFormatAndEncodings (decoder)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("Failed to set format/encodings for host %s on port %d",
            decoder->serverHost, decoder->serverPort), (NULL));
    return FALSE;
  }

  /* Allocate the atomic update counter and register the frame callback */
  gint *received_update = g_new0 (gint, 1);
  rfbClientSetClientData (decoder, process_update, received_update);
  decoder->GotFrameBufferUpdate = process_update;

  /* Prime the pump: ask the server for the first full frame */
  SendFramebufferUpdateRequest (decoder,
      decoder->updateRect.x, decoder->updateRect.y,
      decoder->updateRect.w, decoder->updateRect.h,
      FALSE);

  return TRUE;
}

/*
 * stop() — called on PAUSED→READY (and on error paths).
 *
 * Frees all resources allocated in start()/negotiate().  After this returns,
 * src->decoder is NULL; a subsequent start() will recreate it cleanly.
 */
static gboolean
gst_rfb_src_stop (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  /* Free the update counter allocated in negotiate() before the decoder
   * is destroyed — rfbClientCleanup() does not know about our client data. */
  gint *received_update = rfbClientGetClientData (src->decoder, process_update);
  g_free (received_update);

  /* rfbClientCleanup() closes the socket and frees decoder->serverHost */
  rfbClientCleanup (src->decoder);
  src->decoder = NULL;

  return TRUE;
}

/*
 * fill() — called repeatedly by GstPushSrc to produce video buffers.
 *
 * Waits for libvncclient to signal that a new frame is available (via the
 * process_update callback), then copies the capture region out of the shared
 * framebuffer into the output GstBuffer and requests the next incremental
 * update from the server.
 */
static GstFlowReturn
gst_rfb_src_fill (GstPushSrc * psrc, GstBuffer * outbuf)
{
  GstRfbSrc *src = GST_RFB_SRC (psrc);
  rfbClient *decoder = src->decoder;
  GstMapInfo info;
  int bpp;
  gsize expected_size;
  int i;

  /* Poll until the server sends an update.  WaitForMessage() blocks for up to
   * 50 ms per call, HandleRFBServerMessage() processes the incoming data and
   * triggers process_update(), which atomically increments the counter. */
  gint *received_update = rfbClientGetClientData (decoder, process_update);
  while (!g_atomic_int_get (received_update)) {
    if (g_atomic_int_get (&src->unlocked))
      return GST_FLOW_FLUSHING;
    int ret = WaitForMessage (decoder, 50000);
    if (ret < 0 || (ret > 0 && !HandleRFBServerMessage (decoder))) {
      GST_ELEMENT_ERROR (src, RESOURCE, READ,
          ("Error on VNC connection to host %s on port %d",
              decoder->serverHost, decoder->serverPort), (NULL));
      return GST_FLOW_ERROR;
    }
  }
  g_atomic_int_set (received_update, 0);

  if (!gst_buffer_map (outbuf, &info, GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Could not map the output frame"), (NULL));
    return GST_FLOW_ERROR;
  }

  bpp = decoder->format.bitsPerPixel / 8;
  expected_size = (gsize) decoder->updateRect.w * decoder->updateRect.h * bpp;
  if (info.size < expected_size) {
    gst_buffer_unmap (outbuf, &info);
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Output buffer too small: have %" G_GSIZE_FORMAT
            ", need %" G_GSIZE_FORMAT, info.size, expected_size), (NULL));
    return GST_FLOW_ERROR;
  }

  /* Copy row-by-row from the capture sub-region of the full framebuffer */
  for (i = 0; i < decoder->updateRect.h; i++) {
    memcpy (&info.data[i * decoder->updateRect.w * bpp],
        &decoder->frameBuffer[bpp * (decoder->updateRect.x +
                (decoder->updateRect.y + i) * decoder->width)],
        decoder->updateRect.w * bpp);
  }

  /* Software cursor blending: active only when the server sent cursor-shape
   * encodings (rcSource non-NULL).  When the server drew the cursor into the
   * framebuffer itself this block is skipped. */
  if (src->cursor_width > 0 && src->cursor_height > 0 && decoder->rcSource
      && decoder->rcMask) {
    int mask_row_bytes = (src->cursor_width + 7) / 8;
    /* Top-left corner of cursor in the capture-rect coordinate system */
    int cx = src->cursor_x - src->cursor_hot_x - decoder->updateRect.x;
    int cy = src->cursor_y - src->cursor_hot_y - decoder->updateRect.y;

    for (int row = 0; row < src->cursor_height; row++) {
      int fy = cy + row;
      if (fy < 0 || fy >= decoder->updateRect.h)
        continue;
      for (int col = 0; col < src->cursor_width; col++) {
        int fx = cx + col;
        if (fx < 0 || fx >= decoder->updateRect.w)
          continue;
        /* Skip transparent pixels (1-bit mask, MSB first) */
        if (!(decoder->rcMask[row * mask_row_bytes + col / 8] & (0x80 >> (col % 8))))
          continue;
        memcpy (info.data + (fy * decoder->updateRect.w + fx) * bpp,
            decoder->rcSource + (row * src->cursor_width + col) * bpp, bpp);
      }
    }
  }

  GST_BUFFER_PTS (outbuf) =
      gst_clock_get_time (GST_ELEMENT_CLOCK (src)) -
      GST_ELEMENT_CAST (src)->base_time;

  gst_buffer_unmap (outbuf, &info);

  /* Ask the server for the next incremental update so the loop keeps running.
   * Without this, RFC-compliant servers send nothing after the first frame. */
  SendFramebufferUpdateRequest (decoder,
      decoder->updateRect.x, decoder->updateRect.y,
      decoder->updateRect.w, decoder->updateRect.h,
      TRUE);                    /* TRUE = incremental */

  return GST_FLOW_OK;
}

static gboolean
gst_rfb_src_event (GstBaseSrc * bsrc, GstEvent * event)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
      /* After flushing, request a full non-incremental frame so the first
       * buffer pushed after the flush is complete. */
      if (src->decoder && src->decoder->sock >= 0) {
        SendFramebufferUpdateRequest (src->decoder,
            src->decoder->updateRect.x, src->decoder->updateRect.y,
            src->decoder->updateRect.w, src->decoder->updateRect.h,
            FALSE);
      }
      break;
    default:
      break;
  }

  return GST_BASE_SRC_CLASS (parent_class)->event (bsrc, event);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  /* Initialise the decoder debug category here so the VNC log callbacks can
   * use it immediately — before any element instance is created. */
  GST_DEBUG_CATEGORY_INIT (rfbdecoder_debug, "rfbdecoder", 0, "rfb decoder");

  /* Redirect libvncclient's global log/error output through GStreamer's debug
   * system.  This makes VNC protocol-level messages visible under the same
   * GST_DEBUG infrastructure as the rest of the plugin. */
  rfbClientLog = gst_rfb_vnc_log;
  rfbClientErr = gst_rfb_vnc_err;

  return gst_element_register (plugin, "rfbsrc", GST_RANK_NONE,
      GST_TYPE_RFB_SRC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rfbsrc,
    "Connects to a VNC server and decodes RFB stream",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
