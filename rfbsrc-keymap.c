/* GStreamer
 * Copyright (C) <2026> Julian Grasböck <j.grasboeck@gmail.com>
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

#include <stdlib.h>
#include <string.h>

#include "rfbsrc-keymap.h"

/* LibVNCClient defines TRUE/FALSE itself. Undef GLib macros first to avoid
 * noisy macro-redefinition warnings, then restore GLib-compatible values. */
#ifdef TRUE
#undef TRUE
#endif
#ifdef FALSE
#undef FALSE
#endif
#include <rfb/keysym.h>
#ifdef TRUE
#undef TRUE
#endif
#ifdef FALSE
#undef FALSE
#endif
#define FALSE (0)
#define TRUE (!FALSE)

/* -------------------------------------------------------------------------
 * Unicode → keysym
 * ------------------------------------------------------------------------- */

guint rfbsrc_unicode_to_keysym(gunichar ch)
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
  if (ch <= 0xff) {
    return ch;
  }
  return 0x01000000 | ch;
}

/* -------------------------------------------------------------------------
 * DOM KeyboardEvent → keysym
 * ------------------------------------------------------------------------- */

/* Navigation/editing keys with both a standard and a KP keysym.
 * kp_keysym == 0 means no KP equivalent for that key. */
typedef struct {
  const gchar* dom_key;
  guint keysym;
  guint kp_keysym;
} RfbSrcDomNavKey;

static const RfbSrcDomNavKey dom_nav_keys[] = {
    {"Enter", XK_Return, XK_KP_Enter},
    {"Delete", XK_Delete, XK_KP_Delete},
    {"Insert", XK_Insert, XK_KP_Insert},
    {"Home", XK_Home, XK_KP_Home},
    {"End", XK_End, XK_KP_End},
    {"PageUp", XK_Page_Up, XK_KP_Page_Up},
    {"PageDown", XK_Page_Down, XK_KP_Page_Down},
    {"ArrowLeft", XK_Left, XK_KP_Left},
    {"ArrowRight", XK_Right, XK_KP_Right},
    {"ArrowUp", XK_Up, XK_KP_Up},
    {"ArrowDown", XK_Down, XK_KP_Down},
    {"Clear", XK_Clear, XK_KP_Begin},
};

/* Named keys with a single keysym. MUST remain sorted (strcmp order)
 * for bsearch. */
typedef struct {
  const gchar* name;
  guint keysym;
} NamedKey;

static int named_key_cmp(const void* key, const void* entry)
{
  return strcmp((const gchar*)key, ((const NamedKey*)entry)->name);
}

static const NamedKey kNamedKeys[] = {
    {"Again", XK_Redo},
    {"AllCandidates", XK_MultipleCandidate},
    {"Alphanumeric", XK_Eisu_Shift},
    {"AltGraph", XK_ISO_Level3_Shift},
    {"Backspace", XK_BackSpace},
    {"Cancel", XK_Cancel},
    {"CapsLock", XK_Caps_Lock},
    {"Compose", XK_Multi_key},
    {"ContextMenu", XK_Menu},
    {"Convert", XK_Henkan},
    {"Eisu", XK_Eisu_toggle},
    {"Escape", XK_Escape},
    {"Execute", XK_Execute},
    {"Find", XK_Find},
    {"GroupFirst", XK_ISO_First_Group},
    {"GroupLast", XK_ISO_Last_Group},
    {"GroupNext", XK_ISO_Next_Group},
    {"GroupPrevious", XK_ISO_Prev_Group},
    {"Hankaku", XK_Hankaku},
    {"Help", XK_Help},
    {"Hiragana", XK_Hiragana},
    {"HiraganaKatakana", XK_Hiragana_Katakana},
    {"KanaMode", XK_Kana_Lock},
    {"KanjiMode", XK_Kanji},
    {"Katakana", XK_Katakana},
    {"ModeChange", XK_Mode_switch},
    {"NonConvert", XK_Muhenkan},
    {"NumLock", XK_Num_Lock},
    {"Pause", XK_Pause},
    {"PreviousCandidate", XK_PreviousCandidate},
    {"PrintScreen", XK_Print},
    {"Redo", XK_Redo},
    {"Romaji", XK_Romaji},
    {"ScrollLock", XK_Scroll_Lock},
    {"Select", XK_Select},
    {"SingleCandidate", XK_SingleCandidate},
    {"Tab", XK_Tab},
    {"Undo", XK_Undo},
    {"Zenkaku", XK_Zenkaku},
    {"ZenkakuHankaku", XK_Zenkaku_Hankaku},
};

/* L/R modifier keys: location 2 → right variant, otherwise left. */
typedef struct {
  const gchar* name;
  guint keysym_l;
  guint keysym_r;
} LRKey;

static const LRKey kLRKeys[] = {
    {"Alt", XK_Alt_L, XK_Alt_R},       {"Control", XK_Control_L, XK_Control_R},
    {"Hyper", XK_Hyper_L, XK_Hyper_R}, {"Meta", XK_Meta_L, XK_Meta_R},
    {"OS", XK_Super_L, XK_Super_R},    {"Shift", XK_Shift_L, XK_Shift_R},
    {"Super", XK_Super_L, XK_Super_R},
};

guint rfbsrc_dom_key_to_keysym(const gchar* key, gint location)
{
  if (key == NULL || *key == '\0') {
    return 0;
  }

  /* Numpad keys (location == 3) */
  if (location == 3) {
    if (key[1] == '\0') {
      if (key[0] >= '0' && key[0] <= '9') {
        return XK_KP_0 + (guint)(key[0] - '0');
      }
      switch (key[0]) {
        case '.':
          return XK_KP_Decimal;
        case ',':
          return XK_KP_Separator;
        case '/':
          return XK_KP_Divide;
        case '*':
          return XK_KP_Multiply;
        case '-':
          return XK_KP_Subtract;
        case '+':
          return XK_KP_Add;
        default:
          break;
      }
    }
    for (guint i = 0; i < G_N_ELEMENTS(dom_nav_keys); i++) {
      if (strcmp(key, dom_nav_keys[i].dom_key) == 0) {
        return dom_nav_keys[i].kp_keysym;
      }
    }
    /* No KP match; fall through to standard lookups */
  }

  /* Dead / unidentified keys: no mapping */
  if (strncmp(key, "Dead", 4) == 0 || strcmp(key, "Unidentified") == 0) {
    return 0;
  }

  /* Navigation / editing keys at standard location */
  for (guint i = 0; i < G_N_ELEMENTS(dom_nav_keys); i++) {
    if (dom_nav_keys[i].keysym != 0 &&
        strcmp(key, dom_nav_keys[i].dom_key) == 0) {
      return dom_nav_keys[i].keysym;
    }
  }

  /* Named keys: sorted table → O(log n) binary search */
  {
    const NamedKey* nk = bsearch(key, kNamedKeys, G_N_ELEMENTS(kNamedKeys),
                                 sizeof(kNamedKeys[0]), named_key_cmp);
    if (nk != NULL) {
      return nk->keysym;
    }
  }

  /* L/R modifier keys */
  for (guint i = 0; i < G_N_ELEMENTS(kLRKeys); i++) {
    if (strcmp(key, kLRKeys[i].name) == 0) {
      return location == 2 ? kLRKeys[i].keysym_r : kLRKeys[i].keysym_l;
    }
  }

  /* Function keys F1..F35 */
  if (key[0] == 'F' && key[1] != '\0') {
    guint num = 0;
    const gchar* p = key + 1;
    while (*p >= '0' && *p <= '9') {
      num = num * 10 + (guint)(*p++ - '0');
    }
    if (p != key + 1 && *p == '\0' && num >= 1 && num <= 35) {
      return XK_F1 + num - 1;
    }
  }

  /* Single printable Unicode character */
  if (g_utf8_strlen(key, -1) == 1) {
    return rfbsrc_unicode_to_keysym(g_utf8_get_char(key));
  }

  return 0;
}
