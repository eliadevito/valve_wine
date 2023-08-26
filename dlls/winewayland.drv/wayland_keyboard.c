/*
 * Keyboard related functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd.
 * Copyright 2023 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"
#include <stdlib.h>

#include <linux/input.h>
#undef SW_MAX /* Also defined in winuser.rh */
#include <sys/mman.h>
#include <unistd.h>

#include "waylanddrv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(keyboard);
WINE_DECLARE_DEBUG_CHANNEL(key);

struct layout
{
    struct list entry;
    char *xkb_layout;

    int xkb_group;
    LANGID lang;
    WORD index;
    /* "Layout Id", used by NtUserGetKeyboardLayoutName / LoadKeyboardLayoutW */
    WORD layout_id;
};

/* These are only used from the wayland event thread and don't need locking */
static struct list xkb_layouts = LIST_INIT(xkb_layouts);
static struct rxkb_context *rxkb_context;
static HKL keyboard_hkl; /* the HKL matching the currently active xkb group */

static WORD key2scan(UINT key)
{
    /* base keys can be mapped directly */
    if (key <= KEY_KPDOT) return key;

    /* map keys found in KBDTABLES definitions (Txx Xxx Yxx macros) */
    switch (key)
    {
    case 84 /* ISO_Level3_Shift */: return 0x005a; /* T5A / VK_OEM_WSCTRL */
    case KEY_SYSRQ: return 0x0054; /* T54 / VK_SNAPSHOT */
    case KEY_102ND: return 0x0056; /* T56 / VK_OEM_102 */
    case KEY_F11: return 0x0057; /* T57 / VK_F11 */
    case KEY_F12: return 0x0058; /* T58 / VK_F12 */
    case KEY_LINEFEED: return 0x0059; /* T59 / VK_CLEAR */
    case KEY_EXIT: return 0x005b; /* T5B / VK_OEM_FINISH */
    case KEY_OPEN: return 0x005c; /* T5C / VK_OEM_JUMP */
    /* FIXME: map a KEY to T5D / VK_EREOF */
    /* FIXME: map a KEY to T5E / VK_OEM_BACKTAB */
    case KEY_COMPOSE: return 0x005f; /* T5F / VK_OEM_AUTO */
    case KEY_SCALE: return 0x0062; /* T62 / VK_ZOOM */
    case KEY_HELP: return 0x0063; /* T63 / VK_HELP */
    case KEY_F13: return 0x0064; /* T64 / VK_F13 */
    case KEY_F14: return 0x0065; /* T65 / VK_F14 */
    case KEY_F15: return 0x0066; /* T66 / VK_F15 */
    case KEY_F16: return 0x0067; /* T67 / VK_F16 */
    case KEY_F17: return 0x0068; /* T68 / VK_F17 */
    case KEY_F18: return 0x0069; /* T69 / VK_F18 */
    case KEY_F19: return 0x006a; /* T6A / VK_F19 */
    case KEY_F20: return 0x006b; /* T6B / VK_F20 */
    case KEY_F21: return 0x006c; /* T6C / VK_F21 */
    case KEY_F22: return 0x006d; /* T6D / VK_F22 */
    case KEY_F23: return 0x006e; /* T6E / VK_F23 */
    /* FIXME: map a KEY to T6F / VK_OEM_PA3 */
    case KEY_COMPUTER: return 0x0071; /* T71 / VK_OEM_RESET */
    /* FIXME: map a KEY to T73 / VK_ABNT_C1 */
    case KEY_F24: return 0x0076; /* T76 / VK_F24 */
    case KEY_KPPLUSMINUS: return 0x007b; /* T7B / VK_OEM_PA1 */
    /* FIXME: map a KEY to T7C / VK_TAB */
    /* FIXME: map a KEY to T7E / VK_ABNT_C2 */
    /* FIXME: map a KEY to T7F / VK_OEM_PA2 */
    case KEY_PREVIOUSSONG: return 0x0110; /* X10 / VK_MEDIA_PREV_TRACK */
    case KEY_NEXTSONG: return 0x0119; /* X19 / VK_MEDIA_NEXT_TRACK */
    case KEY_KPENTER: return 0x011c; /* X1C / VK_RETURN */
    case KEY_RIGHTCTRL: return 0x011d; /* X1D / VK_RCONTROL */
    case KEY_MUTE: return 0x0120; /* X20 / VK_VOLUME_MUTE */
    case KEY_PROG2: return 0x0121; /* X21 / VK_LAUNCH_APP2 */
    case KEY_PLAYPAUSE: return 0x0122; /* X22 / VK_MEDIA_PLAY_PAUSE */
    case KEY_STOPCD: return 0x0124; /* X24 / VK_MEDIA_STOP */
    case KEY_VOLUMEDOWN: return 0x012e; /* X2E / VK_VOLUME_DOWN */
    case KEY_VOLUMEUP: return 0x0130; /* X30 / VK_VOLUME_UP */
    case KEY_HOMEPAGE: return 0x0132; /* X32 / VK_BROWSER_HOME */
    case KEY_KPSLASH: return 0x0135; /* X35 / VK_DIVIDE */
    case KEY_PRINT: return 0x0137; /* X37 / VK_SNAPSHOT */
    case KEY_RIGHTALT: return 0x0138; /* X38 / VK_RMENU */
    case KEY_CANCEL: return 0x0146; /* X46 / VK_CANCEL */
    case KEY_HOME: return 0x0147; /* X47 / VK_HOME */
    case KEY_UP: return 0x0148; /* X48 / VK_UP */
    case KEY_PAGEUP: return 0x0149; /* X49 / VK_PRIOR */
    case KEY_LEFT: return 0x014b; /* X4B / VK_LEFT */
    case KEY_RIGHT: return 0x014d; /* X4D / VK_RIGHT */
    case KEY_END: return 0x014f; /* X4F / VK_END */
    case KEY_DOWN: return 0x0150; /* X50 / VK_DOWN */
    case KEY_PAGEDOWN: return 0x0151; /* X51 / VK_NEXT */
    case KEY_INSERT: return 0x0152; /* X52 / VK_INSERT */
    case KEY_DELETE: return 0x0153; /* X53 / VK_DELETE */
    case KEY_LEFTMETA: return 0x015b; /* X5B / VK_LWIN */
    case KEY_RIGHTMETA: return 0x015c; /* X5C / VK_RWIN */
    case KEY_MENU: return 0x015d; /* X5D / VK_APPS */
    case KEY_POWER: return 0x015e; /* X5E / VK_POWER */
    case KEY_SLEEP: return 0x015f; /* X5F / VK_SLEEP */
    case KEY_FIND: return 0x0165; /* X65 / VK_BROWSER_SEARCH */
    case KEY_BOOKMARKS: return 0x0166; /* X66 / VK_BROWSER_FAVORITES */
    case KEY_REFRESH: return 0x0167; /* X67 / VK_BROWSER_REFRESH */
    case KEY_STOP: return 0x0168; /* X68 / VK_BROWSER_STOP */
    case KEY_FORWARD: return 0x0169; /* X69 / VK_BROWSER_FORWARD */
    case KEY_BACK: return 0x016a; /* X6A / VK_BROWSER_BACK */
    case KEY_PROG1: return 0x016b; /* X6B / VK_LAUNCH_APP1 */
    case KEY_MAIL: return 0x016c; /* X6C / VK_LAUNCH_MAIL */
    case KEY_MEDIA: return 0x016d; /* X6D / VK_LAUNCH_MEDIA_SELECT */
    case KEY_PAUSE: return 0x021d; /* Y1D / VK_PAUSE */
    }

    /* otherwise just make up some extended scancode */
    return 0x200 | (key & 0x7f);
}

static inline LANGID langid_from_xkb_layout(const char *layout, size_t layout_len)
{
#define MAKEINDEX(c0, c1) (MAKEWORD(c0, c1) - MAKEWORD('a', 'a'))
    static const LANGID langids[] =
    {
        [MAKEINDEX('a','f')] = MAKELANGID(LANG_DARI, SUBLANG_DEFAULT),
        [MAKEINDEX('a','l')] = MAKELANGID(LANG_ALBANIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('a','m')] = MAKELANGID(LANG_ARMENIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('a','t')] = MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN_AUSTRIAN),
        [MAKEINDEX('a','z')] = MAKELANGID(LANG_AZERBAIJANI, SUBLANG_DEFAULT),
        [MAKEINDEX('a','u')] = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_AUS),
        [MAKEINDEX('b','a')] = MAKELANGID(LANG_BOSNIAN, SUBLANG_BOSNIAN_BOSNIA_HERZEGOVINA_CYRILLIC),
        [MAKEINDEX('b','d')] = MAKELANGID(LANG_BANGLA, SUBLANG_DEFAULT),
        [MAKEINDEX('b','e')] = MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH_BELGIAN),
        [MAKEINDEX('b','g')] = MAKELANGID(LANG_BULGARIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('b','r')] = MAKELANGID(LANG_PORTUGUESE, 2),
        [MAKEINDEX('b','t')] = MAKELANGID(LANG_TIBETAN, 3),
        [MAKEINDEX('b','w')] = MAKELANGID(LANG_TSWANA, SUBLANG_TSWANA_BOTSWANA),
        [MAKEINDEX('b','y')] = MAKELANGID(LANG_BELARUSIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('c','a')] = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_CAN),
        [MAKEINDEX('c','d')] = MAKELANGID(LANG_FRENCH, SUBLANG_CUSTOM_UNSPECIFIED),
        [MAKEINDEX('c','h')] = MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN_SWISS),
        [MAKEINDEX('c','m')] = MAKELANGID(LANG_FRENCH, 11),
        [MAKEINDEX('c','n')] = MAKELANGID(LANG_CHINESE, SUBLANG_DEFAULT),
        [MAKEINDEX('c','z')] = MAKELANGID(LANG_CZECH, SUBLANG_DEFAULT),
        [MAKEINDEX('d','e')] = MAKELANGID(LANG_GERMAN, SUBLANG_DEFAULT),
        [MAKEINDEX('d','k')] = MAKELANGID(LANG_DANISH, SUBLANG_DEFAULT),
        [MAKEINDEX('d','z')] = MAKELANGID(LANG_TAMAZIGHT, SUBLANG_TAMAZIGHT_ALGERIA_LATIN),
        [MAKEINDEX('e','e')] = MAKELANGID(LANG_ESTONIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('e','s')] = MAKELANGID(LANG_SPANISH, SUBLANG_DEFAULT),
        [MAKEINDEX('e','t')] = MAKELANGID(LANG_AMHARIC, SUBLANG_DEFAULT),
        [MAKEINDEX('f','i')] = MAKELANGID(LANG_FINNISH, SUBLANG_DEFAULT),
        [MAKEINDEX('f','o')] = MAKELANGID(LANG_FAEROESE, SUBLANG_DEFAULT),
        [MAKEINDEX('f','r')] = MAKELANGID(LANG_FRENCH, SUBLANG_DEFAULT),
        [MAKEINDEX('g','b')] = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_UK),
        [MAKEINDEX('g','e')] = MAKELANGID(LANG_GEORGIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('g','h')] = MAKELANGID(LANG_ENGLISH, SUBLANG_CUSTOM_UNSPECIFIED),
        [MAKEINDEX('g','n')] = MAKELANGID(LANG_NEUTRAL, SUBLANG_CUSTOM_DEFAULT),
        [MAKEINDEX('g','r')] = MAKELANGID(LANG_GREEK, SUBLANG_DEFAULT),
        [MAKEINDEX('h','r')] = MAKELANGID(LANG_CROATIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('h','u')] = MAKELANGID(LANG_HUNGARIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('i','d')] = MAKELANGID(LANG_INDONESIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('i','e')] = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_EIRE),
        [MAKEINDEX('i','l')] = MAKELANGID(LANG_HEBREW, SUBLANG_DEFAULT),
        [MAKEINDEX('i','n')] = MAKELANGID(LANG_HINDI, SUBLANG_DEFAULT),
        [MAKEINDEX('i','q')] = MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_IRAQ),
        [MAKEINDEX('i','r')] = MAKELANGID(LANG_PERSIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('i','s')] = MAKELANGID(LANG_ICELANDIC, SUBLANG_DEFAULT),
        [MAKEINDEX('i','t')] = MAKELANGID(LANG_ITALIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('j','p')] = MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT),
        [MAKEINDEX('k','e')] = MAKELANGID(LANG_NEUTRAL, SUBLANG_CUSTOM_DEFAULT),
        [MAKEINDEX('k','g')] = MAKELANGID(LANG_KYRGYZ, SUBLANG_DEFAULT),
        [MAKEINDEX('k','h')] = MAKELANGID(LANG_KHMER, SUBLANG_DEFAULT),
        [MAKEINDEX('k','r')] = MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT),
        [MAKEINDEX('k','z')] = MAKELANGID(LANG_KAZAK, SUBLANG_DEFAULT),
        [MAKEINDEX('l','a')] = MAKELANGID(LANG_LAO, SUBLANG_DEFAULT),
        [MAKEINDEX('l','k')] = MAKELANGID(LANG_SINHALESE, SUBLANG_DEFAULT),
        [MAKEINDEX('l','t')] = MAKELANGID(LANG_LITHUANIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('l','v')] = MAKELANGID(LANG_LATVIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('m','a')] = MAKELANGID(LANG_ARABIC, SUBLANG_ARABIC_MOROCCO),
        [MAKEINDEX('m','d')] = MAKELANGID(LANG_ROMANIAN, SUBLANG_CUSTOM_UNSPECIFIED),
        [MAKEINDEX('m','e')] = MAKELANGID(LANG_SERBIAN, SUBLANG_SERBIAN_MONTENEGRO_LATIN),
        [MAKEINDEX('m','k')] = MAKELANGID(LANG_MACEDONIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('m','l')] = MAKELANGID(LANG_NEUTRAL, SUBLANG_CUSTOM_DEFAULT),
        [MAKEINDEX('m','m')] = MAKELANGID(0x55 /*LANG_BURMESE*/, SUBLANG_DEFAULT),
        [MAKEINDEX('m','n')] = MAKELANGID(LANG_MONGOLIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('m','t')] = MAKELANGID(LANG_MALTESE, SUBLANG_DEFAULT),
        [MAKEINDEX('m','v')] = MAKELANGID(LANG_DIVEHI, SUBLANG_DEFAULT),
        [MAKEINDEX('m','y')] = MAKELANGID(LANG_MALAY, SUBLANG_DEFAULT),
        [MAKEINDEX('n','g')] = MAKELANGID(LANG_ENGLISH, SUBLANG_CUSTOM_UNSPECIFIED),
        [MAKEINDEX('n','l')] = MAKELANGID(LANG_DUTCH, SUBLANG_DEFAULT),
        [MAKEINDEX('n','o')] = MAKELANGID(LANG_NORWEGIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('n','p')] = MAKELANGID(LANG_NEPALI, SUBLANG_DEFAULT),
        [MAKEINDEX('p','h')] = MAKELANGID(LANG_FILIPINO, SUBLANG_DEFAULT),
        [MAKEINDEX('p','k')] = MAKELANGID(LANG_URDU, SUBLANG_DEFAULT),
        [MAKEINDEX('p','l')] = MAKELANGID(LANG_POLISH, SUBLANG_DEFAULT),
        [MAKEINDEX('p','t')] = MAKELANGID(LANG_PORTUGUESE, SUBLANG_DEFAULT),
        [MAKEINDEX('r','o')] = MAKELANGID(LANG_ROMANIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('r','s')] = MAKELANGID(LANG_SERBIAN, SUBLANG_SERBIAN_LATIN),
        [MAKEINDEX('r','u')] = MAKELANGID(LANG_RUSSIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('s','e')] = MAKELANGID(LANG_SWEDISH, SUBLANG_DEFAULT),
        [MAKEINDEX('s','i')] = MAKELANGID(LANG_SLOVENIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('s','k')] = MAKELANGID(LANG_SLOVAK, SUBLANG_DEFAULT),
        [MAKEINDEX('s','n')] = MAKELANGID(LANG_WOLOF, SUBLANG_DEFAULT),
        [MAKEINDEX('s','y')] = MAKELANGID(LANG_SYRIAC, SUBLANG_DEFAULT),
        [MAKEINDEX('t','g')] = MAKELANGID(LANG_FRENCH, SUBLANG_CUSTOM_UNSPECIFIED),
        [MAKEINDEX('t','h')] = MAKELANGID(LANG_THAI, SUBLANG_DEFAULT),
        [MAKEINDEX('t','j')] = MAKELANGID(LANG_TAJIK, SUBLANG_DEFAULT),
        [MAKEINDEX('t','m')] = MAKELANGID(LANG_TURKMEN, SUBLANG_DEFAULT),
        [MAKEINDEX('t','r')] = MAKELANGID(LANG_TURKISH, SUBLANG_DEFAULT),
        [MAKEINDEX('t','w')] = MAKELANGID(LANG_CHINESE, SUBLANG_CUSTOM_UNSPECIFIED),
        [MAKEINDEX('t','z')] = MAKELANGID(LANG_SWAHILI, SUBLANG_CUSTOM_UNSPECIFIED),
        [MAKEINDEX('u','a')] = MAKELANGID(LANG_UKRAINIAN, SUBLANG_DEFAULT),
        [MAKEINDEX('u','s')] = MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
        [MAKEINDEX('u','z')] = MAKELANGID(LANG_UZBEK, 2),
        [MAKEINDEX('v','n')] = MAKELANGID(LANG_VIETNAMESE, SUBLANG_DEFAULT),
        [MAKEINDEX('z','a')] = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_SOUTH_AFRICA),
    };
    LANGID langid;

    if (layout_len == 2 && (langid = langids[MAKEINDEX(layout[0], layout[1])])) return langid;
    if (layout_len == 3 && !memcmp(layout, "ara", layout_len)) return MAKELANGID(LANG_ARABIC, SUBLANG_DEFAULT);
    if (layout_len == 3 && !memcmp(layout, "epo", layout_len)) return MAKELANGID(LANG_NEUTRAL, SUBLANG_CUSTOM_DEFAULT);
    if (layout_len == 3 && !memcmp(layout, "mao", layout_len)) return MAKELANGID(LANG_MAORI, SUBLANG_DEFAULT);
    if (layout_len == 4 && !memcmp(layout, "brai", layout_len)) return MAKELANGID(LANG_NEUTRAL, SUBLANG_CUSTOM_DEFAULT);
    if (layout_len == 5 && !memcmp(layout, "latam", layout_len)) return MAKELANGID(LANG_SPANISH, SUBLANG_CUSTOM_UNSPECIFIED);
#undef MAKEINDEX

    FIXME("Unknown layout language %s\n", debugstr_a(layout));
    return MAKELANGID(LANG_NEUTRAL, SUBLANG_CUSTOM_UNSPECIFIED);
};

static HKL get_layout_hkl(struct layout *layout, LCID locale)
{
    if (!layout->layout_id) return (HKL)(UINT_PTR)MAKELONG(locale, layout->lang);
    else return (HKL)(UINT_PTR)MAKELONG(locale, 0xf000 | layout->layout_id);
}

static void add_xkb_layout(const char *xkb_layout, xkb_layout_index_t xkb_group, LANGID lang)
{
    static WORD next_layout_id = 1;

    struct layout *layout;
    unsigned int len;
    WORD index = 0;
    char *ptr;

    TRACE("xkb_layout=%s xkb_group=%u lang=%04x\n", xkb_layout, xkb_group, lang);

    LIST_FOR_EACH_ENTRY(layout, &xkb_layouts, struct layout, entry)
        if (layout->lang == lang) index++;

    len = strlen(xkb_layout) + 1;
    if (!(layout = calloc(1, sizeof(*layout) + len)))
    {
        ERR("Failed to allocate memory for Xkb layout entry\n");
        return;
    }
    ptr = (char *)(layout + 1);

    layout->xkb_layout = strcpy(ptr, xkb_layout);
    layout->xkb_group = xkb_group;
    layout->lang = lang;
    layout->index = index;
    if (index) layout->layout_id = next_layout_id++;

    TRACE("Created layout entry=%p index=%04x lang=%04x id=%04x\n", layout, layout->index, layout->lang, layout->layout_id);
    list_add_tail(&xkb_layouts, &layout->entry);
}

static void set_current_xkb_group(xkb_layout_index_t xkb_group)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    LCID locale = LOWORD(NtUserGetKeyboardLayout(0));
    struct layout *layout;
    HKL hkl;

    LIST_FOR_EACH_ENTRY(layout, &xkb_layouts, struct layout, entry)
        if (layout->xkb_group == xkb_group) break;
    if (&layout->entry != &xkb_layouts)
        hkl = get_layout_hkl(layout, locale);
    else
    {
        ERR("Failed to find Xkb Layout for group %d\n", xkb_group);
        hkl = keyboard_hkl;
    }

    if (hkl == keyboard_hkl) return;
    keyboard_hkl = hkl;

    TRACE("Changing keyboard layout to %p\n", hkl);
    NtUserPostMessage(keyboard->focused_hwnd, WM_INPUTLANGCHANGEREQUEST, 0 /*FIXME*/,
                      (LPARAM)keyboard_hkl);
}

static BOOL find_xkb_layout_variant(const char *name, const char **layout, const char **variant)
{
    struct rxkb_layout *iter;

    for (iter = rxkb_layout_first(rxkb_context); iter; iter = rxkb_layout_next(iter))
    {
        if (!strcmp(name, rxkb_layout_get_description(iter)))
        {
            *layout = rxkb_layout_get_name(iter);
            *variant = rxkb_layout_get_variant(iter);
            return TRUE;
        }
    }

    return FALSE;
}

/**********************************************************************
 *          Keyboard handling
 */

static HWND wayland_keyboard_get_focused_hwnd(void)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    HWND hwnd;

    pthread_mutex_lock(&keyboard->mutex);
    hwnd = keyboard->focused_hwnd;
    pthread_mutex_unlock(&keyboard->mutex);

    return hwnd;
}

static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t format, int fd, uint32_t size)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    struct xkb_keymap *xkb_keymap = NULL;
    xkb_layout_index_t xkb_group;
    struct xkb_state *xkb_state;
    struct layout *entry, *next;
    char *keymap_str;

    TRACE("format=%d fd=%d size=%d\n", format, fd, size);

    if ((keymap_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)) != MAP_FAILED)
    {
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
            FIXME("Unsupported keymap format %#x\n", format);
        else
        {
            xkb_keymap = xkb_keymap_new_from_string(keyboard->xkb_context, keymap_str,
                                                    XKB_KEYMAP_FORMAT_TEXT_V1, 0);
        }

        munmap(keymap_str, size);
    }

    close(fd);

    if (!xkb_keymap)
    {
        ERR("Failed to load Xkb keymap\n");
        return;
    }

    LIST_FOR_EACH_ENTRY_SAFE(entry, next, &xkb_layouts, struct layout, entry)
    {
        list_remove(&entry->entry);
        free(entry);
    }

    for (xkb_group = 0; xkb_group < xkb_keymap_num_layouts(xkb_keymap); xkb_group++)
    {
        const char *layout_name = xkb_keymap_layout_get_name(xkb_keymap, xkb_group);
        const char *layout, *variant = NULL;
        int layout_len, variant_len = 0;
        char buffer[1024];
        LANGID lang;

        if (!find_xkb_layout_variant(layout_name, &layout, &variant)) layout = "us";
        if (variant) variant_len = strlen(variant);
        layout_len = strlen(layout);

        TRACE("Found layout %u name %s -> %s:%s\n", xkb_group, layout_name, layout, variant);

        lang = langid_from_xkb_layout(layout, layout_len);
        snprintf(buffer, ARRAY_SIZE(buffer), "%.*s:%.*s", layout_len, layout, variant_len, variant);
        add_xkb_layout(buffer, xkb_group, lang);
    }

    if ((xkb_state = xkb_state_new(xkb_keymap)))
    {
        pthread_mutex_lock(&keyboard->mutex);
        xkb_state_unref(keyboard->xkb_state);
        keyboard->xkb_state = xkb_state;
        pthread_mutex_unlock(&keyboard->mutex);

        set_current_xkb_group(0);
    }

    xkb_keymap_unref(xkb_keymap);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *wl_surface,
                                  struct wl_array *keys)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    HWND hwnd;

    if (!wl_surface) return;

    /* The wl_surface user data remains valid and immutable for the whole
     * lifetime of the object, so it's safe to access without locking. */
    hwnd = wl_surface_get_user_data(wl_surface);
    TRACE("serial=%u hwnd=%p\n", serial, hwnd);

    pthread_mutex_lock(&keyboard->mutex);
    keyboard->focused_hwnd = hwnd;
    pthread_mutex_unlock(&keyboard->mutex);

    NtUserPostMessage(keyboard->focused_hwnd, WM_INPUTLANGCHANGEREQUEST, 0 /*FIXME*/,
                      (LPARAM)keyboard_hkl);
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *wl_surface)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    HWND hwnd;

    if (!wl_surface) return;

    /* The wl_surface user data remains valid and immutable for the whole
     * lifetime of the object, so it's safe to access without locking. */
    hwnd = wl_surface_get_user_data(wl_surface);
    TRACE("serial=%u hwnd=%p\n", serial, hwnd);

    pthread_mutex_lock(&keyboard->mutex);
    if (keyboard->focused_hwnd == hwnd)
        keyboard->focused_hwnd = NULL;
    pthread_mutex_unlock(&keyboard->mutex);

    /* FIXME: update foreground window as well */
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    UINT scan = key2scan(key);
    INPUT input = {0};
    HWND hwnd;

    if (!(hwnd = wayland_keyboard_get_focused_hwnd())) return;

    TRACE_(key)("serial=%u hwnd=%p key=%d scan=%#x state=%#x\n", serial, hwnd, key, scan, state);

    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scan & 0xff;
    input.ki.wVk = NtUserMapVirtualKeyEx(scan, MAPVK_VSC_TO_VK_EX, keyboard_hkl);
    if (scan & ~0xff) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    __wine_send_input(hwnd, &input, NULL);
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t xkb_group)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;

    if (!wayland_keyboard_get_focused_hwnd()) return;

    TRACE("serial=%u mods_depressed=%#x mods_latched=%#x mods_locked=%#x xkb_group=%d stub!\n",
          serial, mods_depressed, mods_latched, mods_locked, xkb_group);

    pthread_mutex_lock(&keyboard->mutex);
    xkb_state_update_mask(keyboard->xkb_state, mods_depressed, mods_latched,
                          mods_locked, 0, 0, xkb_group);
    pthread_mutex_unlock(&keyboard->mutex);

    set_current_xkb_group(xkb_group);

    /* FIXME: Sync wine modifier state with XKB modifier state. */
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                                        int rate, int delay)
{
    UINT speed;

    TRACE("rate=%d delay=%d\n", rate, delay);

    /* Handle non-negative rate values, ignore invalid (negative) values.  A
     * rate of 0 disables repeat. */
    if (rate >= 80) speed = 31;
    else if (rate >= 5) speed = rate * 400 / 1000 - 1;
    else speed = 0;

    delay = max(0, min(3, round(delay / 250.0) - 1));
    NtUserSystemParametersInfo(SPI_SETKEYBOARDSPEED, speed, NULL, 0);
    NtUserSystemParametersInfo(SPI_SETKEYBOARDDELAY, delay, NULL, 0);
    NtUserCallOneParam(rate > 0, NtUserCallOneParam_SetKeyboardAutoRepeat);
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

/***********************************************************************
 *           wayland_keyboard_init
 */
void wayland_keyboard_init(struct wl_keyboard *wl_keyboard)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    struct xkb_context *xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (!(rxkb_context = rxkb_context_new(RXKB_CONTEXT_NO_FLAGS))
            || !rxkb_context_parse_default_ruleset(rxkb_context))
    {
        ERR("Failed to parse default Xkb ruleset\n");
        return;
    }

    if (!xkb_context)
    {
        ERR("Failed to create XKB context\n");
        return;
    }

    NtUserCallOneParam(TRUE, NtUserCallOneParam_SetKeyboardAutoRepeat);
    pthread_mutex_lock(&keyboard->mutex);
    keyboard->wl_keyboard = wl_keyboard;
    keyboard->xkb_context = xkb_context;
    pthread_mutex_unlock(&keyboard->mutex);
    wl_keyboard_add_listener(keyboard->wl_keyboard, &keyboard_listener, NULL);
}

/***********************************************************************
 *           wayland_keyboard_deinit
 */
void wayland_keyboard_deinit(void)
{
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;

    pthread_mutex_lock(&keyboard->mutex);
    if (keyboard->wl_keyboard)
    {
        wl_keyboard_destroy(keyboard->wl_keyboard);
        keyboard->wl_keyboard = NULL;
    }
    if (keyboard->xkb_context)
    {
        xkb_context_unref(keyboard->xkb_context);
        keyboard->xkb_context = NULL;
    }
    if (keyboard->xkb_state)
    {
        xkb_state_unref(keyboard->xkb_state);
        keyboard->xkb_state = NULL;
    }
    pthread_mutex_unlock(&keyboard->mutex);

    if (rxkb_context)
    {
        rxkb_context_unref(rxkb_context);
        rxkb_context = NULL;
    }
}