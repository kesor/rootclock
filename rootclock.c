/* rootclock - draw a centered time/date on each monitor's portion of the root window. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#include "config.h"

static int xft_color_alloc(Display *dpy, int screen, const char *hex,
                           XftColor *out) {
  Visual *visual = DefaultVisual(dpy, screen);
  Colormap colormap = DefaultColormap(dpy, screen);
  return XftColorAllocName(dpy, visual, colormap, hex, out);
}

/* Draw a filled rect + centered time (+ optional date) as one vertical block */
static void draw_clock_block(Display *dpy, XftDraw *xft, GC gc, int screen,
                             int rx, int ry, int rw, int rh, XftFont *tf,
                             XftFont *df, int show_date_flag, XftColor *bgc,
                             XftColor *tcol, XftColor *dcol, const char *tbuf,
                             const char *dbuf, int block_yoff, int spacing) {
  /* background */
  XSetForeground(dpy, gc, bgc->pixel);
  XFillRectangle(dpy, RootWindow(dpy, screen), gc, rx, ry, (unsigned)rw,
                 (unsigned)rh);

  /* measure verticals */
  const int time_h = tf->ascent + tf->descent;
  const int date_h = (show_date_flag && df) ? (df->ascent + df->descent) : 0;
  const int total_h = time_h + (show_date_flag ? (spacing + date_h) : 0);

  /* baseline for the time line */
  int base_y = ry + (rh - total_h) / 2 + tf->ascent + block_yoff;

  /* center time horizontally using advance width */
  XGlyphInfo ext;
  XftTextExtentsUtf8(dpy, tf, (const FcChar8 *)tbuf, (int)strlen(tbuf), &ext);
  int x = rx + (rw - (int)ext.xOff) / 2;
  XftDrawStringUtf8(xft, tcol, tf, x, base_y, (const FcChar8 *)tbuf,
                    (int)strlen(tbuf));

  if (show_date_flag && df && dbuf && *dbuf) {
    /* baseline for date: below time’s descent + spacing */
    int date_y = base_y + tf->descent + spacing + df->ascent;

    XGlyphInfo dext;
    XftTextExtentsUtf8(dpy, df, (const FcChar8 *)dbuf, (int)strlen(dbuf),
                       &dext);
    int dx = rx + (rw - (int)dext.xOff) / 2;
    XftDrawStringUtf8(xft, dcol, df, dx, date_y, (const FcChar8 *)dbuf,
                      (int)strlen(dbuf));
  }
}

static void render_all(Display *dpy, int screen, XftFont *tf, XftFont *df,
                       int show_date_flag, XftColor *bgc, XftColor *tcol,
                       XftColor *dcol, const char *time_fmt_s,
                       const char *date_fmt_s, int block_y_off_s,
                       int line_spacing_s) {
  Window root = RootWindow(dpy, screen);
  Visual *visual = DefaultVisual(dpy, screen);
  Colormap colormap = DefaultColormap(dpy, screen);

  /* Compose time/date strings once per frame */
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  char tbuf[64], dbuf[128];
  strftime(tbuf, sizeof tbuf, time_fmt_s, tm_info);
  if (show_date_flag)
    strftime(dbuf, sizeof dbuf, date_fmt_s, tm_info);

  XftDraw *xft = XftDrawCreate(dpy, root, visual, colormap);
  if (!xft)
    return;
  GC gc = DefaultGC(dpy, screen);

  /* Query monitors */
  XineramaScreenInfo *xi = NULL;
  int nmon = 1;
  if (XineramaIsActive(dpy)) {
    int n;
    xi = XineramaQueryScreens(dpy, &n);
    if (xi && n > 0)
      nmon = n;
  }

  if (xi) {
    for (int i = 0; i < nmon; i++) {
      int rx = xi[i].x_org, ry = xi[i].y_org, rw = xi[i].width,
          rh = xi[i].height;
      draw_clock_block(dpy, xft, gc, screen, rx, ry, rw, rh, tf, df,
                       show_date_flag, bgc, tcol, dcol, tbuf,
                       show_date_flag ? dbuf : NULL, block_y_off_s,
                       line_spacing_s);
    }
  } else {
    int rw = DisplayWidth(dpy, screen);
    int rh = DisplayHeight(dpy, screen);
    draw_clock_block(dpy, xft, gc, screen, 0, 0, rw, rh, tf, df, show_date_flag,
                     bgc, tcol, dcol, tbuf, show_date_flag ? dbuf : NULL,
                     block_y_off_s, line_spacing_s);
  }

  XFlush(dpy);
  XftDrawDestroy(xft);
  if (xi)
    XFree(xi);
}

int main(void) {
  setlocale(LC_ALL, "");

  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fputs("rootclock: cannot open display\n", stderr);
    return 1;
  }
  int screen = DefaultScreen(dpy);
  Window root = RootWindow(dpy, screen);

  /* Select events (after screen/root are valid) */
  XSelectInput(dpy, root, ExposureMask | StructureNotifyMask);

  /* Fonts */
  XftFont *tf = XftFontOpenName(dpy, screen, time_font);
  XftFont *df = show_date ? XftFontOpenName(dpy, screen, date_font) : NULL;
  if (!tf || (show_date && !df)) {
    fputs("rootclock: failed to load fonts\n", stderr);
    return 1;
  }

  /* Colors */
  XftColor tcol, dcol, bgc;
  if (!xft_color_alloc(dpy, screen, time_color, &tcol) ||
      (show_date && !xft_color_alloc(dpy, screen, date_color, &dcol)) ||
      !xft_color_alloc(dpy, screen, bg_color, &bgc)) {
    fputs("rootclock: failed to alloc colors\n", stderr);
    return 1;
  }

  /* Event + timer loop:
     - redraw immediately on Expose/ConfigureNotify
     - also tick every refresh_sec for time changes
  */
  int xfd = ConnectionNumber(dpy);
  int need_redraw = 1; /* draw once at start */
  while (1) {
    /* Drain any pending events */
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      switch (ev.type) {
      case Expose:
      case ConfigureNotify:
        need_redraw = 1;
        break;
      default:
        break;
      }
    }

    if (need_redraw) {
      render_all(dpy, screen, tf, df, show_date, &bgc, &tcol, &dcol, time_fmt,
                 date_fmt, block_y_off, line_spacing);
      need_redraw = 0;
    }

    /* Wait for either: next X event OR the next tick */
    struct timeval tv;
    tv.tv_sec = refresh_sec;
    tv.tv_usec = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    int r = select(xfd + 1, &fds, NULL, NULL, &tv);
    if (r == 0) {
      /* timer fired */
      need_redraw = 1;
    } /* if r > 0: events will be processed in the next loop */
  }

  /* Unreachable in normal use, kept for completeness */
  Visual *visual = DefaultVisual(dpy, screen);
  Colormap colormap = DefaultColormap(dpy, screen);
  XftColorFree(dpy, visual, colormap, &tcol);
  if (show_date)
    XftColorFree(dpy, visual, colormap, &dcol);
  XftColorFree(dpy, visual, colormap, &bgc);
  XftFontClose(dpy, tf);
  if (df)
    XftFontClose(dpy, df);
  XCloseDisplay(dpy);
  return 0;
}
