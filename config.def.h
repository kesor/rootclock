/* Appearance */
static const char *bg_color   = "#000000";

/* Time (1st line) */
static const char *time_font  = "Liberation Sans:style=Bold:size=120";
static const char *time_color = "#ffffff";
static const char *time_fmt   = "%H:%M";

/* Date (2nd line; set show_date=0 to disable) */
static const int   show_date  = 1;
static const char *date_font  = "Liberation Sans:style=Regular:size=26";
static const char *date_color = "#333333";
static const char *date_fmt   = "%A, %-d %B %Y";

/* Refresh interval (seconds) */
static const int   refresh_sec = 1;

/* Vertical layout */
static const int   block_y_off  = 0;   /* shift entire block (time+date) in px */
static const int   line_spacing = 12;  /* gap between time and date in px */
