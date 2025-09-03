/* rootclock - draw a centered time/date on each monitor's portion of the root window. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <fontconfig/fontconfig.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#include "config.h"
#include "drw.h"
#include "util.h"

static void draw_block_for_region(Drw *drw, int rx, int ry, int rw, int rh,
                                  Fnt *tf, Fnt *df, int show_date_flag,
                                  Clr *bg_scm, Clr *time_scm, Clr *date_scm,
                                  const char *tstr, const char *dstr,
                                  int block_yoff, int spacing) {
  /* fill background */
  drw_setscheme(drw, bg_scm);
  drw_rect(drw, rx, ry, rw, rh, 1, 0);

  /* metrics */
  int time_h = tf->h;
  int ascent_t = tf->xfont->ascent;

  int date_h = 0;
  if (show_date_flag && df) {
    date_h = df->h;
  }

  int total_h = time_h + (show_date_flag ? (spacing + date_h) : 0);
  int base_y = ry + (rh - total_h) / 2 + ascent_t + block_yoff;

  /* time width + centered x */
  drw_setfontset(drw, tf);
  unsigned int tw = drw_fontset_getwidth(drw, tstr);
  int tx = rx + (rw - (int)tw) / 2;

  /* drw_text y is the top of the text box; it centers within h.
     To place baseline at base_y, give box height = time_h and y = base_y -
     ascent_t */
  drw_setscheme(drw, time_scm);
  drw_text(drw, tx, base_y - ascent_t, tw, time_h, 0, tstr, 0);

  if (show_date_flag && df && dstr && *dstr) {
    drw_setfontset(drw, df);
    unsigned int dw = drw_fontset_getwidth(drw, dstr);
    int dx = rx + (rw - (int)dw) / 2;
    int date_top = base_y + (tf->h - ascent_t) + spacing; /* baseline gap */
    int dy = date_top;
    drw_setscheme(drw, date_scm);
    drw_text(drw, dx, dy, dw, date_h, 0, dstr, 0);
  }

  /* copy this region to root */
  drw_map(drw, drw->root, rx, ry, rw, rh);
}

static void render_all(Drw *drw, Fnt *tf, Fnt *df, int show_date_flag,
                       Clr *bg_scm, Clr *time_scm, Clr *date_scm,
                       const char *time_fmt_s, const char *date_fmt_s,
                       int block_y_off_s, int line_spacing_s) {
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  char tbuf[64], dbuf[128];
  strftime(tbuf, sizeof tbuf, time_fmt_s, tm_info);
  if (show_date_flag)
    strftime(dbuf, sizeof dbuf, date_fmt_s, tm_info);

  /* monitors */
  XineramaScreenInfo *xi = NULL;
  int nmon = 1;
  if (XineramaIsActive(drw->dpy)) {
    int n;
    xi = XineramaQueryScreens(drw->dpy, &n);
    if (xi && n > 0)
      nmon = n;
  }

  if (xi) {
    for (int i = 0; i < nmon; i++) {
      int rx = xi[i].x_org, ry = xi[i].y_org, rw = xi[i].width,
          rh = xi[i].height;
      draw_block_for_region(drw, rx, ry, rw, rh, tf, df, show_date_flag, bg_scm,
                            time_scm, date_scm, tbuf,
                            show_date_flag ? dbuf : NULL, block_y_off_s,
                            line_spacing_s);
    }
    XFree(xi);
  } else {
    int rw = DisplayWidth(drw->dpy, drw->screen);
    int rh = DisplayHeight(drw->dpy, drw->screen);
    draw_block_for_region(
        drw, 0, 0, rw, rh, tf, df, show_date_flag, bg_scm, time_scm, date_scm,
        tbuf, show_date_flag ? dbuf : NULL, block_y_off_s, line_spacing_s);
  }
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

  /* drw + initial size */
  unsigned int rw = DisplayWidth(dpy, screen);
  unsigned int rh = DisplayHeight(dpy, screen);
  Drw *drw = drw_create(dpy, screen, root, rw, rh);
  if (!drw)
    return 1;

  /* fonts */
  Fnt *tf = drw_fontset_create(drw, time_fonts, LENGTH(time_fonts));
  Fnt *df = show_date ? drw_fontset_create(drw, date_fonts, LENGTH(date_fonts))
                      : NULL;
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

  /* events */
  XSelectInput(dpy, root, ExposureMask | StructureNotifyMask);

  /* loop: redraw on expose/resize and on timer ticks */
  int xfd = ConnectionNumber(dpy);
  int need_redraw = 1;
  for (;;) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      switch (ev.type) {
      case Expose:
        need_redraw = 1;
        break;
      case ConfigureNotify: {
        /* root size changed */
        unsigned int nrw = DisplayWidth(dpy, screen);
        unsigned int nrh = DisplayHeight(dpy, screen);
        if (nrw != drw->w || nrh != drw->h)
          drw_resize(drw, nrw, nrh);
        need_redraw = 1;
      } break;
      default:
        break;
      }
    }

    if (need_redraw) {
      render_all(drw, tf, df, show_date, bg_scm, time_scm, date_scm, time_fmt,
                 date_fmt, block_y_off, line_spacing);
      need_redraw = 0;
    }

    struct timeval tv = {.tv_sec = refresh_sec, .tv_usec = 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    int r = select(xfd + 1, &fds, NULL, NULL, &tv);
    if (r == 0)
      need_redraw = 1; /* tick */
  }

  /* never reached */
  return 0;
}
