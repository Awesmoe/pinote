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
#define MAX_ANIME 10
#define CONFIG_PATH "pinote_config.json"
#define CACHE_TTL 300  // 5 minutes

// Temperature chart settings
#define MAX_SENSORS 5
#define MAX_TEMP_POINTS 250   // ~10 days of hourly data
#define DEFAULT_CHART_HEIGHT 200

// Orientation settings
// Set to 0 for auto-detect, or force: 1=landscape, 2=portrait, 3=landscape_flipped, 4=portrait_flipped
#define FORCE_ORIENTATION 4

// Orientation constants
#define ORIENTATION_LANDSCAPE 1         // 0° - normal horizontal
#define ORIENTATION_PORTRAIT 2          // 90° clockwise - vertical
#define ORIENTATION_LANDSCAPE_FLIP 3    // 180° - upside down horizontal
#define ORIENTATION_PORTRAIT_FLIP 4     // 270° clockwise - vertical flipped

// Module layout
#define MODULE_CHART  0
#define MODULE_ANIME  1
#define MODULE_NOTES  2
#define MODULE_RSS    3
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
#define MAX_RSS_ITEMS 10

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
} ModuleConfig;

// Config
typedef struct {
    char hue_bridge_ip[64];
    char hue_api_key[128];
    char hue_sensor_ids[10][16];
    int num_hue_sensors;
    int anilist_media_ids[MAX_ANIME];
    int num_anime;
    int anime_truncate;    // max characters for anime titles (0 = no truncation)
    int anime_per_line;    // how many anime per row (0 or 2 = two per line, 1 = one per line)
    float note_scale;      // scale factor for notes (default 0.6)
    float latitude;        // location for weather (Open-Meteo)
    float longitude;
    int has_location;      // 1 if lat/lon configured
    char temp_api_url[256];  // URL for temperature history API
    char temp_api_key[128];  // API key (sent as X-API-Key header)
    int chart_height;        // chart height in pixels (0 = disabled)
    int refresh_interval;    // API refresh interval in seconds (default CACHE_TTL)
    char rss_url[256];       // RSS feed URL
    int max_rss_items;       // max items to display (default 6)
    int rss_truncate;        // max characters for RSS titles (0 = no truncation)
    int rss_per_line;        // RSS items per row (1 or 2, default 1)
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
} StatusCache;

// Temperature chart data structures
typedef struct {
    time_t timestamp;
    float temperature;
} TempPoint;

typedef struct {
    char name[64];
    TempPoint points[MAX_TEMP_POINTS];
    int num_points;
    uint8_t r, g, b;
} SensorData;

typedef struct {
    SensorData sensors[MAX_SENSORS];
    int num_sensors;
    time_t last_fetched;
} TempHistory;

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
    int fetch_method;   // RSS_METHOD_* — remembered after first successful fetch
    time_t last_fetched;
} RssCache;

// ============================================================
// Globals (defined in notes_server.c, rss.c)
// ============================================================

extern NoteStore store;
extern Framebuffer fb;
extern AppConfig config;
extern StatusCache status_cache;
extern volatile int running;
extern int server_socket;
extern TempHistory temp_history;
extern RssCache rss_cache;

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

// ============================================================
// fb_draw.c
// ============================================================

int  fb_init(Framebuffer *fb, const char *device);
void get_display_size(Framebuffer *fb, int *width, int *height);
void set_pixel(Framebuffer *fb, int logical_x, int logical_y, uint8_t r, uint8_t g, uint8_t b);
void fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void clear_screen(Framebuffer *fb);
void fb_flip(Framebuffer *fb);
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
void fetch_temperature_history(void);
void fetch_rss_feed(void);
void refresh_status_cache(void);

// ============================================================
// chart.c
// ============================================================

void draw_chart(Framebuffer *fb, int chart_x, int chart_y, int chart_w, int chart_h);

// ============================================================
// rss.c
// ============================================================

void draw_rss(Framebuffer *fb, int x, int y, int w, int h);
int  get_rss_height(void);

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
