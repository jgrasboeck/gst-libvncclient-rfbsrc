/* GStreamer
 * Copyright (C) <2026> Autonoma
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

#include <gst/video/navigation.h>

/* LibVNCClient defines TRUE/FALSE itself. Undef GLib macros first to avoid
 * noisy macro-redefinition warnings, then restore GLib-compatible values. */
#ifdef TRUE
#undef TRUE
#endif
#ifdef FALSE
#undef FALSE
#endif
#include <rfb/rfbclient.h>
#include <rfb/keysym.h>
#ifdef TRUE
#undef TRUE
#endif
#ifdef FALSE
#undef FALSE
#endif
#define FALSE (0)
#define TRUE (!FALSE)

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define DEFAULT_PROP_HOST             "127.0.0.1"
#define DEFAULT_PROP_PORT             5900
#define DEFAULT_PROP_URI              "rfb://"DEFAULT_PROP_HOST":"G_STRINGIFY(DEFAULT_PROP_PORT)
#define DEFAULT_PROP_VERSION          "auto"
#define DEFAULT_PROP_ENCODINGS        "tight zrle hextile raw"
#define DEFAULT_PROP_MAX_FRAMERATE    30
#define DEFAULT_PROP_FRAME_TIMEOUT_MS 1000
#define DEFAULT_PROP_CONNECT_TIMEOUT  10
#define DEFAULT_PROP_READ_TIMEOUT     10
#define DEFAULT_PROP_CURSOR_MODE      GST_RFB_SRC_CURSOR_MODE_AUTO
#define DEFAULT_CURSOR_FALLBACK_FRAMES 30

#define GST_RFB_TRUE  ((rfbBool) -1)
#define GST_RFB_FALSE ((rfbBool) 0)

enum
{
  PROP_0,
  PROP_URI,
  PROP_HOST,
  PROP_PORT,
  PROP_USERNAME,
  PROP_VERSION,
  PROP_PASSWORD,
  PROP_OFFSET_X,
  PROP_OFFSET_Y,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_INCREMENTAL,
  PROP_USE_COPYRECT,
  PROP_ENCODINGS,
  PROP_SHARED,
  PROP_VIEWONLY,
  PROP_CURSOR_MODE,
  PROP_MAX_FRAMERATE,
  PROP_FRAME_TIMEOUT_MS,
  PROP_CONNECT_TIMEOUT,
  PROP_READ_TIMEOUT
};

enum
{
  SIGNAL_SEND_KEY,
  SIGNAL_SEND_KEY_NAME,
  SIGNAL_SEND_POINTER,
  SIGNAL_SEND_MOUSE_BUTTON,
  SIGNAL_SEND_TEXT,
  SIGNAL_SEND_CLIPBOARD,
  SIGNAL_LAST
};

static guint gst_rfb_src_signals[SIGNAL_LAST];
static gint gst_rfb_src_client_data_tag;

GST_DEBUG_CATEGORY_STATIC (rfbsrc_debug);
#define GST_CAT_DEFAULT rfbsrc_debug

static gchar *
gst_rfb_src_format_libvnc_log (const char *format, va_list args)
{
  gchar *message;

  message = g_strdup_vprintf (format, args);
  if (message)
    g_strchomp (message);

  return message;
}

static void
gst_rfb_src_libvnc_log (const char *format, ...)
{
  va_list args;
  gchar *message;

  va_start (args, format);
  message = gst_rfb_src_format_libvnc_log (format, args);
  va_end (args);

  if (message && *message)
    GST_CAT_DEBUG (rfbsrc_debug, "%s", message);

  g_free (message);
}

static void
gst_rfb_src_libvnc_err (const char *format, ...)
{
  va_list args;
  gchar *message;

  va_start (args, format);
  message = gst_rfb_src_format_libvnc_log (format, args);
  va_end (args);

  if (message && *message)
    GST_CAT_WARNING (rfbsrc_debug, "%s", message);

  g_free (message);
}

static void
gst_rfb_src_install_libvnc_logging (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    rfbClientLog = gst_rfb_src_libvnc_log;
    rfbClientErr = gst_rfb_src_libvnc_err;
    rfbEnableClientLogging = TRUE;
    g_once_init_leave (&initialized, 1);
  }
}

static GType
gst_rfb_src_cursor_mode_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id)) {
    static const GEnumValue values[] = {
      {GST_RFB_SRC_CURSOR_MODE_AUTO, "Auto", "auto"},
      {GST_RFB_SRC_CURSOR_MODE_CLIENT, "Client-side", "client"},
      {GST_RFB_SRC_CURSOR_MODE_SERVER, "Server-side", "server"},
      {GST_RFB_SRC_CURSOR_MODE_NONE, "None", "none"},
      {0, NULL, NULL}
    };
    GType tmp = g_enum_register_static ("GstRfbSrcCursorMode", values);

    g_once_init_leave (&type_id, tmp);
  }

  return type_id;
}

#define GST_TYPE_RFB_SRC_CURSOR_MODE (gst_rfb_src_cursor_mode_get_type ())

static GstStaticPadTemplate gst_rfb_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("BGRx")));

static void gst_rfb_src_finalize (GObject * object);
static void gst_rfb_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rfb_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_rfb_src_start (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_stop (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_negotiate (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_event (GstBaseSrc * bsrc, GstEvent * event);
static gboolean gst_rfb_src_unlock (GstBaseSrc * bsrc);
static gboolean gst_rfb_src_unlock_stop (GstBaseSrc * bsrc);
static GstFlowReturn gst_rfb_src_create (GstPushSrc * psrc,
    GstBuffer ** outbuf);

static void gst_rfb_src_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static gboolean
gst_rfb_src_uri_set_uri (GstURIHandler * handler, const gchar * uri,
    GError ** error);

static gboolean gst_rfb_src_signal_send_key (GstRfbSrc * src, guint keysym,
    gboolean down);
static gboolean gst_rfb_src_signal_send_key_name (GstRfbSrc * src,
    const gchar * key, gboolean down);
static gboolean gst_rfb_src_signal_send_pointer (GstRfbSrc * src, gint x,
    gint y, guint button_mask);
static gboolean gst_rfb_src_signal_send_mouse_button (GstRfbSrc * src,
    gint button, gboolean down, gint x, gint y);
static gboolean gst_rfb_src_signal_send_text (GstRfbSrc * src,
    const gchar * text);
static gboolean gst_rfb_src_signal_send_clipboard (GstRfbSrc * src,
    const gchar * text);

#define gst_rfb_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstRfbSrc, gst_rfb_src, GST_TYPE_PUSH_SRC,
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER, gst_rfb_src_uri_handler_init));
GST_ELEMENT_REGISTER_DEFINE (rfbsrc, "rfbsrc", GST_RANK_NONE, GST_TYPE_RFB_SRC);

static gboolean
gst_rfb_src_is_running (GstRfbSrc * src)
{
  return GST_STATE (src) >= GST_STATE_PAUSED;
}

static gboolean
gst_rfb_src_is_unlocked (GstRfbSrc * src)
{
  return g_atomic_int_get (&src->unlocked) != 0;
}

static void
gst_rfb_src_set_unlocked (GstRfbSrc * src, gboolean unlocked)
{
  g_atomic_int_set (&src->unlocked, unlocked ? 1 : 0);
}

static void
gst_rfb_src_replace_string (gchar ** target, const gchar * value)
{
  g_free (*target);
  *target = g_strdup (value);
}

static gpointer
gst_rfb_src_memdup (gconstpointer data, gsize size)
{
  gpointer copy;

  if (size == 0)
    return NULL;

  copy = g_malloc (size);
  memcpy (copy, data, size);

  return copy;
}

static gboolean
gst_rfb_src_set_ready_string (GstRfbSrc * src, gchar ** target,
    const gchar * value, const gchar * property_name)
{
  if (gst_rfb_src_is_running (src)) {
    GST_WARNING_OBJECT (src, "Can only change %s in NULL or READY state",
        property_name);
    return FALSE;
  }

  gst_rfb_src_replace_string (target, value);
  return TRUE;
}

static guint
gst_rfb_src_unicode_to_keysym (gunichar ch)
{
  switch (ch) {
    case '\b':
      return XK_BackSpace;
    case '\t':
      return XK_Tab;
    case '\n':
    case '\r':
      return XK_Return;
    case 0x7f:
      return XK_Delete;
    default:
      break;
  }

  if (ch <= 0xff)
    return ch;

  return 0x01000000 | ch;
}

typedef struct
{
  const gchar *name;
  guint keysym;
} GstRfbKeyName;

static const GstRfbKeyName gst_rfb_key_names[] = {
  {"BackSpace", XK_BackSpace},
  {"Backspace", XK_BackSpace},
  {"Tab", XK_Tab},
  {"ISO_Left_Tab", XK_Tab},
  {"Return", XK_Return},
  {"Enter", XK_Return},
  {"Escape", XK_Escape},
  {"Esc", XK_Escape},
  {"Delete", XK_Delete},
  {"Del", XK_Delete},
  {"Insert", XK_Insert},
  {"Ins", XK_Insert},
  {"Home", XK_Home},
  {"End", XK_End},
  {"Page_Up", XK_Page_Up},
  {"PageUp", XK_Page_Up},
  {"Prior", XK_Page_Up},
  {"Page_Down", XK_Page_Down},
  {"PageDown", XK_Page_Down},
  {"Next", XK_Page_Down},
  {"Left", XK_Left},
  {"Right", XK_Right},
  {"Up", XK_Up},
  {"Down", XK_Down},
  {"Space", XK_space},
  {"space", XK_space},
  {"Shift_L", XK_Shift_L},
  {"Shift_R", XK_Shift_R},
  {"Control_L", XK_Control_L},
  {"Control_R", XK_Control_R},
  {"Ctrl_L", XK_Control_L},
  {"Ctrl_R", XK_Control_R},
  {"Alt_L", XK_Alt_L},
  {"Alt_R", XK_Alt_R},
  {"Meta_L", XK_Meta_L},
  {"Meta_R", XK_Meta_R},
  {"Super_L", XK_Super_L},
  {"Super_R", XK_Super_R},
  {"Caps_Lock", XK_Caps_Lock},
  {"Num_Lock", XK_Num_Lock},
  {"Scroll_Lock", XK_Scroll_Lock},
  {"Menu", XK_Menu},
  {"Pause", XK_Pause},
  {"Print", XK_Print},
  {"Sys_Req", XK_Sys_Req},
  {"KP_Enter", XK_KP_Enter},
  {"KP_Add", XK_KP_Add},
  {"KP_Subtract", XK_KP_Subtract},
  {"KP_Multiply", XK_KP_Multiply},
  {"KP_Divide", XK_KP_Divide},
  {"KP_Decimal", XK_KP_Decimal},
  {"KP_0", XK_KP_0},
  {"KP_1", XK_KP_1},
  {"KP_2", XK_KP_2},
  {"KP_3", XK_KP_3},
  {"KP_4", XK_KP_4},
  {"KP_5", XK_KP_5},
  {"KP_6", XK_KP_6},
  {"KP_7", XK_KP_7},
  {"KP_8", XK_KP_8},
  {"KP_9", XK_KP_9},
};

static guint
gst_rfb_src_keysym_from_name (const gchar * key)
{
  gchar *end = NULL;
  guint64 parsed;
  guint i;

  if (key == NULL || *key == '\0')
    return 0;

  if (g_utf8_strlen (key, -1) == 1)
    return gst_rfb_src_unicode_to_keysym (g_utf8_get_char (key));

  if (g_str_has_prefix (key, "0x") || g_str_has_prefix (key, "0X")) {
    parsed = g_ascii_strtoull (key + 2, &end, 16);
    if (end != key + 2 && *end == '\0' && parsed <= G_MAXUINT)
      return (guint) parsed;
  }

  if (g_str_has_prefix (key, "U+") || g_str_has_prefix (key, "u+")) {
    parsed = g_ascii_strtoull (key + 2, &end, 16);
    if (end != key + 2 && *end == '\0' && parsed <= 0x10ffff)
      return gst_rfb_src_unicode_to_keysym ((gunichar) parsed);
  }

  if ((key[0] == 'F' || key[0] == 'f') && g_ascii_isdigit (key[1])) {
    parsed = g_ascii_strtoull (key + 1, &end, 10);
    if (end != key + 1 && *end == '\0' && parsed >= 1 && parsed <= 35)
      return XK_F1 + parsed - 1;
  }

  for (i = 0; i < G_N_ELEMENTS (gst_rfb_key_names); i++) {
    if (g_ascii_strcasecmp (key, gst_rfb_key_names[i].name) == 0)
      return gst_rfb_key_names[i].keysym;
  }

  return 0;
}

static GstRfbSrc *
gst_rfb_src_from_client (rfbClient * client)
{
  return rfbClientGetClientData (client, &gst_rfb_src_client_data_tag);
}

static char *
gst_rfb_src_get_password (rfbClient * client)
{
  GstRfbSrc *src = gst_rfb_src_from_client (client);

  if (src == NULL || src->password == NULL)
    return strdup ("");

  return strdup (src->password);
}

static rfbCredential *
gst_rfb_src_get_credential (rfbClient * client, int credential_type)
{
  GstRfbSrc *src = gst_rfb_src_from_client (client);
  rfbCredential *credential;

  if (src == NULL || credential_type != rfbCredentialTypeUser)
    return NULL;

  credential = calloc (1, sizeof (*credential));
  if (credential == NULL)
    return NULL;

  credential->userCredential.username =
      strdup (src->username ? src->username : "");
  credential->userCredential.password =
      strdup (src->password ? src->password : "");

  if (credential->userCredential.username == NULL ||
      credential->userCredential.password == NULL) {
    free (credential->userCredential.username);
    free (credential->userCredential.password);
    free (credential);
    return NULL;
  }

  return credential;
}

static rfbBool
gst_rfb_src_malloc_framebuffer (rfbClient * client)
{
  GstRfbSrc *src = gst_rfb_src_from_client (client);
  guint64 size;
  guint8 *framebuffer;

  if (client->width <= 0 || client->height <= 0)
    return GST_RFB_FALSE;

  size = (guint64) client->width * client->height * 4;
  if (size > G_MAXSIZE)
    return GST_RFB_FALSE;

  framebuffer = calloc (1, (size_t) size);
  if (framebuffer == NULL)
    return GST_RFB_FALSE;

  free (client->frameBuffer);
  client->frameBuffer = framebuffer;

  if (src) {
    src->server_width = client->width;
    src->server_height = client->height;
    src->geometry_changed = TRUE;
    src->frame_valid = FALSE;
    src->frame_dirty = FALSE;
  }

  return GST_RFB_TRUE;
}

static void
gst_rfb_src_got_framebuffer_update (rfbClient * client, int x, int y,
    int width, int height)
{
  GstRfbSrc *src = gst_rfb_src_from_client (client);

  if (src == NULL)
    return;

  GST_LOG_OBJECT (src, "framebuffer update x=%d y=%d width=%d height=%d",
      x, y, width, height);
  src->frame_dirty = TRUE;
  src->frame_valid = TRUE;
  src->update_request_pending = FALSE;
}

static void
gst_rfb_src_finished_framebuffer_update (rfbClient * client)
{
  GstRfbSrc *src = gst_rfb_src_from_client (client);

  if (src == NULL)
    return;

  src->frame_dirty = TRUE;
  src->frame_valid = TRUE;
  src->update_request_pending = FALSE;
}

static void
gst_rfb_src_clear_cursor (GstRfbSrc * src)
{
  g_clear_pointer (&src->cursor_source, g_free);
  g_clear_pointer (&src->cursor_mask, g_free);
  src->cursor_source_size = 0;
  src->cursor_mask_size = 0;
  src->cursor_shape_valid = FALSE;
  src->cursor_position_valid = FALSE;
  src->cursor_client_requested = FALSE;
  src->cursor_auto_fallback_frames = 0;
  src->cursor_x = 0;
  src->cursor_y = 0;
  src->cursor_hot_x = 0;
  src->cursor_hot_y = 0;
  src->cursor_width = 0;
  src->cursor_height = 0;
  src->cursor_bpp = 0;
}

static rfbBool
gst_rfb_src_handle_cursor_pos (rfbClient * client, int x, int y)
{
  GstRfbSrc *src = gst_rfb_src_from_client (client);

  if (src == NULL)
    return GST_RFB_TRUE;

  src->cursor_x = x;
  src->cursor_y = y;
  src->cursor_position_valid = TRUE;
  src->frame_dirty = TRUE;

  GST_LOG_OBJECT (src, "cursor position x=%d y=%d", x, y);

  return GST_RFB_TRUE;
}

static void
gst_rfb_src_got_cursor_shape (rfbClient * client, int xhot, int yhot,
    int width, int height, int bytes_per_pixel)
{
  GstRfbSrc *src = gst_rfb_src_from_client (client);
  guint64 source_size;
  guint64 mask_size;

  if (src == NULL)
    return;

  if (width <= 0 || height <= 0 || bytes_per_pixel <= 0 ||
      bytes_per_pixel > 4) {
    GST_WARNING_OBJECT (src, "ignoring invalid cursor shape %dx%d bpp=%d",
        width, height, bytes_per_pixel);
    src->cursor_shape_valid = FALSE;
    return;
  }

  source_size = (guint64) width * height * bytes_per_pixel;
  mask_size = ((guint64) width + 7) / 8 * height;
  if (source_size > G_MAXSIZE || mask_size > G_MAXSIZE ||
      client->rcSource == NULL) {
    GST_WARNING_OBJECT (src, "ignoring cursor shape with invalid buffers");
    src->cursor_shape_valid = FALSE;
    return;
  }

  g_free (src->cursor_source);
  g_free (src->cursor_mask);
  src->cursor_source = gst_rfb_src_memdup (client->rcSource,
      (gsize) source_size);
  src->cursor_mask = client->rcMask ?
      gst_rfb_src_memdup (client->rcMask, (gsize) mask_size) : NULL;
  src->cursor_source_size = (gsize) source_size;
  src->cursor_mask_size = client->rcMask ? (gsize) mask_size : 0;

  if (src->cursor_source == NULL ||
      (client->rcMask != NULL && src->cursor_mask == NULL)) {
    GST_WARNING_OBJECT (src, "could not copy cursor shape");
    src->cursor_shape_valid = FALSE;
    return;
  }

  src->cursor_hot_x = xhot;
  src->cursor_hot_y = yhot;
  src->cursor_width = width;
  src->cursor_height = height;
  src->cursor_bpp = bytes_per_pixel;
  src->cursor_shape_valid = TRUE;
  src->cursor_auto_fallback_frames = 0;
  src->frame_dirty = TRUE;

  GST_DEBUG_OBJECT (src, "cursor shape %dx%d hot=%d,%d bpp=%d", width,
      height, xhot, yhot, bytes_per_pixel);
}

static gboolean
gst_rfb_src_compute_output_rect (GstRfbSrc * src, gint * x, gint * y,
    gint * width, gint * height)
{
  gint max_width;
  gint max_height;

  if (src->server_width <= 0 || src->server_height <= 0) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("VNC server reported invalid framebuffer size %dx%d",
            src->server_width, src->server_height), (NULL));
    return FALSE;
  }

  if (src->offset_x < 0 || src->offset_x >= src->server_width ||
      src->offset_y < 0 || src->offset_y >= src->server_height) {
    GST_ELEMENT_ERROR (src, RESOURCE, SETTINGS,
        ("Capture offset %d,%d outside VNC framebuffer %dx%d",
            src->offset_x, src->offset_y, src->server_width,
            src->server_height), (NULL));
    return FALSE;
  }

  max_width = src->server_width - src->offset_x;
  max_height = src->server_height - src->offset_y;

  *x = src->offset_x;
  *y = src->offset_y;
  *width = src->requested_width > 0 ? src->requested_width : max_width;
  *height = src->requested_height > 0 ? src->requested_height : max_height;

  if (*width > max_width) {
    GST_WARNING_OBJECT (src, "Requested width %d exceeds server area, "
        "clamping to %d", *width, max_width);
    *width = max_width;
  }
  if (*height > max_height) {
    GST_WARNING_OBJECT (src, "Requested height %d exceeds server area, "
        "clamping to %d", *height, max_height);
    *height = max_height;
  }

  return *width > 0 && *height > 0;
}

static gboolean
gst_rfb_src_update_caps (GstRfbSrc * src)
{
  GstCaps *caps;
  gint x, y, width, height;

  if (!gst_rfb_src_compute_output_rect (src, &x, &y, &width, &height))
    return FALSE;

  if (src->have_caps && src->output_x == x && src->output_y == y &&
      src->output_width == width && src->output_height == height)
    return TRUE;

  gst_video_info_set_format (&src->vinfo, GST_VIDEO_FORMAT_BGRx, width, height);
  GST_VIDEO_INFO_FPS_N (&src->vinfo) = src->max_framerate;
  GST_VIDEO_INFO_FPS_D (&src->vinfo) = 1;

  caps = gst_video_info_to_caps (&src->vinfo);
  if (caps == NULL)
    return FALSE;

  GST_DEBUG_OBJECT (src, "setting caps %" GST_PTR_FORMAT, caps);

  if (!gst_base_src_set_caps (GST_BASE_SRC (src), caps)) {
    gst_caps_unref (caps);
    return FALSE;
  }
  gst_caps_unref (caps);

  src->output_x = x;
  src->output_y = y;
  src->output_width = width;
  src->output_height = height;
  src->frame_duration =
      gst_util_uint64_scale_int (GST_SECOND, 1, src->max_framerate);
  src->have_caps = TRUE;

  return TRUE;
}

static void
gst_rfb_src_sync_client_update_rect (GstRfbSrc * src)
{
  if (src->client == NULL)
    return;

  src->client->updateRect.x = src->output_x;
  src->client->updateRect.y = src->output_y;
  src->client->updateRect.w = src->output_width;
  src->client->updateRect.h = src->output_height;
  src->client->isUpdateRectManagedByLib = GST_RFB_FALSE;
}

static gboolean
gst_rfb_src_open (GstRfbSrc * src)
{
  rfbClient *client;
  const gchar *encodings;

  if (src->connected)
    return TRUE;

  g_return_val_if_fail (src->client == NULL, FALSE);

  if (src->host == NULL || *src->host == '\0') {
    GST_ELEMENT_ERROR (src, RESOURCE, SETTINGS,
        ("VNC host is empty"), (NULL));
    return FALSE;
  }

  client = rfbGetClient (8, 3, 4);
  if (client == NULL) {
    GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
        ("Could not allocate libvncclient client"), (NULL));
    return FALSE;
  }

  src->client = client;
  gst_rfb_src_clear_cursor (src);
  src->cursor_client_requested =
      src->cursor_mode != GST_RFB_SRC_CURSOR_MODE_SERVER;

  rfbClientSetClientData (client, &gst_rfb_src_client_data_tag, src);
  client->MallocFrameBuffer = gst_rfb_src_malloc_framebuffer;
  client->GotFrameBufferUpdate = gst_rfb_src_got_framebuffer_update;
  client->FinishedFrameBufferUpdate = gst_rfb_src_finished_framebuffer_update;
  client->GetPassword = gst_rfb_src_get_password;
  client->GetCredential = gst_rfb_src_get_credential;
  client->HandleCursorPos = gst_rfb_src_handle_cursor_pos;
  client->GotCursorShape = gst_rfb_src_got_cursor_shape;

  client->appData.shareDesktop = src->shared ? GST_RFB_TRUE : GST_RFB_FALSE;
  client->appData.viewOnly = src->view_only ? GST_RFB_TRUE : GST_RFB_FALSE;
  client->appData.forceTrueColour = GST_RFB_TRUE;
  client->appData.requestedDepth = 24;
  client->appData.useRemoteCursor =
      src->cursor_client_requested ? GST_RFB_TRUE : GST_RFB_FALSE;
  client->connectTimeout = src->connect_timeout;
  client->readTimeout = src->read_timeout;
  client->canHandleNewFBSize = GST_RFB_TRUE;

  client->format.bitsPerPixel = 32;
  client->format.depth = 24;
  client->format.bigEndian = GST_RFB_FALSE;
  client->format.trueColour = GST_RFB_TRUE;
  client->format.redMax = 255;
  client->format.greenMax = 255;
  client->format.blueMax = 255;
  client->format.redShift = 16;
  client->format.greenShift = 8;
  client->format.blueShift = 0;

  encodings = src->encodings ? src->encodings : DEFAULT_PROP_ENCODINGS;
  g_clear_pointer (&src->active_encodings, g_free);
  if (src->use_copyrect && strstr (encodings, "copyrect") == NULL) {
    src->active_encodings = g_strconcat ("copyrect ", encodings, NULL);
  } else {
    src->active_encodings = g_strdup (encodings);
  }
  client->appData.encodingsString = src->active_encodings;

  GST_DEBUG_OBJECT (src, "connecting to VNC server %s:%d", src->host,
      src->port);

  if (!ConnectToRFBServer (client, src->host, src->port)) {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        ("Could not connect to VNC server %s:%d", src->host, src->port),
        (NULL));
    goto fail;
  }

  if (!InitialiseRFBConnection (client)) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("Could not initialize VNC connection to %s:%d", src->host,
            src->port), (NULL));
    goto fail;
  }

  if (client->width <= 0 || client->height <= 0) {
    client->width = client->si.framebufferWidth;
    client->height = client->si.framebufferHeight;
  }

  if (client->width <= 0 || client->height <= 0) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("VNC server reported invalid framebuffer size %dx%d",
            client->width, client->height), (NULL));
    goto fail;
  }

  if (client->frameBuffer == NULL && !client->MallocFrameBuffer (client)) {
    GST_ELEMENT_ERROR (src, RESOURCE, NO_SPACE_LEFT,
        ("Could not allocate %dx%d VNC framebuffer", client->width,
            client->height), (NULL));
    goto fail;
  }

  if (!SetFormatAndEncodings (client)) {
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Could not send VNC pixel format/encodings to %s:%d", src->host,
            src->port), (NULL));
    goto fail;
  }

  src->server_width = client->width;
  src->server_height = client->height;
  src->connected = TRUE;
  src->geometry_changed = TRUE;

  GST_INFO_OBJECT (src, "connected to VNC server %s:%d, desktop=%dx%d, "
      "protocol=%d.%d", src->host, src->port, client->width, client->height,
      client->major, client->minor);

  if (!gst_rfb_src_update_caps (src))
    goto fail;

  gst_rfb_src_sync_client_update_rect (src);

  return TRUE;

fail:
  rfbClientCleanup (client);
  src->client = NULL;
  src->connected = FALSE;
  return FALSE;
}

static void
gst_rfb_src_close (GstRfbSrc * src)
{
  if (src->client) {
    rfbClientCleanup (src->client);
    src->client = NULL;
  }

  src->connected = FALSE;
  src->frame_valid = FALSE;
  src->frame_dirty = FALSE;
  src->update_request_pending = FALSE;
  src->geometry_changed = FALSE;
  src->have_caps = FALSE;
  src->server_width = 0;
  src->server_height = 0;
  src->button_mask = 0;
  g_clear_pointer (&src->active_encodings, g_free);
  gst_rfb_src_clear_cursor (src);
}

static gboolean
gst_rfb_src_send_framebuffer_update_request (GstRfbSrc * src)
{
  gboolean incremental;

  if (src->update_request_pending)
    return TRUE;

  incremental = src->frame_valid && src->incremental_update;
  src->frame_dirty = FALSE;

  if (!SendFramebufferUpdateRequest (src->client, src->output_x, src->output_y,
          src->output_width, src->output_height,
          incremental ? GST_RFB_TRUE : GST_RFB_FALSE)) {
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Could not request VNC framebuffer update"), (NULL));
    return FALSE;
  }

  src->update_request_pending = TRUE;
  GST_LOG_OBJECT (src, "sent %s framebuffer update request x=%d y=%d %dx%d",
      incremental ? "incremental" : "full", src->output_x, src->output_y,
      src->output_width, src->output_height);

  return TRUE;
}

static gboolean
gst_rfb_src_wait_for_frame (GstRfbSrc * src)
{
  GstClockTime now;
  GstClockTime deadline;
  GstClockTime timeout_deadline;
  gboolean need_first_frame;

  need_first_frame = !src->frame_valid;
  now = gst_util_get_timestamp ();

  if (need_first_frame) {
    timeout_deadline = now + src->frame_timeout_ms * GST_MSECOND;
    deadline = timeout_deadline;
  } else if (GST_CLOCK_TIME_IS_VALID (src->last_frame_time)) {
    deadline = src->last_frame_time + src->frame_duration;
    timeout_deadline = now + src->frame_timeout_ms * GST_MSECOND;
    if (deadline > timeout_deadline)
      deadline = timeout_deadline;
  } else {
    deadline = now;
    timeout_deadline = now + src->frame_timeout_ms * GST_MSECOND;
  }

  while (TRUE) {
    GstClockTime remaining;
    guint wait_usecs;
    int ret;

    if (gst_rfb_src_is_unlocked (src))
      return FALSE;

    if (need_first_frame && src->frame_valid)
      return TRUE;

    now = gst_util_get_timestamp ();
    if (!need_first_frame && now >= deadline)
      return TRUE;
    if (need_first_frame && now >= timeout_deadline) {
      GST_ELEMENT_ERROR (src, RESOURCE, READ,
          ("Timed out waiting for first VNC framebuffer update"), (NULL));
      return FALSE;
    }

    remaining = (need_first_frame ? timeout_deadline : deadline) - now;
    wait_usecs = (guint) MIN (remaining / GST_USECOND, (GstClockTime) 100000);
    if (wait_usecs == 0)
      wait_usecs = 1;

    ret = WaitForMessage (src->client, wait_usecs);
    if (ret < 0) {
      GST_ELEMENT_ERROR (src, RESOURCE, READ,
          ("Error while waiting for VNC server message"), (NULL));
      return FALSE;
    }

    if (ret > 0) {
      if (!HandleRFBServerMessage (src->client)) {
        GST_ELEMENT_ERROR (src, RESOURCE, READ,
            ("Error while handling VNC server message"), (NULL));
        return FALSE;
      }

      if (src->geometry_changed) {
        src->geometry_changed = FALSE;
        if (!gst_rfb_src_update_caps (src))
          return FALSE;
      }
      need_first_frame = !src->frame_valid;
    }
  }
}

static gboolean
gst_rfb_src_cursor_is_usable (GstRfbSrc * src)
{
  return src->cursor_client_requested &&
      src->cursor_shape_valid && src->cursor_position_valid &&
      src->cursor_mode != GST_RFB_SRC_CURSOR_MODE_NONE;
}

static void
gst_rfb_src_maybe_fallback_cursor (GstRfbSrc * src)
{
  if (src->cursor_mode != GST_RFB_SRC_CURSOR_MODE_AUTO ||
      !src->cursor_client_requested ||
      (src->cursor_shape_valid && src->cursor_position_valid))
    return;

  src->cursor_auto_fallback_frames++;
  if (src->cursor_auto_fallback_frames < DEFAULT_CURSOR_FALLBACK_FRAMES)
    return;

  GST_INFO_OBJECT (src, "no usable remote cursor received; "
      "falling back to server-drawn cursor");

  gst_rfb_src_clear_cursor (src);
  src->cursor_client_requested = FALSE;
  src->client->appData.useRemoteCursor = GST_RFB_FALSE;

  if (!SetFormatAndEncodings (src->client))
    GST_WARNING_OBJECT (src,
        "could not renegotiate server-drawn cursor fallback");
}

static gboolean
gst_rfb_src_cursor_mask_is_opaque (GstRfbSrc * src, gint x, gint y)
{
  gsize mask_stride;
  gsize mask_offset;

  if (src->cursor_mask == NULL)
    return TRUE;

  mask_stride = ((gsize) src->cursor_width + 7) / 8;
  mask_offset = (gsize) y * mask_stride + (gsize) x / 8;
  if (mask_offset >= src->cursor_mask_size)
    return FALSE;

  return (src->cursor_mask[mask_offset] & (0x80 >> (x & 7))) != 0;
}

static void
gst_rfb_src_draw_cursor (GstRfbSrc * src, GstMapInfo * map,
    gsize output_stride)
{
  gint rel_x;
  gint rel_y;
  gint start_x;
  gint start_y;
  gint end_x;
  gint end_y;
  gint y;

  if (!gst_rfb_src_cursor_is_usable (src))
    return;

  if (src->cursor_bpp != 4 && src->cursor_bpp != 3) {
    GST_LOG_OBJECT (src, "skipping unsupported cursor bpp=%d",
        src->cursor_bpp);
    return;
  }

  rel_x = src->cursor_x - src->cursor_hot_x - src->output_x;
  rel_y = src->cursor_y - src->cursor_hot_y - src->output_y;
  start_x = MAX (0, -rel_x);
  start_y = MAX (0, -rel_y);
  end_x = MIN (src->cursor_width, src->output_width - rel_x);
  end_y = MIN (src->cursor_height, src->output_height - rel_y);

  if (start_x >= end_x || start_y >= end_y)
    return;

  for (y = start_y; y < end_y; y++) {
    gint x;

    for (x = start_x; x < end_x; x++) {
      gsize source_offset;
      gsize dest_offset;
      const guint8 *source_pixel;
      guint8 *dest_pixel;

      if (!gst_rfb_src_cursor_mask_is_opaque (src, x, y))
        continue;

      source_offset = ((gsize) y * src->cursor_width + x) *
          src->cursor_bpp;
      dest_offset = (gsize) (rel_y + y) * output_stride +
          (gsize) (rel_x + x) * 4;

      if (source_offset + src->cursor_bpp > src->cursor_source_size ||
          dest_offset + 4 > map->size)
        continue;

      source_pixel = src->cursor_source + source_offset;
      dest_pixel = map->data + dest_offset;

      dest_pixel[0] = source_pixel[0];
      dest_pixel[1] = source_pixel[1];
      dest_pixel[2] = source_pixel[2];
      dest_pixel[3] = 0xff;
    }
  }
}

static gboolean
gst_rfb_src_copy_frame (GstRfbSrc * src, GstBuffer * buffer)
{
  GstMapInfo map;
  const guint8 *source;
  gsize source_stride;
  gsize output_stride;
  gsize row_bytes;
  gint row;

  if (src->client == NULL || src->client->frameBuffer == NULL)
    return FALSE;

  if (!gst_buffer_map (buffer, &map, GST_MAP_WRITE))
    return FALSE;

  source = src->client->frameBuffer;
  source_stride = (gsize) src->client->width * 4;
  output_stride = GST_VIDEO_INFO_PLANE_STRIDE (&src->vinfo, 0);
  row_bytes = (gsize) src->output_width * 4;

  for (row = 0; row < src->output_height; row++) {
    const guint8 *src_row = source +
        ((gsize) (src->output_y + row) * source_stride) +
        ((gsize) src->output_x * 4);
    guint8 *dst_row = map.data + (gsize) row * output_stride;

    memcpy (dst_row, src_row, row_bytes);
  }

  gst_rfb_src_draw_cursor (src, &map, output_stride);

  gst_buffer_unmap (buffer, &map);
  return TRUE;
}

static GstClockTime
gst_rfb_src_get_running_time (GstRfbSrc * src)
{
  GstClock *clock;
  GstClockTime now;
  GstClockTime base_time;

  clock = gst_element_get_clock (GST_ELEMENT (src));
  if (clock) {
    now = gst_clock_get_time (clock);
    base_time = gst_element_get_base_time (GST_ELEMENT (src));
    gst_object_unref (clock);

    if (GST_CLOCK_TIME_IS_VALID (now) &&
        GST_CLOCK_TIME_IS_VALID (base_time) && now >= base_time)
      return now - base_time;
  }

  now = src->next_pts;
  src->next_pts += src->frame_duration;
  return now;
}

static gboolean
gst_rfb_src_send_key_locked (GstRfbSrc * src, guint keysym, gboolean down)
{
  if (src->view_only) {
    GST_LOG_OBJECT (src, "dropping key event in view-only mode");
    return FALSE;
  }

  if (!src->connected || src->client == NULL) {
    GST_WARNING_OBJECT (src, "dropping key event because VNC is not connected");
    return FALSE;
  }

  if (keysym == 0) {
    GST_WARNING_OBJECT (src, "dropping key event with empty keysym");
    return FALSE;
  }

  return SendKeyEvent (src->client, keysym,
      down ? GST_RFB_TRUE : GST_RFB_FALSE) ? TRUE : FALSE;
}

static gboolean
gst_rfb_src_send_pointer_locked (GstRfbSrc * src, gint x, gint y,
    guint button_mask)
{
  gint remote_x;
  gint remote_y;

  if (src->view_only) {
    GST_LOG_OBJECT (src, "dropping pointer event in view-only mode");
    return FALSE;
  }

  if (!src->connected || src->client == NULL) {
    GST_WARNING_OBJECT (src,
        "dropping pointer event because VNC is not connected");
    return FALSE;
  }

  remote_x = src->offset_x + x;
  remote_y = src->offset_y + y;

  if (src->client->width > 0)
    remote_x = CLAMP (remote_x, 0, src->client->width - 1);
  if (src->client->height > 0)
    remote_y = CLAMP (remote_y, 0, src->client->height - 1);

  src->button_mask = button_mask;
  src->cursor_x = remote_x;
  src->cursor_y = remote_y;
  src->cursor_position_valid = TRUE;
  src->frame_dirty = TRUE;

  GST_LOG_OBJECT (src, "sending pointer event x=%d y=%d mask=%u",
      remote_x, remote_y, button_mask);

  return SendPointerEvent (src->client, remote_x, remote_y,
      button_mask) ? TRUE : FALSE;
}

static gboolean
gst_rfb_src_signal_send_key (GstRfbSrc * src, guint keysym, gboolean down)
{
  gboolean ret;

  g_rec_mutex_lock (&src->client_lock);
  ret = gst_rfb_src_send_key_locked (src, keysym, down);
  g_rec_mutex_unlock (&src->client_lock);

  return ret;
}

static gboolean
gst_rfb_src_signal_send_key_name (GstRfbSrc * src, const gchar * key,
    gboolean down)
{
  guint keysym;

  keysym = gst_rfb_src_keysym_from_name (key);
  if (keysym == 0) {
    GST_WARNING_OBJECT (src, "unknown key name '%s'", GST_STR_NULL (key));
    return FALSE;
  }

  return gst_rfb_src_signal_send_key (src, keysym, down);
}

static gboolean
gst_rfb_src_signal_send_pointer (GstRfbSrc * src, gint x, gint y,
    guint button_mask)
{
  gboolean ret;

  g_rec_mutex_lock (&src->client_lock);
  ret = gst_rfb_src_send_pointer_locked (src, x, y, button_mask);
  g_rec_mutex_unlock (&src->client_lock);

  return ret;
}

static gboolean
gst_rfb_src_signal_send_mouse_button (GstRfbSrc * src, gint button,
    gboolean down, gint x, gint y)
{
  gboolean ret;
  guint mask_bit;

  if (button < 1 || button > 8) {
    GST_WARNING_OBJECT (src, "invalid mouse button %d", button);
    return FALSE;
  }

  mask_bit = 1u << (button - 1);

  g_rec_mutex_lock (&src->client_lock);
  if (down)
    src->button_mask |= mask_bit;
  else
    src->button_mask &= ~mask_bit;

  ret = gst_rfb_src_send_pointer_locked (src, x, y, src->button_mask);
  g_rec_mutex_unlock (&src->client_lock);

  return ret;
}

static gboolean
gst_rfb_src_signal_send_text (GstRfbSrc * src, const gchar * text)
{
  const gchar *p;
  gboolean ret = TRUE;

  if (text == NULL)
    return FALSE;

  g_rec_mutex_lock (&src->client_lock);

  for (p = text; *p != '\0'; p = g_utf8_next_char (p)) {
    gunichar ch;
    guint keysym;

    ch = g_utf8_get_char_validated (p, -1);
    if (ch == (gunichar) - 1 || ch == (gunichar) - 2) {
      ret = FALSE;
      break;
    }

    keysym = gst_rfb_src_unicode_to_keysym (ch);
    ret = gst_rfb_src_send_key_locked (src, keysym, TRUE) &&
        gst_rfb_src_send_key_locked (src, keysym, FALSE);
    if (!ret)
      break;
  }

  g_rec_mutex_unlock (&src->client_lock);

  return ret;
}

static gboolean
gst_rfb_src_signal_send_clipboard (GstRfbSrc * src, const gchar * text)
{
  gboolean ret;
  gchar *latin1 = NULL;
  gsize bytes_written = 0;
  GError *error = NULL;

  if (text == NULL)
    return FALSE;

  g_rec_mutex_lock (&src->client_lock);

  if (src->view_only) {
    ret = FALSE;
  } else if (!src->connected || src->client == NULL) {
    GST_WARNING_OBJECT (src,
        "dropping clipboard event because VNC is not connected");
    ret = FALSE;
  } else {
    /* Older LibVNCClient only exposes SendClientCutText(), whose payload is
     * Latin-1. Keep the plugin linkable there and degrade non-Latin-1 chars. */
    latin1 = g_convert_with_fallback (text, -1, "ISO-8859-1", "UTF-8", "?",
        NULL, &bytes_written, &error);
    if (latin1 == NULL) {
      GST_WARNING_OBJECT (src, "could not convert clipboard text: %s",
          error->message);
      g_clear_error (&error);
      ret = FALSE;
    } else {
      ret = SendClientCutText (src->client, latin1, (int) bytes_written) ?
          TRUE : FALSE;
      g_free (latin1);
    }
  }

  g_rec_mutex_unlock (&src->client_lock);

  return ret;
}

static void
gst_rfb_src_class_init (GstRfbSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstElementClass *gstelement_class;
  GstPushSrcClass *gstpushsrc_class;
  GParamFlags ready_flags;

  GST_DEBUG_CATEGORY_INIT (rfbsrc_debug, "rfbsrc", 0,
      "LibVNCClient-backed RFB source");
  gst_rfb_src_install_libvnc_logging ();

  gobject_class = (GObjectClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;
  gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = gst_rfb_src_finalize;
  gobject_class->set_property = gst_rfb_src_set_property;
  gobject_class->get_property = gst_rfb_src_get_property;

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_rfb_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_rfb_src_stop);
  gstbasesrc_class->negotiate = GST_DEBUG_FUNCPTR (gst_rfb_src_negotiate);
  gstbasesrc_class->event = GST_DEBUG_FUNCPTR (gst_rfb_src_event);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_rfb_src_unlock);
  gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_rfb_src_unlock_stop);
  gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_rfb_src_create);

  gst_element_class_add_static_pad_template (gstelement_class,
      &gst_rfb_src_template);

  gst_element_class_set_static_metadata (gstelement_class,
      "RFB/VNC source", "Source/Video",
      "Creates a raw video stream from a VNC server using LibVNCClient",
      "David A. Schleef <ds@schleef.org>, "
      "Andre Moreira Magalhaes <andre.magalhaes@indt.org.br>, "
      "Thijs Vermeir <thijsvermeir@gmail.com>, Autonoma");

  ready_flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
      GST_PARAM_MUTABLE_READY;

  /**
   * GstRfbSrc:uri:
   *
   * URI in the form rfb://[user:password@]host[:port]?property=value.
   */
  g_object_class_install_property (gobject_class, PROP_URI,
      g_param_spec_string ("uri", "URI",
          "URI in the form rfb://[user:password@]host[:port]?property=value",
          DEFAULT_PROP_URI, ready_flags));

  g_object_class_install_property (gobject_class, PROP_HOST,
      g_param_spec_string ("host", "Host", "Host to connect to",
          DEFAULT_PROP_HOST, ready_flags));

  g_object_class_install_property (gobject_class, PROP_PORT,
      g_param_spec_int ("port", "Port", "Port to connect to",
          1, 65535, DEFAULT_PROP_PORT, ready_flags));

  g_object_class_install_property (gobject_class, PROP_USERNAME,
      g_param_spec_string ("username", "Username",
          "Username for VNC security types that require one", NULL,
          ready_flags));

  g_object_class_install_property (gobject_class, PROP_VERSION,
      g_param_spec_string ("version", "RFB protocol version",
          "Requested RFB protocol version (kept for compatibility; "
          "LibVNCClient negotiates automatically)", DEFAULT_PROP_VERSION,
          ready_flags));

  g_object_class_install_property (gobject_class, PROP_PASSWORD,
      g_param_spec_string ("password", "Password",
          "Password for VNC authentication", NULL,
          G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_OFFSET_X,
      g_param_spec_int ("offset-x", "X offset",
          "Left offset of the capture rectangle", 0, G_MAXINT, 0,
          ready_flags));

  g_object_class_install_property (gobject_class, PROP_OFFSET_Y,
      g_param_spec_int ("offset-y", "Y offset",
          "Top offset of the capture rectangle", 0, G_MAXINT, 0,
          ready_flags));

  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_int ("width", "Width",
          "Capture rectangle width, or 0 for remaining desktop width",
          0, G_MAXINT, 0, ready_flags));

  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_int ("height", "Height",
          "Capture rectangle height, or 0 for remaining desktop height",
          0, G_MAXINT, 0, ready_flags));

  g_object_class_install_property (gobject_class, PROP_INCREMENTAL,
      g_param_spec_boolean ("incremental", "Incremental updates",
          "Request incremental framebuffer updates after the first frame",
          TRUE, ready_flags));

  g_object_class_install_property (gobject_class, PROP_USE_COPYRECT,
      g_param_spec_boolean ("use-copyrect", "Use CopyRect",
          "Include CopyRect in requested encodings", FALSE, ready_flags));

  g_object_class_install_property (gobject_class, PROP_ENCODINGS,
      g_param_spec_string ("encodings", "Encodings",
          "LibVNCClient encoding preference string",
          DEFAULT_PROP_ENCODINGS, ready_flags));

  g_object_class_install_property (gobject_class, PROP_SHARED,
      g_param_spec_boolean ("shared", "Shared desktop",
          "Share desktop with other VNC clients", TRUE, ready_flags));

  g_object_class_install_property (gobject_class, PROP_VIEWONLY,
      g_param_spec_boolean ("view-only", "View only",
          "Disable sending keyboard, pointer, and clipboard events", FALSE,
          ready_flags));

  g_object_class_install_property (gobject_class, PROP_CURSOR_MODE,
      g_param_spec_enum ("cursor-mode", "Cursor mode",
          "How to include the remote mouse cursor in output frames",
          GST_TYPE_RFB_SRC_CURSOR_MODE, DEFAULT_PROP_CURSOR_MODE,
          ready_flags));

  g_object_class_install_property (gobject_class, PROP_MAX_FRAMERATE,
      g_param_spec_int ("max-framerate", "Maximum framerate",
          "Maximum output frames per second", 1, 240,
          DEFAULT_PROP_MAX_FRAMERATE, ready_flags));

  g_object_class_install_property (gobject_class, PROP_FRAME_TIMEOUT_MS,
      g_param_spec_uint ("frame-timeout-ms", "Frame timeout",
          "Maximum time to wait for the first framebuffer update in ms",
          1, 60000, DEFAULT_PROP_FRAME_TIMEOUT_MS, ready_flags));

  g_object_class_install_property (gobject_class, PROP_CONNECT_TIMEOUT,
      g_param_spec_uint ("connect-timeout", "Connect timeout",
          "Socket connect timeout in seconds", 1, 3600,
          DEFAULT_PROP_CONNECT_TIMEOUT, ready_flags));

  g_object_class_install_property (gobject_class, PROP_READ_TIMEOUT,
      g_param_spec_uint ("read-timeout", "Read timeout",
          "Socket read timeout in seconds", 0, 3600,
          DEFAULT_PROP_READ_TIMEOUT, ready_flags));

  /**
   * GstRfbSrc::send-key:
   * @object: the #GstRfbSrc
   * @keysym: X11/RFB keysym value
   * @down: %TRUE for key press, %FALSE for key release
   *
   * Send one keyboard event to the VNC server.
   *
   * Returns: %TRUE when the event was sent.
   */
  gst_rfb_src_signals[SIGNAL_SEND_KEY] =
      g_signal_new_class_handler ("send-key", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_rfb_src_signal_send_key), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 2, G_TYPE_UINT, G_TYPE_BOOLEAN);

  /**
   * GstRfbSrc::send-key-name:
   * @object: the #GstRfbSrc
   * @key: a key name such as "Return", "Left", "F1", "a", "U+20AC", or "0xff0d"
   * @down: %TRUE for key press, %FALSE for key release
   *
   * Send one named keyboard event to the VNC server.
   *
   * Returns: %TRUE when the event was sent.
   */
  gst_rfb_src_signals[SIGNAL_SEND_KEY_NAME] =
      g_signal_new_class_handler ("send-key-name", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_rfb_src_signal_send_key_name), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);

  /**
   * GstRfbSrc::send-pointer:
   * @object: the #GstRfbSrc
   * @x: X coordinate inside the captured output rectangle
   * @y: Y coordinate inside the captured output rectangle
   * @button_mask: VNC button mask
   *
   * Send one pointer event to the VNC server.
   *
   * Returns: %TRUE when the event was sent.
   */
  gst_rfb_src_signals[SIGNAL_SEND_POINTER] =
      g_signal_new_class_handler ("send-pointer", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_rfb_src_signal_send_pointer), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 3, G_TYPE_INT, G_TYPE_INT, G_TYPE_UINT);

  /**
   * GstRfbSrc::send-mouse-button:
   * @object: the #GstRfbSrc
   * @button: mouse button number, 1 to 8
   * @down: %TRUE for press, %FALSE for release
   * @x: X coordinate inside the captured output rectangle
   * @y: Y coordinate inside the captured output rectangle
   *
   * Update internal button mask and send a pointer event.
   *
   * Returns: %TRUE when the event was sent.
   */
  gst_rfb_src_signals[SIGNAL_SEND_MOUSE_BUTTON] =
      g_signal_new_class_handler ("send-mouse-button",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_rfb_src_signal_send_mouse_button), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 4, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_INT);

  /**
   * GstRfbSrc::send-text:
   * @object: the #GstRfbSrc
   * @text: UTF-8 text to send as key press/release pairs
   *
   * Send text using RFB key events.
   *
   * Returns: %TRUE when all key events were sent.
   */
  gst_rfb_src_signals[SIGNAL_SEND_TEXT] =
      g_signal_new_class_handler ("send-text", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_rfb_src_signal_send_text), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 1, G_TYPE_STRING);

  /**
   * GstRfbSrc::send-clipboard:
   * @object: the #GstRfbSrc
   * @text: UTF-8 clipboard text
   *
   * Send a client-cut-text message to the VNC server.
   *
   * Returns: %TRUE when the clipboard message was sent.
   */
  gst_rfb_src_signals[SIGNAL_SEND_CLIPBOARD] =
      g_signal_new_class_handler ("send-clipboard", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_rfb_src_signal_send_clipboard), NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
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
  src->version = g_strdup (DEFAULT_PROP_VERSION);
  src->encodings = g_strdup (DEFAULT_PROP_ENCODINGS);
  src->incremental_update = TRUE;
  src->shared = TRUE;
  src->view_only = FALSE;
  src->use_copyrect = FALSE;
  src->cursor_mode = DEFAULT_PROP_CURSOR_MODE;
  src->max_framerate = DEFAULT_PROP_MAX_FRAMERATE;
  src->frame_timeout_ms = DEFAULT_PROP_FRAME_TIMEOUT_MS;
  src->connect_timeout = DEFAULT_PROP_CONNECT_TIMEOUT;
  src->read_timeout = DEFAULT_PROP_READ_TIMEOUT;
  src->frame_duration =
      gst_util_uint64_scale_int (GST_SECOND, 1, src->max_framerate);
  src->last_frame_time = GST_CLOCK_TIME_NONE;

  gst_video_info_init (&src->vinfo);
  g_rec_mutex_init (&src->client_lock);
}

static void
gst_rfb_src_finalize (GObject * object)
{
  GstRfbSrc *src = GST_RFB_SRC (object);

  g_rec_mutex_lock (&src->client_lock);
  gst_rfb_src_close (src);
  g_rec_mutex_unlock (&src->client_lock);

  if (src->uri)
    gst_uri_unref (src->uri);

  g_free (src->host);
  g_free (src->username);
  g_free (src->password);
  g_free (src->version);
  g_free (src->encodings);
  g_free (src->active_encodings);
  g_rec_mutex_clear (&src->client_lock);

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
      gst_rfb_src_set_ready_string (src, &src->host,
          g_value_get_string (value), "host");
      break;
    case PROP_PORT:
      if (!gst_rfb_src_is_running (src))
        src->port = g_value_get_int (value);
      break;
    case PROP_USERNAME:
      gst_rfb_src_set_ready_string (src, &src->username,
          g_value_get_string (value), "username");
      break;
    case PROP_VERSION:
      gst_rfb_src_set_ready_string (src, &src->version,
          g_value_get_string (value), "version");
      break;
    case PROP_PASSWORD:
      gst_rfb_src_set_ready_string (src, &src->password,
          g_value_get_string (value), "password");
      break;
    case PROP_OFFSET_X:
      if (!gst_rfb_src_is_running (src))
        src->offset_x = g_value_get_int (value);
      break;
    case PROP_OFFSET_Y:
      if (!gst_rfb_src_is_running (src))
        src->offset_y = g_value_get_int (value);
      break;
    case PROP_WIDTH:
      if (!gst_rfb_src_is_running (src))
        src->requested_width = g_value_get_int (value);
      break;
    case PROP_HEIGHT:
      if (!gst_rfb_src_is_running (src))
        src->requested_height = g_value_get_int (value);
      break;
    case PROP_INCREMENTAL:
      if (!gst_rfb_src_is_running (src))
        src->incremental_update = g_value_get_boolean (value);
      break;
    case PROP_USE_COPYRECT:
      if (!gst_rfb_src_is_running (src))
        src->use_copyrect = g_value_get_boolean (value);
      break;
    case PROP_ENCODINGS:
      gst_rfb_src_set_ready_string (src, &src->encodings,
          g_value_get_string (value), "encodings");
      break;
    case PROP_SHARED:
      if (!gst_rfb_src_is_running (src))
        src->shared = g_value_get_boolean (value);
      break;
    case PROP_VIEWONLY:
      if (!gst_rfb_src_is_running (src))
        src->view_only = g_value_get_boolean (value);
      break;
    case PROP_CURSOR_MODE:
      if (!gst_rfb_src_is_running (src))
        src->cursor_mode = g_value_get_enum (value);
      break;
    case PROP_MAX_FRAMERATE:
      if (!gst_rfb_src_is_running (src)) {
        src->max_framerate = g_value_get_int (value);
        src->frame_duration =
            gst_util_uint64_scale_int (GST_SECOND, 1, src->max_framerate);
      }
      break;
    case PROP_FRAME_TIMEOUT_MS:
      if (!gst_rfb_src_is_running (src))
        src->frame_timeout_ms = g_value_get_uint (value);
      break;
    case PROP_CONNECT_TIMEOUT:
      if (!gst_rfb_src_is_running (src))
        src->connect_timeout = g_value_get_uint (value);
      break;
    case PROP_READ_TIMEOUT:
      if (!gst_rfb_src_is_running (src))
        src->read_timeout = g_value_get_uint (value);
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
      if (src->uri)
        g_value_take_string (value, gst_uri_to_string (src->uri));
      else
        g_value_set_string (value, NULL);
      GST_OBJECT_UNLOCK (object);
      break;
    case PROP_HOST:
      g_value_set_string (value, src->host);
      break;
    case PROP_PORT:
      g_value_set_int (value, src->port);
      break;
    case PROP_USERNAME:
      g_value_set_string (value, src->username);
      break;
    case PROP_VERSION:
      g_value_set_string (value, src->version);
      break;
    case PROP_OFFSET_X:
      g_value_set_int (value, src->offset_x);
      break;
    case PROP_OFFSET_Y:
      g_value_set_int (value, src->offset_y);
      break;
    case PROP_WIDTH:
      g_value_set_int (value, src->requested_width);
      break;
    case PROP_HEIGHT:
      g_value_set_int (value, src->requested_height);
      break;
    case PROP_INCREMENTAL:
      g_value_set_boolean (value, src->incremental_update);
      break;
    case PROP_USE_COPYRECT:
      g_value_set_boolean (value, src->use_copyrect);
      break;
    case PROP_ENCODINGS:
      g_value_set_string (value, src->encodings);
      break;
    case PROP_SHARED:
      g_value_set_boolean (value, src->shared);
      break;
    case PROP_VIEWONLY:
      g_value_set_boolean (value, src->view_only);
      break;
    case PROP_CURSOR_MODE:
      g_value_set_enum (value, src->cursor_mode);
      break;
    case PROP_MAX_FRAMERATE:
      g_value_set_int (value, src->max_framerate);
      break;
    case PROP_FRAME_TIMEOUT_MS:
      g_value_set_uint (value, src->frame_timeout_ms);
      break;
    case PROP_CONNECT_TIMEOUT:
      g_value_set_uint (value, src->connect_timeout);
      break;
    case PROP_READ_TIMEOUT:
      g_value_set_uint (value, src->read_timeout);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_rfb_src_start (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  g_rec_mutex_lock (&src->client_lock);
  gst_rfb_src_set_unlocked (src, FALSE);
  src->frame_valid = FALSE;
  src->frame_dirty = FALSE;
  src->geometry_changed = FALSE;
  src->update_request_pending = FALSE;
  src->last_frame_time = GST_CLOCK_TIME_NONE;
  src->next_pts = 0;
  g_rec_mutex_unlock (&src->client_lock);

  return TRUE;
}

static gboolean
gst_rfb_src_stop (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  g_rec_mutex_lock (&src->client_lock);
  gst_rfb_src_close (src);
  gst_rfb_src_set_unlocked (src, FALSE);
  g_rec_mutex_unlock (&src->client_lock);

  return TRUE;
}

static gboolean
gst_rfb_src_negotiate (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);
  gboolean ret;

  g_rec_mutex_lock (&src->client_lock);
  ret = gst_rfb_src_open (src);
  if (ret)
    ret = gst_rfb_src_update_caps (src);
  g_rec_mutex_unlock (&src->client_lock);

  return ret;
}

static GstFlowReturn
gst_rfb_src_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
  GstRfbSrc *src = GST_RFB_SRC (psrc);
  GstBuffer *buffer;
  GstFlowReturn ret = GST_FLOW_OK;

  *outbuf = NULL;

  g_rec_mutex_lock (&src->client_lock);

  if (gst_rfb_src_is_unlocked (src)) {
    ret = GST_FLOW_FLUSHING;
    goto out;
  }

  if (!gst_rfb_src_open (src)) {
    ret = gst_rfb_src_is_unlocked (src) ? GST_FLOW_FLUSHING : GST_FLOW_ERROR;
    goto out;
  }

  if (src->geometry_changed) {
    src->geometry_changed = FALSE;
    if (!gst_rfb_src_update_caps (src)) {
      ret = GST_FLOW_NOT_NEGOTIATED;
      goto out;
    }
    gst_rfb_src_sync_client_update_rect (src);
  }

  if (!gst_rfb_src_send_framebuffer_update_request (src) ||
      !gst_rfb_src_wait_for_frame (src)) {
    ret = gst_rfb_src_is_unlocked (src) ? GST_FLOW_FLUSHING : GST_FLOW_ERROR;
    goto out;
  }

  gst_rfb_src_maybe_fallback_cursor (src);

  if (!src->frame_valid || src->client->frameBuffer == NULL) {
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        ("No VNC framebuffer available"), (NULL));
    ret = GST_FLOW_ERROR;
    goto out;
  }

  buffer = gst_buffer_new_allocate (NULL, GST_VIDEO_INFO_SIZE (&src->vinfo),
      NULL);
  if (buffer == NULL) {
    ret = GST_FLOW_ERROR;
    goto out;
  }

  if (!gst_rfb_src_copy_frame (src, buffer)) {
    gst_buffer_unref (buffer);
    GST_ELEMENT_ERROR (src, RESOURCE, WRITE,
        ("Could not copy VNC framebuffer into output buffer"), (NULL));
    ret = GST_FLOW_ERROR;
    goto out;
  }

  GST_BUFFER_PTS (buffer) = gst_rfb_src_get_running_time (src);
  GST_BUFFER_DURATION (buffer) = src->frame_duration;
  src->last_frame_time = gst_util_get_timestamp ();
  *outbuf = buffer;

out:
  g_rec_mutex_unlock (&src->client_lock);
  return ret;
}

static gboolean
gst_rfb_src_event (GstBaseSrc * bsrc, GstEvent * event)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);
  gdouble x, y;
  gint button;
  GstNavigationEventType event_type;

  if (GST_EVENT_TYPE (event) != GST_EVENT_NAVIGATION)
    return TRUE;

  if (src->view_only)
    return TRUE;

  event_type = gst_navigation_event_get_type (event);

  switch (event_type) {
    case GST_NAVIGATION_EVENT_KEY_PRESS:
    case GST_NAVIGATION_EVENT_KEY_RELEASE:{
      const gchar *key;

      if (gst_navigation_event_parse_key_event (event, &key))
        gst_rfb_src_signal_send_key_name (src, key,
            event_type == GST_NAVIGATION_EVENT_KEY_PRESS);
      break;
    }
    case GST_NAVIGATION_EVENT_MOUSE_BUTTON_PRESS:
      if (gst_navigation_event_parse_mouse_button_event (event,
              &button, &x, &y))
        gst_rfb_src_signal_send_mouse_button (src, button, TRUE, (gint) x,
            (gint) y);
      break;
    case GST_NAVIGATION_EVENT_MOUSE_BUTTON_RELEASE:
      if (gst_navigation_event_parse_mouse_button_event (event,
              &button, &x, &y))
        gst_rfb_src_signal_send_mouse_button (src, button, FALSE, (gint) x,
            (gint) y);
      break;
    case GST_NAVIGATION_EVENT_MOUSE_MOVE:
      if (gst_navigation_event_parse_mouse_move_event (event, &x, &y))
        gst_rfb_src_signal_send_pointer (src, (gint) x, (gint) y,
            src->button_mask);
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
  rfbClient *client;

  GST_OBJECT_LOCK (src);
  gst_rfb_src_set_unlocked (src, TRUE);
  client = src->client;
  if (client)
    rfbCloseSocket (client->sock);
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static gboolean
gst_rfb_src_unlock_stop (GstBaseSrc * bsrc)
{
  GstRfbSrc *src = GST_RFB_SRC (bsrc);

  GST_OBJECT_LOCK (src);
  gst_rfb_src_set_unlocked (src, FALSE);
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static GstURIType
gst_rfb_src_uri_get_type (GType type)
{
  (void) type;
  return GST_URI_SRC;
}

static const gchar *const *
gst_rfb_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { (char *) "rfb", (char *) "vnc", NULL };

  (void) type;
  return protocols;
}

static gchar *
gst_rfb_src_uri_get_uri (GstURIHandler * handler)
{
  GstRfbSrc *src = (GstRfbSrc *) handler;
  gchar *str_uri = NULL;

  GST_OBJECT_LOCK (src);
  if (src->uri)
    str_uri = gst_uri_to_string (src->uri);
  GST_OBJECT_UNLOCK (src);

  return str_uri;
}

static void
gst_rfb_src_set_properties_from_uri_query (GObject * object,
    const GstUri * uri)
{
  GHashTable *query_table;
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  query_table = gst_uri_get_query_table (uri);
  if (query_table == NULL)
    return;

  g_hash_table_iter_init (&iter, query_table);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    if (key == NULL || value == NULL)
      continue;

    GST_DEBUG_OBJECT (object, "setting property '%s' from URI query",
        (const gchar *) key);
    gst_util_set_object_arg (object, (const gchar *) key,
        (const gchar *) value);
  }

  g_hash_table_unref (query_table);
}

static gboolean
gst_rfb_src_uri_set_uri (GstURIHandler * handler, const gchar * str_uri,
    GError ** error)
{
  GstRfbSrc *src = (GstRfbSrc *) handler;
  GstUri *uri = NULL;
  const gchar *scheme;
  const gchar *userinfo;
  const gchar *host;
  guint port;

  g_return_val_if_fail (str_uri != NULL, FALSE);

  if (gst_rfb_src_is_running (src)) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_STATE,
        "Changing the URI on rfbsrc when it is running is not supported");
    GST_ERROR_OBJECT (src,
        "Changing the URI on rfbsrc when it is running is not supported");
    return FALSE;
  }

  uri = gst_uri_from_string (str_uri);
  if (uri == NULL) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "Invalid URI: %s", str_uri);
    return FALSE;
  }

  scheme = gst_uri_get_scheme (uri);
  if (g_strcmp0 (scheme, "rfb") != 0 && g_strcmp0 (scheme, "vnc") != 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "Invalid URI scheme '%s' (expected rfb or vnc)", GST_STR_NULL (scheme));
    gst_uri_unref (uri);
    return FALSE;
  }

  host = gst_uri_get_host (uri);
  if (host && *host)
    g_object_set (src, "host", host, NULL);

  port = gst_uri_get_port (uri);
  if (port != GST_URI_NO_PORT)
    g_object_set (src, "port", (gint) port, NULL);
  else
    g_object_set (src, "port", DEFAULT_PROP_PORT, NULL);

  userinfo = gst_uri_get_userinfo (uri);
  if (userinfo) {
    gchar **split;
    gchar *user = NULL;
    gchar *pass = NULL;

    split = g_strsplit (userinfo, ":", 2);
    if (split[0] && split[1]) {
      user = g_uri_unescape_string (split[0], NULL);
      pass = g_uri_unescape_string (split[1], NULL);
    } else if (split[0]) {
      pass = g_uri_unescape_string (split[0], NULL);
    }

    if (user)
      g_object_set (src, "username", user, NULL);
    if (pass)
      g_object_set (src, "password", pass, NULL);

    g_free (user);
    g_free (pass);
    g_strfreev (split);
  }

  GST_OBJECT_LOCK (src);
  if (src->uri)
    gst_uri_unref (src->uri);
  src->uri = gst_uri_ref (uri);
  GST_OBJECT_UNLOCK (src);

  gst_rfb_src_set_properties_from_uri_query (G_OBJECT (src), uri);
  gst_uri_unref (uri);

  return TRUE;
}

static void
gst_rfb_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  (void) iface_data;

  iface->get_type = gst_rfb_src_uri_get_type;
  iface->get_protocols = gst_rfb_src_uri_get_protocols;
  iface->get_uri = gst_rfb_src_uri_get_uri;
  iface->set_uri = gst_rfb_src_uri_set_uri;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (rfbsrc, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rfbsrc,
    "Connects to a VNC server through LibVNCClient and decodes RFB stream",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
