#ifndef GST_PLUGINS_BAD_GST_LIBRFB_RFBSRC_KEYMAP_H_
#define GST_PLUGINS_BAD_GST_LIBRFB_RFBSRC_KEYMAP_H_

#include <glib.h>

G_BEGIN_DECLS

/* Maps a single Unicode character to an RFB/X11 keysym. */
guint rfbsrc_unicode_to_keysym(gunichar ch);

/* Maps a DOM KeyboardEvent (key, location) to an RFB/X11 keysym.
 * location: 0=standard, 1=left, 2=right, 3=numpad.
 * Returns 0 if there is no mapping. */
guint rfbsrc_dom_key_to_keysym(const gchar* key, gint location);

G_END_DECLS

#endif  // GST_PLUGINS_BAD_GST_LIBRFB_RFBSRC_KEYMAP_H_
