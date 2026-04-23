#ifndef PINOTE_H
#define PINOTE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <math.h>

// ============================================================
// Constants
// ============================================================

#define MAX_NOTES 100
#define NOTES_FILE "pinote_notes.dat"
#define MIN_NOTE_SCALE 0.15f
#define MAX_STROKES_PER_NOTE 50
#define MAX_POINTS_PER_STROKE 256
#define PORT 5000
#define BUFFER_SIZE 131072  // 128KB
#define STATUS_ROW_HEIGHT 40
#define FONT_SCALE 2
#define MAX_ANIME 32
#define CONFIG_PATH "pinote_config.json"
#define CACHE_TTL 900  // 15 minutes

// Chart settings
#define MAX_SENSORS 5
#define MAX_CHART_POINTS 250   // ~10 days of hourly data
#define DEFAULT_CHART_HEIGHT 200

// Orientation constants (set via "orientation" in pinote_config.json)
#define ORIENTATION_LANDSCAPE 1         // 0° - normal horizontal
#define ORIENTATION_PORTRAIT 2          // 90° clockwise - vertical
#define ORIENTATION_LANDSCAPE_FLIP 3    // 180° - upside down horizontal
#define ORIENTATION_PORTRAIT_FLIP 4     // 270° clockwise - vertical flipped

// Module layout
#define MODULE_CHART  0
#define MODULE_ANIME  1
#define MODULE_NOTES  2
#define MODULE_RSS      3
#define MODULE_FORECAST 4
#define MODULE_WIDTH_FULL 0
#define MODULE_WIDTH_HALF 1
#define MODULE_HEIGHT_FILL -1  // returned by get_module_height for "fill remaining space"
#define MAX_MODULES 10

// UI colors (backgrounds, separators)
#define BG_MODULE_R 15
#define BG_MODULE_G 15
#define BG_MODULE_B 20
#define BG_CHART_R  18
#define BG_CHART_G  18
#define BG_CHART_B  25
#define SEP_MAJOR_R 50
#define SEP_MAJOR_G 50
#define SEP_MAJOR_B 60
#define SEP_MINOR_R 35
#define SEP_MINOR_G 35
#define SEP_MINOR_B 45

// RSS feed
#define MAX_RSS_ITEMS 20

// Weather forecast
#define MAX_FORECAST_DAYS 7

// Sprite animation
#define SPRITE_FPS 10
#define SPRITE_FRAME_MS (1000 / SPRITE_FPS)

// ============================================================
// Data structures
// ============================================================

typedef struct {
    float x;
    float y;
} Point;

typedef struct {
    Point points[MAX_POINTS_PER_STROKE];
    int num_points;
} Stroke;

typedef struct {
    Stroke strokes[MAX_STROKES_PER_NOTE];
    int num_strokes;
    float width;
    float height;
    int is_linebreak;
} Note;

typedef struct {
    Note notes[MAX_NOTES];
    int num_notes;
    pthread_mutex_t lock;
} NoteStore;

typedef struct {
    int fd;
    uint8_t *fbp;       // mmap'd framebuffer (front)
    uint8_t *backbuf;   // back buffer for double-buffering
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    long screensize;
} Framebuffer;

// Module config entry
typedef struct {
    int type;    // MODULE_CHART, MODULE_ANIME, MODULE_NOTES, MODULE_RSS
    int width;   // MODULE_WIDTH_FULL or MODULE_WIDTH_HALF
    int span;    // 1 = normal (default), 2+ = span multiple rows on one side
} ModuleConfig;

// Config
typedef struct {
    char hue_bridge_ip[64];
    char hue_api_key[128];
    char hue_sensor_ids[10][16];
    int num_hue_sensors;
    int anime_truncate;    // max characters for anime titles (0 = no truncation)
    int anime_per_line;    // how many anime per row (0 or 2 = two per line, 1 = one per line)
    float note_scale;      // scale factor for notes (default 0.6)
    float latitude;        // location for weather (Open-Meteo)
    float longitude;
    int has_location;      // 1 if lat/lon configured
    char chart_api_url[256];  // URL for chart data API
    char chart_api_key[128];  // API key (sent as X-API-Key header)
    char anidata_url[256];    // Base URL for ANIDATA service (anime metadata)
    char anidata_api_key[128]; // API key for ANIDATA writes (sent as X-API-Key header)
    int anime_include_p2w;    // 1 = include MAL plan-to-watch (default 1), 0 = watching only
    int chart_height;        // chart height in pixels (0 = disabled)
    int refresh_interval;    // API refresh interval in seconds (default CACHE_TTL)
    char rss_urls[10][256];  // RSS feed URLs (rss_url accepts string or array)
    int num_rss_urls;
    int max_rss_items;       // max items to display (default 6)
    int rss_truncate;        // max characters for RSS titles (0 = no truncation)
    int rss_per_line;        // RSS items per row (1 or 2, default 1)
    int rss_wrap;            // wrap long titles to second line (0 or 1, default 0)
    int sprite_enabled;      // 1 = show animated sprite, 0 = off (default 0)
    int orientation;         // 1=landscape, 2=portrait, 3=landscape_flip, 4=portrait_flip
    char webhook_url[256];   // Discord/Slack/generic webhook for notifications
    int forecast_days;       // forecast days to show (1-7, default 3, 0 = disabled)
    char background_image[256]; // path to 24-bit BMP, size must match logical display
    int modules_transparent;    // 1 = skip module bg fills (show background_image through)
    ModuleConfig modules[MAX_MODULES];
    int num_modules;
} AppConfig;

// Cached API data (refreshed every CACHE_TTL seconds)
typedef struct {
    char hue_temps[10][32];   // individual Hue sensor temperatures
    char hue_names[10][64];   // sensor names from Hue API (for chart color matching)
    int num_hue_temps;
    char weather_temp[32];
    char anime_titles[MAX_ANIME][128];     // anime title (truncated)
    char anime_countdowns[MAX_ANIME][64];  // countdown string ("3d 5h Ep12")
    long anime_airing_at[MAX_ANIME];      // unix timestamp (0 = TBA, -1 = aired)
    int num_anime_entries;
    time_t last_fetched;
    time_t anime_last_fetched;  // updated only on successful anime fetch
} StatusCache;

// Chart data structures
typedef struct {
    time_t timestamp;
    float value;
} ChartPoint;

typedef struct {
    char name[64];
    ChartPoint points[MAX_CHART_POINTS];
    int num_points;
    uint8_t r, g, b;
} SensorData;

typedef struct {
    SensorData sensors[MAX_SENSORS];
    int num_sensors;
    time_t last_fetched;
} ChartData;

// RSS feed data structures
typedef struct {
    char title[256];
    char date_str[16];   // "Mar 13" format
} RssItem;

// RSS fetch method (cached per-feed to avoid redundant requests)
#define RSS_METHOD_UNKNOWN 0  // not yet determined
#define RSS_METHOD_XML     1  // direct RSS XML worked
#define RSS_METHOD_JSON    2  // rss2json fallback needed

typedef struct {
    RssItem items[MAX_RSS_ITEMS];
    int num_items;
    int fetch_methods[10]; // per-URL fetch method cache
    char label[64];      // display label for current feed (domain, shown when cycling)
    char next_label[64]; // display label for next feed in rotation
    time_t last_fetched;
} RssCache;

// Weather forecast data
typedef struct {
    char day_name[4];    // "Mon", "Tue", etc.
    int weather_code;    // WMO code (0-99)
    float temp_max;
    float temp_min;
    float wind_max;      // km/h
    float uv_max;
    float precip_mm;
} ForecastDay;

typedef struct {
    ForecastDay days[MAX_FORECAST_DAYS];
    int num_days;
    time_t last_fetched;
} ForecastCache;

// ============================================================
// Globals (defined in notes_server.c, rss.c, forecast.c)
// ============================================================

extern NoteStore store;
extern Framebuffer fb;
extern AppConfig config;
extern StatusCache status_cache;
extern volatile int running;
extern volatile int first_render_done;
extern int server_socket;
extern ChartData chart_data;
extern RssCache rss_cache;
extern ForecastCache forecast_cache;

// ============================================================
// Shared helpers
// ============================================================

// Truncate a string to max_chars, replacing last two chars with ".."
static inline void truncate_with_dots(char *str, int max_chars) {
    if (max_chars >= 3 && (int)strlen(str) > max_chars) {
        str[max_chars - 2] = '.';
        str[max_chars - 1] = '.';
        str[max_chars] = '\0';
    }
}

// Strip UTF-8 accented Latin characters to ASCII equivalents (in-place)
// Handles 2-byte UTF-8 sequences (U+00C0-U+00FF): é→e, ü→u, ñ→n, etc.
// ASCII base for U+00C0..U+00FF (Latin-1 Supplement accented chars)
// À Á Â Ã Ä Å Æ Ç È É Ê Ë Ì Í Î Ï Ð Ñ Ò Ó Ô Õ Ö × Ø Ù Ú Û Ü Ý Þ ß
// à á â ã ä å æ ç è é ê ë ì í î ï ð ñ ò ó ô õ ö ÷ ø ù ú û ü ý þ ÿ
static const char latin_accent_map[64] =
    "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";

static inline void strip_utf8_accents(char *str, int size) {
    size_t src_len = strnlen(str, size > 0 ? (size_t)size - 1 : 0);
    unsigned char *s = (unsigned char *)str;
    unsigned char *src_end = s + src_len;
    char tmp[256];
    char *out = tmp;
    char *end = tmp + (size < (int)sizeof(tmp) ? size : (int)sizeof(tmp)) - 1;
    while (s < src_end && out < end) {
        if (s + 1 < src_end && s[0] == 0xC3 && s[1] >= 0x80 && s[1] <= 0xBF) {
            // 2-byte UTF-8: U+00C0..U+00FF
            // German umlauts and ß → preserve as Latin-1 bytes so draw_char
            // can render them via dedicated glyphs (Ä Ö Ü ß ä ö ü).
            unsigned char b1 = s[1];
            if (b1 == 0x84 || b1 == 0x96 || b1 == 0x9C || b1 == 0x9F ||
                b1 == 0xA4 || b1 == 0xB6 || b1 == 0xBC) {
                *out++ = 0xC0 | (b1 & 0x3F);
            } else {
                *out++ = latin_accent_map[b1 - 0x80];
            }
            s += 2;
        } else if (s + 1 < src_end && s[0] == 0xC2 && s[1] == 0xB7) {
            // U+00B7 middle dot · → " - "
            if (out + 3 <= end) { *out++ = ' '; *out++ = '-'; *out++ = ' '; }
            s += 2;
        } else if (s + 2 < src_end && s[0] == 0xE2 && s[1] == 0x80 &&
                   (s[2] == 0x93 || s[2] == 0x94)) {
            // U+2013 en dash / U+2014 em dash → " - "
            if (out + 3 <= end) { *out++ = ' '; *out++ = '-'; *out++ = ' '; }
            s += 3;
        } else if (s + 2 < src_end && s[0] == 0xE2 && s[1] == 0x80 &&
                   (s[2] == 0x98 || s[2] == 0x99 || s[2] == 0x9A || s[2] == 0x9B)) {
            // U+2018..U+201B smart single quotes → '
            *out++ = '\'';
            s += 3;
        } else if (s + 2 < src_end && s[0] == 0xE2 && s[1] == 0x80 &&
                   (s[2] == 0x9C || s[2] == 0x9D || s[2] == 0x9E || s[2] == 0x9F)) {
            // U+201C..U+201F smart double quotes → "
            *out++ = '"';
            s += 3;
        } else if (s + 2 < src_end && s[0] == 0xE2 && s[1] == 0x80 && s[2] == 0xA6) {
            // U+2026 horizontal ellipsis → ...
            if (out + 3 <= end) { *out++ = '.'; *out++ = '.'; *out++ = '.'; }
            s += 3;
        } else if (s + 1 < src_end && (s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            // Other 2-byte UTF-8: replace with '?'
            *out++ = '?';
            s += 2;
        } else if (s + 2 < src_end && (s[0] & 0xF0) == 0xE0 &&
                   (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            // 3-byte UTF-8: replace with '?'
            *out++ = '?';
            s += 3;
        } else if (s + 3 < src_end && (s[0] & 0xF8) == 0xF0 &&
                   (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
                   (s[3] & 0xC0) == 0x80) {
            // 4-byte UTF-8: replace with '?'
            *out++ = '?';
            s += 4;
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
    strcpy(str, tmp);
}

// ============================================================
// fb_draw.c
// ============================================================

int  fb_init(Framebuffer *fb, const char *device);
void get_display_size(Framebuffer *fb, int *width, int *height);
int  fb_load_background(Framebuffer *fb, const char *path);
void set_pixel(Framebuffer *fb, int logical_x, int logical_y, uint8_t r, uint8_t g, uint8_t b);
void set_pixel_front(Framebuffer *fb, int logical_x, int logical_y, uint8_t r, uint8_t g, uint8_t b);
void fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void clear_screen(Framebuffer *fb);
void fb_flip(Framebuffer *fb);
void fb_restore_pixel(Framebuffer *fb, int logical_x, int logical_y);
void fb_restore_rect(Framebuffer *fb, int rx, int ry, int rw, int rh);
void draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, int thickness);
void draw_char(Framebuffer *fb, int x, int y, char c, uint8_t r, uint8_t g, uint8_t b, int scale);
int  draw_text(Framebuffer *fb, int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b, int scale);

// ============================================================
// config.c
// ============================================================

void load_config(const char *path, AppConfig *cfg);

// ============================================================
// api_fetch.c
// ============================================================

int  run_cmd(const char *cmd, char *buf, int bufsize);
void fetch_chart_data(ChartData *cd);
void fetch_rss_feed(RssCache *out);
void refresh_status_cache(void);

// ============================================================
// chart.c
// ============================================================

void draw_chart(Framebuffer *fb, int chart_x, int chart_y, int chart_w, int chart_h);

// ============================================================
// rss.c
// ============================================================

void draw_rss(Framebuffer *fb, int x, int y, int w, int h);
int  get_rss_height(int w);

// ============================================================
// forecast.c
// ============================================================

void draw_forecast(Framebuffer *fb, int x, int y, int w, int h);
int  get_forecast_height(void);

// ============================================================
// sprite.c
// ============================================================

void *sprite_thread(void *arg);
void sprite_redraw_after_flip(void);

// ============================================================
// notes_server.c
// ============================================================

void draw_status_bar(Framebuffer *fb, int width);
void draw_anime(Framebuffer *fb, int x, int y, int w, int h);
int  get_anime_height(void);
void draw_notes_area(Framebuffer *fb, int x, int y, int w, int h);
void render_screen(void);
void save_notes(void);
void load_notes(void);

#endif // PINOTE_H
