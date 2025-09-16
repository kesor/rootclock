/* rootclock - draw a centered time/date on each monitor's portion of the root window. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
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

/* Constants for validation limits */
#define MAX_MONITORS 64
#define MAX_SCREEN_DIMENSION 32767
#define FALLBACK_TIME "••••"
#define FALLBACK_DATE "Unknown Date"
#define TIME_BUF_SIZE 64
#define DATE_BUF_SIZE 128
#define MIN_UPDATE_INTERVAL_MS 50 /* Minimum 50ms between forced updates */

/* X11 Atoms for wallpaper handling (picom compatibility) */
static Atom _XROOTPMAP_ID = None;
static Atom ESETROOT_PMAP_ID = None;

static volatile sig_atomic_t running = 1;

/* Cached Xinerama monitor information */
static XineramaScreenInfo *cached_monitors = NULL;
static int cached_monitor_count = 0;
static int monitors_dirty = 1; /* force initial query */

/* Time tracking for consistent updates */
static time_t last_displayed_time = 0;

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

/* Initialize wallpaper-related X11 atoms */
static void init_wallpaper_atoms(Display *dpy) {
  _XROOTPMAP_ID = XInternAtom(dpy, "_XROOTPMAP_ID", False);
  ESETROOT_PMAP_ID = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);
}

/* Get existing wallpaper pixmap from root window properties */
static Pixmap get_wallpaper_pixmap(Display *dpy, Window root) {
  Atom type;
  int format;
  unsigned long nitems, bytes_after;
  unsigned char *prop = NULL;
  Pixmap pixmap = None;

  /* Try _XROOTPMAP_ID first */
  if (XGetWindowProperty(dpy, root, _XROOTPMAP_ID, 0L, 1L, False,
                         XA_PIXMAP, &type, &format, &nitems, &bytes_after,
                         &prop) == Success && prop) {
    if (type == XA_PIXMAP && format == 32 && nitems == 1) {
      pixmap = *((Pixmap *)prop);
    }
    XFree(prop);
  }

  /* If that failed, try ESETROOT_PMAP_ID */
  if (pixmap == None) {
    prop = NULL;
    if (XGetWindowProperty(dpy, root, ESETROOT_PMAP_ID, 0L, 1L, False,
                           XA_PIXMAP, &type, &format, &nitems, &bytes_after,
                           &prop) == Success && prop) {
      if (type == XA_PIXMAP && format == 32 && nitems == 1) {
        pixmap = *((Pixmap *)prop);
      }
      XFree(prop);
    }
  }

  return pixmap;
}

/* Set the wallpaper pixmap and update root window properties */
static void set_wallpaper_pixmap(Display *dpy, Window root, Pixmap pixmap) {
  /* Set the root window background */
  XSetWindowBackgroundPixmap(dpy, root, pixmap);
  XClearWindow(dpy, root);

  /* Update the properties to notify compositors */
  XChangeProperty(dpy, root, _XROOTPMAP_ID, XA_PIXMAP, 32,
                  PropModeReplace, (unsigned char *)&pixmap, 1);
  XChangeProperty(dpy, root, ESETROOT_PMAP_ID, XA_PIXMAP, 32,
                  PropModeReplace, (unsigned char *)&pixmap, 1);

  /* Kill the previous pixmap to avoid leaks (following Esetroot convention) */
  XKillClient(dpy, AllTemporary);
}

/* Draw clock text on region with semi-transparent background for wallpaper visibility */
static void draw_clock_for_region(Drw *drw, int rx, int ry, int rw, int rh,
                                  Fnt *tf, Fnt *df, int show_date_flag,
                                  Clr *bg_scm, Clr *time_scm, Clr *date_scm,
                                  const char *tstr, const char *dstr,
                                  int block_yoff, int spacing) {
  if (show_date_flag && (!df || !date_scm || !dstr)) {
    fprintf(stderr, "rootclock: invalid parameters for date display\n");
    return;
  }

  drw_setfontset(drw, tf);
  unsigned int tw = drw_fontset_getwidth(drw, tstr);
  int tx = rx + (rw - (int)tw) / 2;

  /* Calculate time and date heights */
  unsigned int time_h = tf->h;
  unsigned int date_h = (show_date_flag && df) ? df->h : 0;
  unsigned int total_h = time_h + (show_date_flag ? (spacing + date_h) : 0);

  /* Vertical positioning with block offset */
  int block_center_y = ry + rh / 2 + block_yoff;
  int base_y = block_center_y - (int)total_h / 2;

  /* Ensure the text placement is within bounds, even if region is too small */
  if (rh < (int)time_h) {
    base_y = ry;
  } else {
    if (base_y < ry) base_y = ry;
    if (base_y + (int)time_h > ry + rh) base_y = ry + rh - (int)time_h;
  }

  /* Calculate text positioning */  
  int ascent_t = tf->xfont ? tf->xfont->ascent : (int)time_h * 3 / 4;
  int ty = base_y;
  
  /* Draw a subtle background rectangle behind the text for better contrast */
  int padding = 8;
  int bg_x = tx - padding;
  int bg_y = ty - padding;
  int bg_w = tw + 2 * padding;
  int bg_h = time_h + 2 * padding;
  
  if (show_date_flag && df && dstr && *dstr) {
    drw_setfontset(drw, df);
    unsigned int dw = drw_fontset_getwidth(drw, dstr);
    int dx = rx + (rw - (int)dw) / 2;
    int min_x = (dx - padding < bg_x) ? dx - padding : bg_x;
    int max_x = ((dx + (int)dw + padding) > (bg_x + bg_w)) ? dx + (int)dw + padding : bg_x + bg_w;
    bg_x = min_x;
    bg_w = max_x - min_x;
    bg_h += spacing + date_h + padding;
  }
  
  /* Draw semi-transparent background */
  drw_setscheme(drw, bg_scm);
  drw_rect(drw, bg_x, bg_y, bg_w, bg_h, 1, 0);
  
  /* Draw time text */
  drw_setscheme(drw, time_scm);
  drw_text(drw, tx, ty, tw, time_h, 0, tstr, 0);

  if (show_date_flag && df && dstr && *dstr) {
    drw_setfontset(drw, df);
    unsigned int dw = drw_fontset_getwidth(drw, dstr);
    int dx = rx + (rw - (int)dw) / 2;
    int date_top = base_y + (tf->h - ascent_t) + spacing; /* baseline gap */
    int dy = date_top;
    drw_setscheme(drw, date_scm);
    drw_text(drw, dx, dy, dw, date_h, 0, dstr, 0);
  }
}

static void render_all(Drw *drw, Fnt *tf, Fnt *df, int show_date_flag,
                       Clr *bg_scm, Clr *time_scm, Clr *date_scm,
                       const char *time_fmt_s, const char *date_fmt_s,
                       int block_y_off_s, int line_spacing_s) {
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

  /* Get the existing wallpaper pixmap to use as background */
  Pixmap wallpaper_pixmap = get_wallpaper_pixmap(drw->dpy, drw->root);
  
  /* Copy wallpaper to our drawing surface if available */
  if (wallpaper_pixmap != None) {
    /* Copy the wallpaper pixmap as background - handle potential size differences */
    unsigned int pixmap_w, pixmap_h, pixmap_border, pixmap_depth;
    Window pixmap_root;
    if (XGetGeometry(drw->dpy, wallpaper_pixmap, &pixmap_root,
                     NULL, NULL, &pixmap_w, &pixmap_h, &pixmap_border, &pixmap_depth)) {
      unsigned int copy_w = drw->w < pixmap_w ? drw->w : pixmap_w;
      unsigned int copy_h = drw->h < pixmap_h ? drw->h : pixmap_h;
      XCopyArea(drw->dpy, wallpaper_pixmap, drw->drawable, drw->gc,
                0, 0, copy_w, copy_h, 0, 0);
      /* If the drawable is larger than the pixmap, fill the rest with background color */
      if (copy_w < drw->w || copy_h < drw->h) {
        drw_setscheme(drw, bg_scm);
        if (copy_w < drw->w)
          drw_rect(drw, copy_w, 0, drw->w - copy_w, drw->h, 1, 0);
        if (copy_h < drw->h)
          drw_rect(drw, 0, copy_h, drw->w, drw->h - copy_h, 1, 0);
      }
    } else {
      /* Failed to get pixmap geometry, fallback to background color */
      drw_setscheme(drw, bg_scm);
      drw_rect(drw, 0, 0, drw->w, drw->h, 1, 0);
    }
  } else {
    /* No wallpaper found, fill with background color */
    drw_setscheme(drw, bg_scm);
    drw_rect(drw, 0, 0, drw->w, drw->h, 1, 0);
  }

  XineramaScreenInfo *xi = NULL;
  int nmon = 1;

  /* Use cached monitor information */
  if (monitors_dirty) {
    update_monitor_cache(drw->dpy);
  }

  xi = cached_monitors;
  nmon = cached_monitor_count;

  /* Draw clock on each monitor region */
  if (xi) {
    for (int i = 0; i < nmon; i++) {
      int rx = xi[i].x_org, ry = xi[i].y_org, rw = xi[i].width,
          rh = xi[i].height;
      if (rw <= 0 || rh <= 0 || rw > MAX_SCREEN_DIMENSION ||
          rh > MAX_SCREEN_DIMENSION) {
        continue;
      }
      /* Draw clock on the wallpaper surface */
      draw_clock_for_region(drw, rx, ry, rw, rh, tf, df, show_date_flag,
                            bg_scm, time_scm, date_scm, tbuf,
                            show_date_flag ? dbuf : NULL, block_y_off_s,
                            line_spacing_s);
    }
  } else {
    int rw = DisplayWidth(drw->dpy, drw->screen);
    int rh = DisplayHeight(drw->dpy, drw->screen);
    draw_clock_for_region(drw, 0, 0, rw, rh, tf, df, show_date_flag,
                          bg_scm, time_scm, date_scm, tbuf,
                          show_date_flag ? dbuf : NULL, block_y_off_s,
                          line_spacing_s);
  }

  /* Set our rendered pixmap as the new wallpaper for picom compatibility */
  set_wallpaper_pixmap(drw->dpy, drw->root, drw->drawable);
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

  unsigned int rw = DisplayWidth(dpy, screen);
  unsigned int rh = DisplayHeight(dpy, screen);
  if (rw == 0 || rh == 0 || rw > MAX_SCREEN_DIMENSION ||
      rh > MAX_SCREEN_DIMENSION) {
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

  /* Initialize wallpaper atoms for picom compatibility */
  init_wallpaper_atoms(dpy);

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
        need_redraw = 1;
        break;
      case ConfigureNotify: {
        unsigned int nrw = DisplayWidth(dpy, screen);
        unsigned int nrh = DisplayHeight(dpy, screen);
        if (nrw != drw->w || nrh != drw->h)
          drw_resize(drw, nrw, nrh);
        monitors_dirty = 1; /* mark monitors as needing refresh */
        need_redraw = 1;
      } break;
      default:
        break;
      }
    }

    /* Check if time has changed (for second-precise updates) */
    time_t current_time = time(NULL);
    if (current_time != last_displayed_time && current_time != (time_t)-1) {
      need_redraw = 1;
    }

    if (need_redraw) {
      render_all(drw, tf, df, show_date, bg_scm, time_scm, date_scm, time_fmt,
                 date_fmt, block_y_off, line_spacing);
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
        time_t boundary_time = ts.tv_sec;
        time_t next_boundary;

        if (refresh_sec >= 3600) {
          /* Hourly or longer: align to hour boundaries */
          struct tm tm_boundary_buf;
          struct tm *tm_boundary_ptr = localtime(&boundary_time);
          if (tm_boundary_ptr) {
            tm_boundary_buf = *tm_boundary_ptr; /* Copy to local storage */
            tm_boundary_buf.tm_sec = 0;
            tm_boundary_buf.tm_min = 0;
            tm_boundary_buf.tm_hour++;
            next_boundary = mktime(&tm_boundary_buf);
          } else {
            next_boundary = boundary_time + refresh_sec;
          }
        } else if (refresh_sec >= 60) {
          /* Minute-level intervals: align to minute boundaries */
          struct tm tm_minute_buf;
          struct tm *tm_minute_ptr = localtime(&boundary_time);
          if (tm_minute_ptr) {
            tm_minute_buf = *tm_minute_ptr; /* Copy to local storage */
            tm_minute_buf.tm_sec = 0;
            /* For refresh_sec like 59, we want next minute boundary */
            /* For refresh_sec like 120, we want appropriate minute alignment */
            int minute_interval =
                (refresh_sec + 30) / 60; /* round to nearest minute */
            tm_minute_buf.tm_min =
                ((tm_minute_buf.tm_min / minute_interval) + 1) * minute_interval;
            next_boundary = mktime(&tm_minute_buf);
          } else {
            next_boundary = boundary_time + refresh_sec;
          }
        } else {
          /* Short intervals: align to second boundaries with refresh_sec spacing */
          next_boundary = ((boundary_time / refresh_sec) + 1) * refresh_sec;
        }

        time_t wait_time = next_boundary - boundary_time;
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
  if (drw)
    drw_free(drw);
  XCloseDisplay(dpy);

  return 0;
}
