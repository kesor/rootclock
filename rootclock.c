/* rootclock - draw a centered time/date on each monitor's portion of the root window. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrender.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#include "config.h"
#include "drw.h"
#include "util.h"

#define UTF_INVALID 0xFFFD
#define MURMUR3_C1 0x21F0AAADu
#define MURMUR3_C2 0xD35A2D97u

/* Constants for validation limits */
#define MAX_MONITORS 64
#define MAX_SCREEN_DIMENSION 32767
#define FALLBACK_TIME "••••"
#define FALLBACK_DATE "Unknown Date"
#define TIME_BUF_SIZE 64
#define DATE_BUF_SIZE 128
#define MIN_UPDATE_INTERVAL_MS 50 /* Minimum 50ms between forced updates */

static volatile sig_atomic_t running = 1;

/* Cached Xinerama monitor information */
static XineramaScreenInfo *cached_monitors = NULL;
static int cached_monitor_count = 0;
static int monitors_dirty = 1; /* force initial query */

/* Time tracking for consistent updates */
static time_t last_displayed_time = 0;
static unsigned long invert_xor_mask = 0;
static int warned_no_wallpaper_pixmap = 0;

static int utf8decode(const char *s_in, long *u, int *err) {
  static const unsigned char lens[] = {
      /* 0XXXX */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      /* 10XXX */ 0, 0, 0, 0, 0, 0, 0, 0, /* invalid */
      /* 110XX */ 2, 2, 2, 2,
      /* 1110X */ 3, 3,
      /* 11110 */ 4,
      /* 11111 */ 0, /* invalid */
  };
  static const unsigned char leading_mask[] = {0x7F, 0x1F, 0x0F, 0x07};
  /* leading_mask[i] corresponds to the bit mask for a UTF-8 sequence of
   * (i + 1) bytes:
   *   leading_mask[0] = 0x7F for 1-byte (ASCII) sequences
   *   leading_mask[1] = 0x1F for 2-byte sequences
   *   leading_mask[2] = 0x0F for 3-byte sequences
   *   leading_mask[3] = 0x07 for 4-byte sequences
   */
  static const unsigned int overlong[] = {0x0, 0x80, 0x0800, 0x10000};

  const unsigned char *s = (const unsigned char *)s_in;
  int len = lens[*s >> 3];
  *u = UTF_INVALID;
  *err = 1;
  if (len == 0)
    return 1;
  long cp = s[0] & leading_mask[len - 1];
  for (int i = 1; i < len; ++i) {
    if (s[i] == '\0' || (s[i] & 0xC0) != 0x80)
      return i;
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if (cp > 0x10FFFF || (cp >> 11) == 0x1B || cp < overlong[len - 1])
    /* (cp >> 11) == 0x1B catches UTF-16 surrogate code points (0xD800–0xDFFF),
     * which are disallowed in UTF-8. */
    return len;

  *err = 0;
  *u = cp;
  return len;
}

static void signal_handler(int sig) {
  (void)sig;
  running = 0;
}

static void update_monitor_cache(Display *dpy) {
  if (cached_monitors) {
    XFree(cached_monitors);
    cached_monitors = NULL;
    cached_monitor_count = 0;
  }

  cached_monitor_count = 1; /* Default fallback */
  if (XineramaIsActive(dpy)) {
    int n;
    XineramaScreenInfo *xi = XineramaQueryScreens(dpy, &n);
    if (xi && n > 0 && n <= MAX_MONITORS) {
      cached_monitors = xi;
      cached_monitor_count = n;
    } else {
      fprintf(stderr, "rootclock: Xinerama query failed or returned invalid "
                      "data, using single screen\n");
      if (xi) {
        XFree(xi);
      }
    }
  }
  monitors_dirty = 0;
}

static Pixmap get_root_pixmap(Display *dpy, Window root) {
  static Atom atom_xrootpmap = None;
  static Atom atom_esetroot = None;
  Atom type = None;
  int format = 0;
  unsigned long nitems = 0, bytes_after = 0;
  unsigned char *data = NULL;
  Pixmap pixmap = None;
  Atom atoms[2];

  if (atom_xrootpmap == None)
    atom_xrootpmap = XInternAtom(dpy, "_XROOTPMAP_ID", False);
  if (atom_esetroot == None)
    atom_esetroot = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);

  atoms[0] = atom_xrootpmap;
  atoms[1] = atom_esetroot;

  for (int i = 0; i < 2; i++) {
    if (atoms[i] == None)
      continue;
    if (XGetWindowProperty(dpy, root, atoms[i], 0L, 1L, False, AnyPropertyType, &type, &format,
                           &nitems, &bytes_after, &data) == Success &&
        data) {
      if (type == XA_PIXMAP && format == 32 && nitems == 1) {
        pixmap = *((Pixmap *)data);
      }
      XFree(data);
      data = NULL;
    }
    if (pixmap != None)
      break;
  }

  return pixmap;
}

static int is_blend_mode(int mode) {
  switch (mode) {
  case BG_MODE_INVERT:
  case BG_MODE_MULTIPLY:
  case BG_MODE_SCREEN:
  case BG_MODE_OVERLAY:
  case BG_MODE_DARKEN:
  case BG_MODE_LIGHTEN:
    return 1;
  default:
    return 0;
  }
}

static XRenderColor clr_to_xrender(const Clr *clr) {
  XRenderColor rc = {0, 0, 0, 0xffff};
  if (!clr)
    return rc;
  rc.red = clr->color.red;
  rc.green = clr->color.green;
  rc.blue = clr->color.blue;
  rc.alpha = clr->color.alpha;
  return rc;
}

static int prepare_background(Drw *drw, Drawable src_drawable, int rx, int ry, unsigned int rw,
                              unsigned int rh, Clr *bg_scm) {
  if (!drw || rw == 0 || rh == 0)
    return 1;

  /* default to solid fill */
  int used_solid = 1;
  XSetFunction(drw->dpy, drw->gc, GXcopy);

  switch (background_mode) {
  case BG_MODE_COPY:
  case BG_MODE_INVERT:
  case BG_MODE_MULTIPLY:
  case BG_MODE_SCREEN:
  case BG_MODE_OVERLAY:
  case BG_MODE_DARKEN:
  case BG_MODE_LIGHTEN: {
    if (src_drawable == None) {
      drw_setscheme(drw, bg_scm);
      drw_rect(drw, rx, ry, rw, rh, 1, 0);
      break;
    }

    used_solid = 0;
    XCopyArea(drw->dpy, src_drawable, drw->drawable, drw->gc, rx, ry, rw, rh, rx, ry);
  } break;
  case BG_MODE_SOLID:
  default:
    drw_setscheme(drw, bg_scm);
    drw_rect(drw, rx, ry, rw, rh, 1, 0);
    break;
  }

  return used_solid;
}

static int compositor_is_active(Display *dpy, int screen) {
  char sel_name[32];
  snprintf(sel_name, sizeof sel_name, "_NET_WM_CM_S%d", screen);
  Atom sel = XInternAtom(dpy, sel_name, False);
  if (sel == None) {
    return 0;
  }
  return XGetSelectionOwner(dpy, sel) != None;
}

static Window create_desktop_window(Display *dpy, int screen, Window root, unsigned int w,
                                    unsigned int h, unsigned long bg_pixel) {
  XSetWindowAttributes swa;
  memset(&swa, 0, sizeof swa);
  swa.override_redirect = True;
  swa.background_pixel = bg_pixel;
  swa.event_mask = ExposureMask;

  Window win = XCreateWindow(dpy, root, 0, 0, w, h, 0, DefaultDepth(dpy, screen), InputOutput,
                             DefaultVisual(dpy, screen),
                             CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
  if (!win) {
    return None;
  }

  Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom type_desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
  if (type_atom != None && type_desktop != None) {
    XChangeProperty(dpy, win, type_atom, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&type_desktop, 1);
  }

  Atom state_atom = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom state_below = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
  if (state_atom != None && state_below != None) {
    XChangeProperty(dpy, win, state_atom, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&state_below, 1);
  }

  XMapWindow(dpy, win);
  XLowerWindow(dpy, win);
  XFlush(dpy);

  return win;
}

static void destroy_desktop_window(Display *dpy, Window *win) {
  if (win && *win != None) {
    XDestroyWindow(dpy, *win);
    *win = None;
  }
}

static Fnt *fontset_xfont_create(Drw *drw, const char *fontname, FcPattern *fontpattern) {
  Fnt *font;
  XftFont *xfont = NULL;
  FcPattern *pattern = NULL;

  if (fontname) {
    if (!(xfont = XftFontOpenName(drw->dpy, drw->screen, fontname))) {
      fprintf(stderr, "error, cannot load font from name: '%s'\n", fontname);
      return NULL;
    }
    if (!(pattern = FcNameParse((FcChar8 *)fontname))) {
      fprintf(stderr, "error, cannot parse font name to pattern: '%s'\n", fontname);
      XftFontClose(drw->dpy, xfont);
      return NULL;
    }
  } else if (fontpattern) {
    if (!(xfont = XftFontOpenPattern(drw->dpy, fontpattern))) {
      fprintf(stderr, "error, cannot load font from pattern.\n");
      return NULL;
    }
  } else {
    die("no font specified.");
  }

  font = ecalloc(1, sizeof(Fnt));
  font->xfont = xfont;
  font->pattern = pattern;
  font->h = xfont->ascent + xfont->descent;
  font->dpy = drw->dpy;

  return font;
}

static void fontset_xfont_free(Fnt *font) {
  if (!font)
    return;
  if (font->pattern)
    FcPatternDestroy(font->pattern);
  XftFontClose(font->dpy, font->xfont);
  free(font);
}

enum DrawTargetType {
  DRAW_TARGET_NORMAL,
  DRAW_TARGET_ALPHA8,
};

static int draw_text_core(Drw *drw, Drawable drawable, Visual *visual, Colormap colormap,
                          enum DrawTargetType target_type, const XftColor *color_override, int x,
                          int y, unsigned int w, unsigned int h, unsigned int lpad,
                          const char *text, int invert, int fill_bg) {
  int ty, ellipsis_x = 0;
  unsigned int tmpw, ew, ellipsis_w = 0, ellipsis_len, hash, h0, h1;
  XftDraw *d = NULL;
  Fnt *usedfont, *curfont, *nextfont;
  int utf8strlen, utf8charlen, utf8err, render = x || y || w || h;
  long utf8codepoint = 0;
  const char *utf8str;
  FcCharSet *fccharset;
  FcPattern *fcpattern;
  FcPattern *match;
  XftResult result;
  int charexists = 0, overflow = 0;
  /* Cache of codepoints known to be missing across the font chain.
   * 128 entries chosen as a small, cache-friendly size that balances hit rate
   * with minimal memory usage. Collision policy is simple overwrite. */
  static unsigned int nomatches[128], ellipsis_width, invalid_width;
  static const char invalid[] = "\xEF\xBF\xBD";

  if (!drw || (render && (!drw->scheme || !w)) || !text || !drw->fonts)
    return 0;

  if (!render) {
    /* width-measurement pass: allow optional clamp via invert (dwm API compat) */
    w = invert ? (unsigned int)invert : ~0U;
  } else {
    if (fill_bg && target_type == DRAW_TARGET_NORMAL) {
      XSetForeground(drw->dpy, drw->gc, drw->scheme[invert ? ColFg : ColBg].pixel);
      XFillRectangle(drw->dpy, drawable, drw->gc, x, y, w, h);
    }
    if (w < lpad)
      return x + w;
    switch (target_type) {
    case DRAW_TARGET_NORMAL:
      d = XftDrawCreate(drw->dpy, drawable, visual, colormap);
      break;
    case DRAW_TARGET_ALPHA8:
      d = XftDrawCreateAlpha(drw->dpy, drawable, 8);
      break;
    }
    x += lpad;
    w -= lpad;
  }

  usedfont = drw->fonts;
  /* Lazy-initialised metrics; this routine mirrors dwm and is intended for
   * single-threaded use. */
  if (!ellipsis_width && render)
    ellipsis_width = drw_fontset_getwidth(drw, "...");
  if (!invalid_width && render)
    invalid_width = drw_fontset_getwidth(drw, invalid);
  while (1) {
    ew = ellipsis_len = utf8err = utf8charlen = utf8strlen = 0;
    utf8str = text;
    nextfont = NULL;
    while (*text) {
      utf8charlen = utf8decode(text, &utf8codepoint, &utf8err);
      charexists = 0;
      for (curfont = drw->fonts; curfont; curfont = curfont->next) {
        charexists = charexists || XftCharExists(drw->dpy, curfont->xfont, utf8codepoint);
        if (charexists) {
          drw_font_getexts(curfont, text, utf8charlen, &tmpw, NULL);
          if (ew + ellipsis_width <= w) {
            ellipsis_x = x + ew;
            ellipsis_w = w - ew;
            ellipsis_len = utf8strlen;
          }

          if (ew + tmpw > w) {
            overflow = 1;
            if (!render)
              x += tmpw;
            else
              utf8strlen = ellipsis_len;
          } else if (curfont == usedfont) {
            text += utf8charlen;
            utf8strlen += utf8err ? 0 : utf8charlen;
            ew += utf8err ? 0 : tmpw;
          } else {
            nextfont = curfont;
          }
          break;
        }
      }

      if (overflow || !charexists || nextfont || utf8err) {
        if (utf8err && utf8charlen > 0)
          text += utf8charlen; /* skip invalid sequence to avoid stalling */
        break;
      } else
        charexists = 0;
    }

    if (utf8strlen) {
      if (render) {
        ty = y + (h - usedfont->h) / 2 + usedfont->xfont->ascent;
        const XftColor *draw_color =
            color_override ? color_override : &drw->scheme[invert ? ColBg : ColFg];
        XftDrawStringUtf8(d, draw_color, usedfont->xfont, x, ty, (XftChar8 *)utf8str, utf8strlen);
      }
      x += ew;
      w -= ew;
    }
    if (utf8err && (!render || invalid_width < w)) {
      if (render)
        draw_text_core(drw, drawable, visual, colormap, target_type, color_override, x, y, w, h, 0,
                       invalid, invert, 0);
      x += invalid_width;
      w -= invalid_width;
    }
    if (render && overflow)
      draw_text_core(drw, drawable, visual, colormap, target_type, color_override, ellipsis_x, y,
                     ellipsis_w, h, 0, "...", invert, 0);

    if (!*text || overflow) {
      break;
    } else if (nextfont) {
      charexists = 0;
      usedfont = nextfont;
    } else {
      charexists = 1;

      hash = (unsigned int)utf8codepoint;
      hash = ((hash >> 16) ^ hash) * MURMUR3_C1;
      hash = ((hash >> 15) ^ hash) * MURMUR3_C2;
      /* Final two rounds of the MurmurHash3 finalizer to mix bits before
       * indexing the small nomatches cache. This decorrelates code points and
       * helps avoid clustering. See Austin Appleby's smhasher project.
       */
      h0 = ((hash >> 15) ^ hash) % LENGTH(nomatches);
      h1 = (hash >> 17) % LENGTH(nomatches);
      if (nomatches[h0] == utf8codepoint || nomatches[h1] == utf8codepoint)
        goto no_match;

      fccharset = FcCharSetCreate();
      FcCharSetAddChar(fccharset, utf8codepoint);

      if (!drw->fonts->pattern) {
        die("the first font in the cache must be loaded from a font string.");
      }

      fcpattern = FcPatternDuplicate(drw->fonts->pattern);
      FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
      FcPatternAddBool(fcpattern, FC_SCALABLE, FcTrue);

      FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
      FcDefaultSubstitute(fcpattern);
      match = XftFontMatch(drw->dpy, drw->screen, fcpattern, &result);

      FcCharSetDestroy(fccharset);
      FcPatternDestroy(fcpattern);

      if (match) {
        usedfont = fontset_xfont_create(drw, NULL, match);
        if (usedfont && XftCharExists(drw->dpy, usedfont->xfont, utf8codepoint)) {
          curfont = drw->fonts;
          while (curfont->next)
            curfont = curfont->next;
          curfont->next = usedfont;
        } else {
          fontset_xfont_free(usedfont);
          nomatches[nomatches[h0] ? h1 : h0] = utf8codepoint;
        no_match:
          usedfont = drw->fonts;
        }
      }
    }
  }
  if (d)
    XftDrawDestroy(d);

  return x + (render ? w : 0);
}

static int draw_text_custom(Drw *drw, int x, int y, unsigned int w, unsigned int h,
                            unsigned int lpad, const char *text, int invert, int fill_bg) {
  if (fill_bg)
    return drw_text(drw, x, y, w, h, lpad, text, invert);

  return draw_text_core(drw, drw->drawable, DefaultVisual(drw->dpy, drw->screen),
                        DefaultColormap(drw->dpy, drw->screen), DRAW_TARGET_NORMAL, NULL, x, y, w,
                        h, lpad, text, invert, 0);
}

static void draw_text_mask(Drw *drw, Pixmap mask, int x, int y, unsigned int w, unsigned int h,
                           const char *text) {
  XRenderColor rc = {0xffff, 0xffff, 0xffff, 0xffff};
  XftColor color;
  color.color = rc;
  color.pixel = 1;
  draw_text_core(drw, mask, NULL, None, DRAW_TARGET_ALPHA8, &color, x, y, w, h, 0, text, 0, 0);
}

static int apply_effect_for_text(Drw *drw, int mode, int text_x, int text_y, unsigned int text_w,
                                 unsigned int text_h, const char *text, Fnt *font,
                                 const Clr *fg_clr) {
  if (!text || !*text || text_w == 0 || text_h == 0 || !drw || !font)
    return 0;

  Display *dpy = drw->dpy;
  /* Blend paths allocate a fresh coverage mask per draw. These regions are
   * large but infrequent compared to the overall render time; if profiling ever
   * shows this as a bottleneck we can pool pixmaps keyed by dimensions. */
  Pixmap mask = XCreatePixmap(dpy, drw->root, text_w, text_h, 8);
  if (!mask)
    return 0;

  GC mask_gc = XCreateGC(dpy, mask, 0, NULL);
  if (!mask_gc) {
    XFreePixmap(dpy, mask);
    return 0;
  }
  XSetForeground(dpy, mask_gc, 0);
  XFillRectangle(dpy, mask, mask_gc, 0, 0, text_w, text_h);
  XFreeGC(dpy, mask_gc);

  Fnt *prev_font = drw->fonts;
  drw_setfontset(drw, font);
  draw_text_mask(drw, mask, 0, 0, text_w, text_h, text);
  drw_setfontset(drw, prev_font);

  int success = 0;
  switch (mode) {
  case BG_MODE_INVERT:
  case BG_MODE_MULTIPLY:
  case BG_MODE_SCREEN:
  case BG_MODE_OVERLAY:
  case BG_MODE_DARKEN:
  case BG_MODE_LIGHTEN: {
    XRenderPictFormat *dst_fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, drw->screen));
    XRenderPictFormat *mask_fmt = XRenderFindStandardFormat(dpy, PictStandardA8);
    if (!dst_fmt || !mask_fmt)
      break;

    Picture dst = XRenderCreatePicture(dpy, drw->drawable, dst_fmt, 0, NULL);
    Picture mask_pic = XRenderCreatePicture(dpy, mask, mask_fmt, 0, NULL);
    Picture src = None;

    XRenderColor rc = clr_to_xrender(fg_clr);
    src = XRenderCreateSolidFill(dpy, &rc);

    int op = PictOpOver;
    switch (mode) {
    case BG_MODE_INVERT:
      op = PictOpDifference;
      break;
    case BG_MODE_MULTIPLY:
      op = PictOpMultiply;
      break;
    case BG_MODE_SCREEN:
      op = PictOpScreen;
      break;
    case BG_MODE_OVERLAY:
      op = PictOpOverlay;
      break;
    case BG_MODE_DARKEN:
      op = PictOpDarken;
      break;
    case BG_MODE_LIGHTEN:
      op = PictOpLighten;
      break;
    default:
      break;
    }

    XRenderComposite(dpy, op, src, mask_pic, dst, 0, 0, 0, 0, text_x, text_y, text_w, text_h);

    if (src != None)
      XRenderFreePicture(dpy, src);
    XRenderFreePicture(dpy, mask_pic);
    XRenderFreePicture(dpy, dst);
    success = 1;
  } break;
  default:
    break;
  }

  XFreePixmap(dpy, mask);
  return success;
}

static void draw_block_for_region(Drw *drw, Window target_win, int rx, int ry, int rw, int rh,
                                  Fnt *tf, Fnt *df, int show_date_flag, Clr *bg_scm, Clr *time_scm,
                                  Clr *date_scm, const char *tstr, const char *dstr, int block_yoff,
                                  int spacing, Pixmap wallpaper_pm) {
  if (show_date_flag && (!df || !date_scm || !dstr)) {
    fprintf(stderr, "rootclock: invalid parameters for date display\n");
    return;
  }

  int time_h = tf->h;
  if (!tf->xfont) {
    fprintf(stderr, "rootclock: invalid font configuration\n");
    return;
  }
  int ascent_t = tf->xfont->ascent;

  int date_h = 0;
  if (show_date_flag && df) {
    date_h = df->h;
  }

  int total_h = time_h + (show_date_flag ? (spacing + date_h) : 0);
  int base_y = ry + (rh - total_h) / 2 + ascent_t + block_yoff;

  drw_setfontset(drw, tf);
  unsigned int tw = drw_fontset_getwidth(drw, tstr);

  unsigned int dw = 0;
  int date_top = 0;
  int has_date = show_date_flag && df && dstr && *dstr;
  if (has_date) {
    drw_setfontset(drw, df);
    dw = drw_fontset_getwidth(drw, dstr);
    date_top = base_y + (tf->h - ascent_t) + spacing;
  }
  drw_setfontset(drw, tf);

  int time_top = base_y - ascent_t;
  int block_top = time_top;
  int block_bottom = time_top + time_h;
  if (has_date) {
    int date_bottom = date_top + date_h;
    if (date_top < block_top)
      block_top = date_top;
    if (date_bottom > block_bottom)
      block_bottom = date_bottom;
  }

  unsigned int block_w = tw;
  if (dw > block_w)
    block_w = dw;
  block_w += 2U * (unsigned int)block_padding_x;
  if (block_w > (unsigned int)rw)
    block_w = rw;

  int block_x = rx + ((int)rw - (int)block_w) / 2;
  if (block_x < rx)
    block_x = rx;
  if (block_x + (int)block_w > rx + rw) {
    block_x = rx;
    block_w = rw;
  }

  int region_bottom = ry + rh;
  int block_y = block_top - block_padding_y;
  if (block_y < ry)
    block_y = ry;
  int bottom_with_padding = block_bottom + block_padding_y;
  if (bottom_with_padding > region_bottom)
    bottom_with_padding = region_bottom;
  unsigned int block_h =
      bottom_with_padding > block_y ? (unsigned int)(bottom_with_padding - block_y) : 0;

  Drawable src_drawable = 0;
  if (background_mode != BG_MODE_SOLID) {
    if (wallpaper_pm != None) {
      src_drawable = wallpaper_pm;
    } else if (background_mode == BG_MODE_COPY || is_blend_mode(background_mode)) {
      src_drawable = drw->root;
    } else if (!warned_no_wallpaper_pixmap) {
      fprintf(stderr, "rootclock: wallpaper pixmap not available; falling back to "
                      "solid background\n");
      warned_no_wallpaper_pixmap = 1;
    }
  }

  int fill_bg =
      prepare_background(drw, src_drawable, rx, ry, (unsigned int)rw, (unsigned int)rh, bg_scm);

  drw_setfontset(drw, tf);
  drw_setscheme(drw, time_scm);
  int tx = block_x + ((int)block_w - (int)tw) / 2;
  if (tx < rx)
    tx = rx;

  int dx = 0;
  if (has_date) {
    dx = block_x + ((int)block_w - (int)dw) / 2;
    if (dx < rx)
      dx = rx;
  }

  int skip_text_draw = 0;
  if (is_blend_mode(background_mode) && block_w > 0 && block_h > 0) {
    int time_done = apply_effect_for_text(drw, background_mode, tx, time_top, tw, time_h, tstr, tf,
                                          &time_scm[ColFg]);
    int date_done = 1;
    if (has_date) {
      date_done = apply_effect_for_text(drw, background_mode, dx, date_top, dw, date_h, dstr, df,
                                        &date_scm[ColFg]);
    }
    if (time_done && date_done)
      skip_text_draw = 1;
  }

  if (!skip_text_draw) {
    drw_setscheme(drw, time_scm);
    draw_text_custom(drw, tx, time_top, tw, time_h, 0, tstr, 0, fill_bg);

    if (has_date) {
      drw_setfontset(drw, df);
      drw_setscheme(drw, date_scm);
      draw_text_custom(drw, dx, date_top, dw, date_h, 0, dstr, 0, fill_bg);
    }
  }

  drw_map(drw, target_win, rx, ry, rw, rh);
}

static void render_all(Drw *drw, Fnt *tf, Fnt *df, int show_date_flag, Clr *bg_scm, Clr *time_scm,
                       Clr *date_scm, const char *time_fmt_s, const char *date_fmt_s,
                       int block_y_off_s, int line_spacing_s, Window target_win) {
  char tbuf[TIME_BUF_SIZE], dbuf[DATE_BUF_SIZE];
  time_t now = time(NULL);

  if (now == (time_t)-1) {
    fprintf(stderr, "rootclock: time() failed, unable to get current time\n");
    exit(1);
  }

  /* Update last_displayed_time for consistent tracking */
  last_displayed_time = now;

  struct tm *tm_info = localtime(&now);
  if (!tm_info) {
    fprintf(stderr, "rootclock: localtime() failed, unable to format time\n");
    exit(1);
  }
  if (strftime(tbuf, sizeof tbuf, time_fmt_s, tm_info) == 0) {
    /* strftime failed or buffer too small, use fallback */
    snprintf(tbuf, sizeof tbuf, "%s", FALLBACK_TIME);
  }

  if (show_date_flag) {
    if (strftime(dbuf, sizeof dbuf, date_fmt_s, tm_info) == 0) {
      /* strftime failed or buffer too small, use fallback */
      snprintf(dbuf, sizeof dbuf, "%s", FALLBACK_DATE);
    }
  }

  XineramaScreenInfo *xi = NULL;
  int nmon = 1;
  Pixmap wallpaper_pm = get_root_pixmap(drw->dpy, drw->root);

  /* Use cached monitor information */
  if (monitors_dirty) {
    update_monitor_cache(drw->dpy);
  }

  xi = cached_monitors;
  nmon = cached_monitor_count;

  if (xi) {
    for (int i = 0; i < nmon; i++) {
      int rx = xi[i].x_org, ry = xi[i].y_org, rw = xi[i].width, rh = xi[i].height;
      if (rw <= 0 || rh <= 0 || rw > MAX_SCREEN_DIMENSION || rh > MAX_SCREEN_DIMENSION) {
        continue;
      }
      draw_block_for_region(drw, target_win, rx, ry, rw, rh, tf, df, show_date_flag, bg_scm,
                            time_scm, date_scm, tbuf, show_date_flag ? dbuf : NULL, block_y_off_s,
                            line_spacing_s, wallpaper_pm);
    }
  } else {
    int rw = DisplayWidth(drw->dpy, drw->screen);
    int rh = DisplayHeight(drw->dpy, drw->screen);
    draw_block_for_region(drw, target_win, 0, 0, rw, rh, tf, df, show_date_flag, bg_scm, time_scm,
                          date_scm, tbuf, show_date_flag ? dbuf : NULL, block_y_off_s,
                          line_spacing_s, wallpaper_pm);
  }
}

int main(void) {
  setlocale(LC_ALL, "");

  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fputs("rootclock: cannot open display\n", stderr);
    return 1;
  }
  int screen = DefaultScreen(dpy);
  Window root = RootWindow(dpy, screen);
  Window draw_win = root;
  Window desktop_win = None;
  int compositor_active = compositor_is_active(dpy, screen);

  XWindowAttributes root_attr;
  if (XGetWindowAttributes(dpy, root, &root_attr) && root_attr.visual) {
    invert_xor_mask =
        root_attr.visual->red_mask | root_attr.visual->green_mask | root_attr.visual->blue_mask;
  } else {
    invert_xor_mask = 0x00ffffff;
  }

  unsigned int rw = DisplayWidth(dpy, screen);
  unsigned int rh = DisplayHeight(dpy, screen);
  if (rw == 0 || rh == 0 || rw > MAX_SCREEN_DIMENSION || rh > MAX_SCREEN_DIMENSION) {
    fprintf(stderr, "rootclock: invalid display dimensions %ux%u\n", rw, rh);
    XCloseDisplay(dpy);
    return 1;
  }
  Drw *drw = drw_create(dpy, screen, root, rw, rh);
  if (!drw) {
    fprintf(stderr, "rootclock: failed to create drawing context\n");
    XCloseDisplay(dpy);
    return 1;
  }

  Fnt *tf = drw_fontset_create(drw, time_fonts, LENGTH(time_fonts));
  Fnt *df = show_date ? drw_fontset_create(drw, date_fonts, LENGTH(date_fonts)) : NULL;
  if (!tf || (show_date && !df))
    die("rootclock: failed to load fonts");

  /* color schemes:
     index order: ColFg, ColBg, ColBorder
     We'll use:
       - bg_scm: fg=bg_color to fill background rectangles
       - time_scm: fg=time_color, bg=bg_color
       - date_scm: fg=date_color, bg=bg_color
  */
  const char *bg_names[] = {bg_color, bg_color, bg_color};
  const char *time_names[] = {time_color, bg_color, bg_color};
  const char *date_names[] = {date_color, bg_color, bg_color};
  Clr *bg_scm = drw_scm_create(drw, bg_names, 3);
  Clr *time_scm = drw_scm_create(drw, time_names, 3);
  Clr *date_scm = drw_scm_create(drw, date_names, 3);
  if (!bg_scm || !time_scm || !date_scm)
    die("rootclock: color alloc failed");

  unsigned long bg_pixel = XBlackPixel(dpy, screen);
  if (compositor_active) {
    desktop_win = create_desktop_window(dpy, screen, root, rw, rh, bg_pixel);
    if (desktop_win != None) {
      draw_win = desktop_win;
      XSelectInput(dpy, desktop_win, ExposureMask);
    } else {
      compositor_active = 0;
      fprintf(stderr, "rootclock: compositor detected but failed to create "
                      "background window, falling back to root drawing\n");
    }
  }

  XSelectInput(dpy, root, ExposureMask | StructureNotifyMask);

  /* loop: redraw on expose/resize and on timer ticks */
  int xfd = ConnectionNumber(dpy);
  int need_redraw = 1;
  while (running) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      switch (ev.type) {
      case Expose:
        if (ev.xexpose.window == root || ev.xexpose.window == draw_win)
          need_redraw = 1;
        break;
      case ConfigureNotify: {
        unsigned int nrw = DisplayWidth(dpy, screen);
        unsigned int nrh = DisplayHeight(dpy, screen);
        if (nrw != drw->w || nrh != drw->h)
          drw_resize(drw, nrw, nrh);
        if (desktop_win != None && (nrw != rw || nrh != rh)) {
          XResizeWindow(dpy, desktop_win, nrw, nrh);
          XLowerWindow(dpy, desktop_win);
        }
        rw = nrw;
        rh = nrh;
        monitors_dirty = 1; /* mark monitors as needing refresh */
        need_redraw = 1;
      } break;
      default:
        break;
      }
    }

    int compositor_now = compositor_is_active(dpy, screen);
    if (compositor_now && desktop_win == None) {
      desktop_win = create_desktop_window(dpy, screen, root, rw, rh, bg_pixel);
      if (desktop_win != None) {
        draw_win = desktop_win;
        XSelectInput(dpy, desktop_win, ExposureMask);
        need_redraw = 1;
      }
    } else if (!compositor_now && desktop_win != None) {
      destroy_desktop_window(dpy, &desktop_win);
      draw_win = root;
      need_redraw = 1;
    }
    compositor_active = compositor_now;

    /* Check if time has changed (for second-precise updates) */
    time_t current_time = time(NULL);
    if (current_time != last_displayed_time && current_time != (time_t)-1) {
      need_redraw = 1;
    }

    if (need_redraw) {
      render_all(drw, tf, df, show_date, bg_scm, time_scm, date_scm, time_fmt, date_fmt,
                 block_y_off, line_spacing, draw_win);
      need_redraw = 0;
    }

    /* Calculate timeout for next update - align to appropriate time boundaries */
    struct timeval tv;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
      if (refresh_sec == 1) {
        /* For 1-second updates, use precise second-boundary alignment */
        long usec_in_sec = (ts.tv_nsec / 1000) % 1000000;

        if (usec_in_sec < 950000) {
          /* We're not too close to the next second, wait until 50ms before it */
          tv.tv_sec = 0;
          tv.tv_usec = (950000 - usec_in_sec);
        } else {
          /* We're very close to or past 950ms mark, wait for next second + 50ms */
          tv.tv_sec = 0;
          tv.tv_usec = (1050000 - usec_in_sec);
          if (tv.tv_usec < 0)
            tv.tv_usec = 0;
        }
      } else {
        /* For longer intervals, align to time boundaries based on refresh_sec */
        time_t current_time = ts.tv_sec;
        time_t next_boundary;

        if (refresh_sec >= 3600) {
          /* Hourly or longer: align to hour boundaries */
          struct tm *tm_info = localtime(&current_time);
          if (tm_info) {
            tm_info->tm_sec = 0;
            tm_info->tm_min = 0;
            tm_info->tm_hour++;
            next_boundary = mktime(tm_info);
          } else {
            next_boundary = current_time + refresh_sec;
          }
        } else if (refresh_sec >= 60) {
          /* Minute-level intervals: align to minute boundaries */
          struct tm *tm_info = localtime(&current_time);
          if (tm_info) {
            tm_info->tm_sec = 0;
            /* For refresh_sec like 59, we want next minute boundary */
            /* For refresh_sec like 120, we want appropriate minute alignment */
            int minute_interval = (refresh_sec + 30) / 60; /* round to nearest minute */
            tm_info->tm_min = ((tm_info->tm_min / minute_interval) + 1) * minute_interval;
            next_boundary = mktime(tm_info);
          } else {
            next_boundary = current_time + refresh_sec;
          }
        } else {
          /* Short intervals: align to second boundaries with refresh_sec spacing */
          next_boundary = ((current_time / refresh_sec) + 1) * refresh_sec;
        }

        time_t wait_time = next_boundary - current_time;
        if (wait_time <= 0) {
          wait_time = 1; /* minimum wait */
        }

        /* Wake up 50ms before the boundary for smooth updates */
        if (wait_time > 1) {
          tv.tv_sec = wait_time - 1;
          tv.tv_usec = 950000; /* 950ms into the previous second */
        } else {
          tv.tv_sec = 0;
          tv.tv_usec = wait_time * 1000000 - 50000; /* 50ms before */
          if (tv.tv_usec < 0) {
            tv.tv_sec = 0;
            tv.tv_usec = 0;
          }
        }
      }
    } else {
      /* Fallback to simple periodic updates */
      tv.tv_sec = refresh_sec;
      tv.tv_usec = 0;
    }

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    int r = select(xfd + 1, &fds, NULL, NULL, &tv);
    if (r == 0) {
      need_redraw = 1; /* timeout - force redraw */
    } else if ((r < 0 && errno != EINTR) || !running) {
      break;
    }
  }

  free(bg_scm);
  free(time_scm);
  free(date_scm);
  if (cached_monitors)
    XFree(cached_monitors);
  destroy_desktop_window(dpy, &desktop_win);
  if (drw)
    drw_free(drw);
  XCloseDisplay(dpy);

  return 0;
}
