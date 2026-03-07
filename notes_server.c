/*
 * Pi Notepad Server - Production Version
 * Minimal logging, optimized for deployment
*/

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

#define MAX_NOTES 500
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

// Orientation settings
// Set to 0 for auto-detect, or force: 1=landscape, 2=portrait, 3=landscape_flipped, 4=portrait_flipped
#define FORCE_ORIENTATION 4

// Orientation constants
#define ORIENTATION_LANDSCAPE 1         // 0° - normal horizontal
#define ORIENTATION_PORTRAIT 2          // 90° clockwise - vertical
#define ORIENTATION_LANDSCAPE_FLIP 3    // 180° - upside down horizontal
#define ORIENTATION_PORTRAIT_FLIP 4     // 270° clockwise - vertical flipped

int current_orientation = 0;

// Data structures
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

// Config
typedef struct {
    char hue_bridge_ip[64];
    char hue_api_key[128];
    char hue_sensor_ids[10][16];
    int num_hue_sensors;
    int anilist_media_ids[MAX_ANIME];
    int num_anime;
    int truncate_titles;   // 1 = truncate long titles, 0 = full names
    int anime_per_line;    // how many anime per row (0 = all on one line)
    float note_scale;      // scale factor for notes (default 0.45)
    float latitude;        // location for weather (Open-Meteo)
    float longitude;
    int has_location;      // 1 if lat/lon configured
} AppConfig;

// Cached API data (refreshed every CACHE_TTL seconds)
typedef struct {
    char hue_temp[32];
    char weather_temp[32];
    char anime_lines[MAX_ANIME][512];  // one string per row
    int num_lines;
    time_t last_fetched;
} StatusCache;

// Forward declarations
void render_notes();

NoteStore store = {0};
Framebuffer fb = {0};
AppConfig config = {0};
StatusCache status_cache = {0};
volatile int running = 1;
int server_socket = -1;
time_t server_start_time;

// Signal handler
void handle_signal(int sig) {
    running = 0;
    if (server_socket != -1) shutdown(server_socket, SHUT_RDWR);
}

// Initialize framebuffer
int fb_init(Framebuffer *fb, const char *device) {
    fb->fd = open(device, O_RDWR);
    if (fb->fd == -1) return -1;

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->vinfo) == -1) {
        close(fb->fd);
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->finfo) == -1) {
        close(fb->fd);
        return -1;
    }

    fb->screensize = fb->vinfo.yres * fb->finfo.line_length;
    fb->fbp = (uint8_t *)mmap(0, fb->screensize, PROT_READ | PROT_WRITE, 
                              MAP_SHARED, fb->fd, 0);
    
    if (fb->fbp == MAP_FAILED) {
        close(fb->fd);
        return -1;
    }

    // Allocate back buffer for double-buffering
    fb->backbuf = (uint8_t *)malloc(fb->screensize);
    if (!fb->backbuf) {
        munmap(fb->fbp, fb->screensize);
        close(fb->fd);
        return -1;
    }

    // Determine orientation
    if (FORCE_ORIENTATION > 0) {
        current_orientation = FORCE_ORIENTATION;
    } else {
        // Auto-detect: portrait if height > width
        if (fb->vinfo.yres > fb->vinfo.xres) {
            current_orientation = ORIENTATION_PORTRAIT;
        } else {
            current_orientation = ORIENTATION_LANDSCAPE;
        }
    }

    return 0;
}

// Get logical display dimensions based on orientation
void get_display_size(Framebuffer *fb, int *width, int *height) {
    if (current_orientation == ORIENTATION_PORTRAIT || 
        current_orientation == ORIENTATION_PORTRAIT_FLIP) {
        // Portrait: swap dimensions so layout logic works
        *width = fb->vinfo.yres;
        *height = fb->vinfo.xres;
    } else {
        *width = fb->vinfo.xres;
        *height = fb->vinfo.yres;
    }
}

// Transform coordinates based on orientation
void transform_coords(Framebuffer *fb, int logical_x, int logical_y, int *physical_x, int *physical_y) {
    switch (current_orientation) {
        case ORIENTATION_LANDSCAPE:
            // No transformation
            *physical_x = logical_x;
            *physical_y = logical_y;
            break;
            
        case ORIENTATION_PORTRAIT:
            // 90Â° clockwise: (x,y) -> (height-y, x)
            *physical_x = fb->vinfo.xres - 1 - logical_y;
            *physical_y = logical_x;
            break;
            
        case ORIENTATION_LANDSCAPE_FLIP:
            // 180Â°: (x,y) -> (width-x, height-y)
            *physical_x = fb->vinfo.xres - 1 - logical_x;
            *physical_y = fb->vinfo.yres - 1 - logical_y;
            break;
            
        case ORIENTATION_PORTRAIT_FLIP:
            // 270Â° clockwise: (x,y) -> (y, width-x)
            *physical_x = logical_y;
            *physical_y = fb->vinfo.yres - 1 - logical_x;
            break;
            
        default:
            *physical_x = logical_x;
            *physical_y = logical_y;
            break;
    }
}

// Set pixel (writes to back buffer, with orientation support)
void set_pixel(Framebuffer *fb, int logical_x, int logical_y, uint8_t r, uint8_t g, uint8_t b) {
    int x, y;
    transform_coords(fb, logical_x, logical_y, &x, &y);

    if (x < 0 || x >= fb->vinfo.xres || y < 0 || y >= fb->vinfo.yres)
        return;

    long location = y * fb->finfo.line_length + x * (fb->vinfo.bits_per_pixel / 8);

    if (fb->vinfo.bits_per_pixel == 32) {
        *(fb->backbuf + location + 0) = b;
        *(fb->backbuf + location + 1) = g;
        *(fb->backbuf + location + 2) = r;
        *(fb->backbuf + location + 3) = 0;
    } else if (fb->vinfo.bits_per_pixel == 24) {
        *(fb->backbuf + location + 0) = b;
        *(fb->backbuf + location + 1) = g;
        *(fb->backbuf + location + 2) = r;
    } else if (fb->vinfo.bits_per_pixel == 16) {
        uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        *((uint16_t *)(fb->backbuf + location)) = rgb565;
    }
}

// Fill rectangle
void fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    if (fb->vinfo.bits_per_pixel == 16) {
        uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        
        for (int row = y; row < y + h && row < fb->vinfo.yres; row++) {
            long line_start = row * fb->finfo.line_length + x * 2;
            uint16_t *pixel = (uint16_t *)(fb->backbuf + line_start);
            
            for (int col = 0; col < w && (x + col) < fb->vinfo.xres; col++) {
                *pixel++ = rgb565;
            }
        }
    } else {
        for (int row = y; row < y + h; row++) {
            for (int col = x; col < x + w; col++) {
                set_pixel(fb, col, row, r, g, b);
            }
        }
    }
}

// Clear screen (back buffer)
void clear_screen(Framebuffer *fb) {
    fill_rect(fb, 0, 0, fb->vinfo.xres, fb->vinfo.yres, 20, 20, 25);
}

// Copy back buffer to framebuffer in one shot (no flicker)
void fb_flip(Framebuffer *fb) {
    memcpy(fb->fbp, fb->backbuf, fb->screensize);
}

// Draw line
void draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, int thickness) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        for (int ty = -thickness/2; ty <= thickness/2; ty++) {
            for (int tx = -thickness/2; tx <= thickness/2; tx++) {
                if (tx*tx + ty*ty <= (thickness/2)*(thickness/2)) {
                    set_pixel(fb, x0 + tx, y0 + ty, r, g, b);
                }
            }
        }

        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// ============================================================
// Bitmap font (8x16, printable ASCII 32-126)
// Classic VGA/CP437 style, public domain
// Each character: 16 bytes, one per row, MSB = leftmost pixel
// ============================================================
static const uint8_t font8x16[95][16] = {
    // 32: Space
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 33: !
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 34: "
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 35: #
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00},
    // 36: $
    {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    // 37: %
    {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    // 38: &
    {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 39: '
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 40: (
    {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    // 41: )
    {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    // 42: *
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    // 43: +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    // 44: ,
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    // 45: -
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 46: .
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 47: /
    {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    // 48: 0
    {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 49: 1
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    // 50: 2
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 51: 3
    {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 52: 4
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    // 53: 5
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 54: 6
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 55: 7
    {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    // 56: 8
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 57: 9
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    // 58: :
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    // 59: ;
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    // 60: <
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    // 61: =
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 62: >
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    // 63: ?
    {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 64: @
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    // 65: A
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 66: B
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    // 67: C
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    // 68: D
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    // 69: E
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    // 70: F
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 71: G
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    // 72: H
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 73: I
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 74: J
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    // 75: K
    {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 76: L
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    // 77: M
    {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 78: N
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 79: O
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 80: P
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 81: Q
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    // 82: R
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 83: S
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 84: T
    {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 85: U
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 86: V
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    // 87: W
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    // 88: X
    {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 89: Y
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 90: Z
    {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 91: [
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    // 92: backslash
    {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    // 93: ]
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    // 94: ^
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 95: _
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00},
    // 96: `
    {0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 97: a
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 98: b
    {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    // 99: c
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 100: d
    {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 101: e
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 102: f
    {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    // 103: g
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    // 104: h
    {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 105: i
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 106: j
    {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    // 107: k
    {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 108: l
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 109: m
    {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00},
    // 110: n
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    // 111: o
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 112: p
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    // 113: q
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    // 114: r
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 115: s
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 116: t
    {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    // 117: u
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 118: v
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    // 119: w
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00},
    // 120: x
    {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    // 121: y
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    // 122: z
    {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 123: {
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    // 124: |
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    // 125: }
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    // 126: ~
    {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

// Draw a single character at (x,y) with given color and scale
void draw_char(Framebuffer *fb, int x, int y, char c, uint8_t r, uint8_t g, uint8_t b, int scale) {
    if (c < 32 || c > 126) return;
    const uint8_t *glyph = font8x16[c - 32];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        set_pixel(fb, x + col * scale + sx, y + row * scale + sy, r, g, b);
                    }
                }
            }
        }
    }
}

// Draw a string at (x,y), returns the x position after the last character
int draw_text(Framebuffer *fb, int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b, int scale) {
    while (*text) {
        draw_char(fb, x, y, *text, r, g, b, scale);
        x += 8 * scale;
        text++;
    }
    return x;
}

// ============================================================
// System info helpers
// ============================================================

// Get the local IP address of the active network interface
void get_local_ip(char *buf, int bufsize) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        snprintf(buf, bufsize, "No IP");
        return;
    }

    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &serv.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        close(sock);
        snprintf(buf, bufsize, "No IP");
        return;
    }

    struct sockaddr_in name = {0};
    socklen_t namelen = sizeof(name);
    getsockname(sock, (struct sockaddr *)&name, &namelen);
    close(sock);

    inet_ntop(AF_INET, &name.sin_addr, buf, bufsize);
}

// Get Wi-Fi signal level only (no SSID)
int get_wifi_signal() {
    int signal_dbm = -100;
    FILE *wf = fopen("/proc/net/wireless", "r");
    if (wf) {
        char line[256];
        if (fgets(line, sizeof(line), wf) && fgets(line, sizeof(line), wf)) {
            if (fgets(line, sizeof(line), wf)) {
                char iface[32];
                float status, link, level;
                if (sscanf(line, "%31s %f %f %f", iface, &status, &link, &level) >= 4) {
                    signal_dbm = (int)level;
                }
            }
        }
        fclose(wf);
    }
    return signal_dbm;
}

// Format signal strength as bars: [||||] or [||  ] etc.
void format_signal_bars(int dbm, char *buf, int bufsize) {
    // dBm ranges: >-50 excellent(4), >-60 good(3), >-70 fair(2), >-80 weak(1), else none(0)
    int bars;
    if (dbm > -50) bars = 4;
    else if (dbm > -60) bars = 3;
    else if (dbm > -70) bars = 2;
    else if (dbm > -80) bars = 1;
    else bars = 0;

    char inner[5] = "    ";
    for (int i = 0; i < bars; i++) inner[i] = '|';
    snprintf(buf, bufsize, "[%s]", inner);
}

// Format uptime string
void get_uptime_str(char *buf, int bufsize, time_t start_time) {
    time_t now = time(NULL);
    int elapsed = (int)(now - start_time);

    int days = elapsed / 86400;
    int hours = (elapsed % 86400) / 3600;
    int mins = (elapsed % 3600) / 60;

    if (days > 0)
        snprintf(buf, bufsize, "Up %dd %dh %dm", days, hours, mins);
    else if (hours > 0)
        snprintf(buf, bufsize, "Up %dh %dm", hours, mins);
    else
        snprintf(buf, bufsize, "Up %dm", mins);
}

// ============================================================
// Config file loader
// ============================================================

// Helper: extract a JSON string value by key
static int json_get_string(const char *json, const char *key, char *out, int out_size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return 0;
    pos++;
    int i = 0;
    while (*pos && *pos != '"' && i < out_size - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return 1;
}

void load_config(const char *path, AppConfig *cfg) {
    memset(cfg, 0, sizeof(AppConfig));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[4096];
    int len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    json_get_string(buf, "hue_bridge_ip", cfg->hue_bridge_ip, sizeof(cfg->hue_bridge_ip));
    json_get_string(buf, "hue_api_key", cfg->hue_api_key, sizeof(cfg->hue_api_key));
    // Parse hue_sensor_ids array (also supports old "hue_sensor_id" string)
    const char *sid_arr = strstr(buf, "\"hue_sensor_ids\"");
    if (sid_arr) {
        const char *bracket = strchr(sid_arr, '[');
        if (bracket) {
            const char *p = bracket + 1;
            while (*p && *p != ']' && cfg->num_hue_sensors < 10) {
                while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                if (*p == '"') {
                    p++;
                    int i = 0;
                    while (*p && *p != '"' && i < 15) cfg->hue_sensor_ids[cfg->num_hue_sensors][i++] = *p++;
                    cfg->hue_sensor_ids[cfg->num_hue_sensors][i] = '\0';
                    cfg->num_hue_sensors++;
                    if (*p == '"') p++;
                } else if (*p >= '0' && *p <= '9') {
                    int i = 0;
                    while (*p >= '0' && *p <= '9' && i < 15) cfg->hue_sensor_ids[cfg->num_hue_sensors][i++] = *p++;
                    cfg->hue_sensor_ids[cfg->num_hue_sensors][i] = '\0';
                    cfg->num_hue_sensors++;
                } else {
                    break;
                }
            }
        }
    } else {
        // Fallback: single "hue_sensor_id" string
        char single_id[16] = {0};
        if (json_get_string(buf, "hue_sensor_id", single_id, sizeof(single_id)) && single_id[0]) {
            strncpy(cfg->hue_sensor_ids[0], single_id, 15);
            cfg->num_hue_sensors = 1;
        }
    }

    // Parse truncate_titles (default: false)
    cfg->truncate_titles = (strstr(buf, "\"truncate_titles\":true") != NULL) ? 1 : 0;

    // Parse anime_per_line (default: 0 = all on one line)
    const char *apl = strstr(buf, "\"anime_per_line\":");
    if (apl) {
        sscanf(apl + 17, "%d", &cfg->anime_per_line);
    }

    // Parse note_scale (default: 0.6)
    cfg->note_scale = 0.6f;
    const char *ns = strstr(buf, "\"note_scale\":");
    if (ns) {
        sscanf(ns + 13, "%f", &cfg->note_scale);
        if (cfg->note_scale < 0.1f) cfg->note_scale = 0.1f;
        if (cfg->note_scale > 1.0f) cfg->note_scale = 1.0f;
    }

    // Parse location (latitude/longitude for Open-Meteo weather)
    const char *lat = strstr(buf, "\"latitude\":");
    const char *lon = strstr(buf, "\"longitude\":");
    if (lat && lon) {
        sscanf(lat + 11, "%f", &cfg->latitude);
        sscanf(lon + 12, "%f", &cfg->longitude);
        cfg->has_location = 1;
    }

    // Parse anilist_media_ids array
    const char *ids = strstr(buf, "\"anilist_media_ids\"");
    if (ids) {
        const char *bracket = strchr(ids, '[');
        if (bracket) {
            const char *p = bracket + 1;
            while (*p && *p != ']' && cfg->num_anime < MAX_ANIME) {
                while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                if (*p >= '0' && *p <= '9') {
                    cfg->anilist_media_ids[cfg->num_anime++] = atoi(p);
                    while (*p >= '0' && *p <= '9') p++;
                } else {
                    break;
                }
            }
        }
    }
}

// ============================================================
// Note persistence
// ============================================================

void save_notes() {
    FILE *f = fopen(NOTES_FILE, "wb");
    if (!f) return;
    fwrite(&store.num_notes, sizeof(int), 1, f);
    fwrite(store.notes, sizeof(Note), store.num_notes, f);
    fclose(f);
}

void load_notes() {
    FILE *f = fopen(NOTES_FILE, "rb");
    if (!f) return;
    int num = 0;
    if (fread(&num, sizeof(int), 1, f) != 1 || num < 0 || num > MAX_NOTES) {
        fclose(f);
        return;
    }
    if ((int)fread(store.notes, sizeof(Note), num, f) == num) {
        store.num_notes = num;
    }
    fclose(f);
}

// ============================================================
// API fetchers
// ============================================================

// Fetch temperature from Philips Hue Bridge
void get_hue_temperature(const AppConfig *cfg, char *buf, int bufsize) {
    buf[0] = '\0';
    if (!cfg->hue_bridge_ip[0] || !cfg->hue_api_key[0] || cfg->num_hue_sensors == 0)
        return;

    for (int s = 0; s < cfg->num_hue_sensors; s++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "curl -s --connect-timeout 2 'http://%s/api/%s/sensors/%s' 2>/dev/null",
            cfg->hue_bridge_ip, cfg->hue_api_key, cfg->hue_sensor_ids[s]);

        FILE *fp = popen(cmd, "r");
        if (!fp) continue;

        char response[4096];
        int len = 0;
        while (len < (int)sizeof(response) - 1) {
            int n = fread(response + len, 1, sizeof(response) - 1 - len, fp);
            if (n <= 0) break;
            len += n;
        }
        pclose(fp);
        response[len] = '\0';

        // Add separator
        if (buf[0])
            strncat(buf, " | ", bufsize - strlen(buf) - 1);

        const char *temp = strstr(response, "\"temperature\":");
        if (temp) {
            int raw = 0;
            sscanf(temp + 14, "%d", &raw);
            char val[32];
            snprintf(val, sizeof(val), "%d.%dC", raw / 100, (abs(raw) % 100) / 10);
            strncat(buf, val, bufsize - strlen(buf) - 1);
        } else {
            strncat(buf, "err", bufsize - strlen(buf) - 1);
        }
    }
}

// Fetch outside temperature from Open-Meteo
void get_weather_temperature(const AppConfig *cfg, char *buf, int bufsize) {
    buf[0] = '\0';
    if (!cfg->has_location) return;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "curl -s --connect-timeout 3 'https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m' 2>/dev/null",
        cfg->latitude, cfg->longitude);

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    char response[2048];
    int len = 0;
    while (len < (int)sizeof(response) - 1) {
        int n = fread(response + len, 1, sizeof(response) - 1 - len, fp);
        if (n <= 0) break;
        len += n;
    }
    pclose(fp);
    response[len] = '\0';

    const char *temp = strstr(response, "\"temperature_2m\":");
    if (temp) {
        // Skip past the key in "current" block (not "current_units")
        const char *current = strstr(response, "\"current\":{");
        if (current) {
            temp = strstr(current, "\"temperature_2m\":");
            if (temp) {
                float val = 0;
                sscanf(temp + 17, "%f", &val);
                snprintf(buf, bufsize, "Out: %.1fC", val);
            }
        }
    }
}

// Parsed anime info (used for sorting)
typedef struct {
    char title[128];
    char countdown[64];
    long airing_at;  // unix timestamp, 0 = TBA
} AnimeInfo;

// Parse one anime entry from a JSON block starting at *pos
// Advances *pos past the parsed block
static void parse_one_anime(const char **pos, AnimeInfo *info) {
    memset(info, 0, sizeof(AnimeInfo));
    const char *p = *pos;

    // Parse title (romaji) — handle JSON escaped quotes (\")
    const char *romaji = strstr(p, "\"romaji\":\"");
    if (romaji) {
        romaji += 10;
        int i = 0;
        while (*romaji && i < (int)sizeof(info->title) - 1) {
            if (*romaji == '\\' && *(romaji + 1) == '"') {
                // Escaped quote — skip the backslash, keep the quote
                romaji++;
                info->title[i++] = *romaji++;
                continue;
            }
            if (*romaji == '"') break;  // Unescaped quote = end of string
            info->title[i++] = *romaji++;
        }
        info->title[i] = '\0';
    }

    // Parse airingAt (search within this entry)
    const char *airing = strstr(p, "\"airingAt\":");
    if (airing) {
        sscanf(airing + 11, "%ld", &info->airing_at);

        time_t now = time(NULL);
        long diff = info->airing_at - now;

        if (diff <= 0) {
            snprintf(info->countdown, sizeof(info->countdown), "Aired!");
        } else {
            int days = diff / 86400;
            int hours = (diff % 86400) / 3600;
            int mins = (diff % 3600) / 60;
            if (days > 0)
                snprintf(info->countdown, sizeof(info->countdown), "%dd %dh", days, hours);
            else if (hours > 0)
                snprintf(info->countdown, sizeof(info->countdown), "%dh %dm", hours, mins);
            else
                snprintf(info->countdown, sizeof(info->countdown), "%dm", mins);
        }

        const char *ep = strstr(p, "\"episode\":");
        if (ep) {
            int ep_num = 0;
            sscanf(ep + 10, "%d", &ep_num);
            char ep_str[32];
            snprintf(ep_str, sizeof(ep_str), " Ep%d", ep_num);
            strncat(info->countdown, ep_str, sizeof(info->countdown) - strlen(info->countdown) - 1);
        }
    } else {
        snprintf(info->countdown, sizeof(info->countdown), "TBA");
    }
}

// Fetch all anime in a single AniList GraphQL request using aliases
void fetch_all_anime(const int *media_ids, int count, AnimeInfo *out) {
    if (count <= 0) return;

    // Build query: {a0:Media(id:X){...} a1:Media(id:Y){...} ...}
    char query[4096];
    int pos = 0;
    pos += snprintf(query + pos, sizeof(query) - pos, "{");
    for (int i = 0; i < count && pos < (int)sizeof(query) - 200; i++) {
        pos += snprintf(query + pos, sizeof(query) - pos,
            "a%d:Media(id:%d){title{romaji}nextAiringEpisode{airingAt episode}}",
            i, media_ids[i]);
    }
    pos += snprintf(query + pos, sizeof(query) - pos, "}");

    // Build curl command
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "curl -s --connect-timeout 10 -X POST 'https://graphql.anilist.co' "
        "-H 'Content-Type: application/json' "
        "-d '{\"query\":\"%s\"}' 2>/dev/null", query);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        for (int i = 0; i < count; i++) {
            memset(&out[i], 0, sizeof(AnimeInfo));
            snprintf(out[i].countdown, sizeof(out[i].countdown), "TBA");
        }
        return;
    }

    char response[32768];
    int total = 0;
    while (total < (int)sizeof(response) - 1) {
        int n = fread(response + total, 1, sizeof(response) - 1 - total, fp);
        if (n <= 0) break;
        total += n;
    }
    pclose(fp);
    response[total] = '\0';

    // Parse each aliased result: "a0":{...}, "a1":{...}, ...
    for (int i = 0; i < count; i++) {
        char alias[16];
        snprintf(alias, sizeof(alias), "\"a%d\":", i);
        const char *entry = strstr(response, alias);
        if (entry) {
            parse_one_anime(&entry, &out[i]);
        } else {
            memset(&out[i], 0, sizeof(AnimeInfo));
            snprintf(out[i].countdown, sizeof(out[i].countdown), "TBA");
        }
    }
}

// qsort comparator: sort by airing_at ascending, TBA (0) goes last
int anime_sort_cmp(const void *a, const void *b) {
    const AnimeInfo *aa = (const AnimeInfo *)a;
    const AnimeInfo *bb = (const AnimeInfo *)b;
    // TBA (0) sorts to the end
    if (aa->airing_at == 0 && bb->airing_at == 0) return 0;
    if (aa->airing_at == 0) return 1;
    if (bb->airing_at == 0) return -1;
    if (aa->airing_at < bb->airing_at) return -1;
    if (aa->airing_at > bb->airing_at) return 1;
    return 0;
}

// Refresh the status cache (called periodically, does the slow API calls)
void refresh_status_cache() {
    // Hue temperature
    get_hue_temperature(&config, status_cache.hue_temp, sizeof(status_cache.hue_temp));

    // Outside temperature (Open-Meteo)
    get_weather_temperature(&config, status_cache.weather_temp, sizeof(status_cache.weather_temp));

    // Fetch all anime in one API call
    AnimeInfo anime[MAX_ANIME];
    int anime_count = config.num_anime;
    fetch_all_anime(config.anilist_media_ids, anime_count, anime);

    // Sort by airing time (soonest first, TBA last)
    qsort(anime, anime_count, sizeof(AnimeInfo), anime_sort_cmp);

    // Build display lines based on anime_per_line
    status_cache.num_lines = 0;
    int per_line = config.anime_per_line > 0 ? config.anime_per_line : anime_count;
    int items_on_current_line = 0;

    for (int i = 0; i < anime_count; i++) {
        if (items_on_current_line == 0) {
            if (status_cache.num_lines >= MAX_ANIME) break;
            status_cache.anime_lines[status_cache.num_lines][0] = '\0';
        }

        char *title = anime[i].title;

        // Truncate if enabled
        if (config.truncate_titles && strlen(title) > 20) {
            title[18] = '.';
            title[19] = '.';
            title[20] = '\0';
        }

        // Add separator if not first on this line
        char *line = status_cache.anime_lines[status_cache.num_lines];
        if (items_on_current_line > 0)
            strncat(line, "  |  ", sizeof(status_cache.anime_lines[0]) - strlen(line) - 1);

        char entry[256];
        if (title[0])
            snprintf(entry, sizeof(entry), "%s: %s", title, anime[i].countdown);
        else
            snprintf(entry, sizeof(entry), "?: %s", anime[i].countdown);

        strncat(line, entry, sizeof(status_cache.anime_lines[0]) - strlen(line) - 1);
        items_on_current_line++;

        if (items_on_current_line >= per_line) {
            status_cache.num_lines++;
            items_on_current_line = 0;
        }
    }

    if (items_on_current_line > 0)
        status_cache.num_lines++;

    status_cache.last_fetched = time(NULL);
}

// ============================================================
// Status bar
// ============================================================

// Get current status bar height (1 row for system info + 1 row per anime line)
int get_status_bar_height() {
    int anime_rows = status_cache.num_lines > 0 ? status_cache.num_lines : 0;
    return STATUS_ROW_HEIGHT * (1 + anime_rows);
}
void draw_status_bar(Framebuffer *fb) {
    int display_width, display_height;
    get_display_size(fb, &display_width, &display_height);
    int bar_height = get_status_bar_height();

    // Refresh API cache if stale
    time_t now = time(NULL);
    if (now - status_cache.last_fetched >= CACHE_TTL) {
        refresh_status_cache();
    }

    // Draw background
    for (int y = 0; y < bar_height; y++) {
        for (int x = 0; x < display_width; x++) {
            set_pixel(fb, x, y, 15, 15, 20);
        }
    }

    // Separator line at bottom of bar
    for (int x = 0; x < display_width; x++) {
        set_pixel(fb, x, bar_height - 1, 50, 50, 60);
    }

    // Row 1: system info
    int row1_y = (STATUS_ROW_HEIGHT - 16 * FONT_SCALE) / 2;
    int section_x = 10;

    // IP address (blue)
    char ip[64];
    get_local_ip(ip, sizeof(ip));
    char ip_str[80];
    snprintf(ip_str, sizeof(ip_str), "IP: %s:%d", ip, PORT);
    section_x = draw_text(fb, section_x, row1_y, ip_str, 100, 200, 255, FONT_SCALE);
    section_x += 30;

    // Wi-Fi signal bars only (green)
    int signal_dbm = get_wifi_signal();
    char bars[16];
    format_signal_bars(signal_dbm, bars, sizeof(bars));
    section_x = draw_text(fb, section_x, row1_y, bars, 100, 255, 100, FONT_SCALE);
    section_x += 30;

    // Outside temperature (cyan)
    if (status_cache.weather_temp[0]) {
        section_x = draw_text(fb, section_x, row1_y, status_cache.weather_temp, 100, 220, 255, FONT_SCALE);
        section_x += 30;
    }

    // Hue temperature (yellow)
    if (status_cache.hue_temp[0]) {
        draw_text(fb, section_x, row1_y, status_cache.hue_temp, 255, 220, 80, FONT_SCALE);
    }

    // Date + time (white, right-aligned)
    struct tm *tm_info = localtime(&now);
    char datetime_str[32];
    strftime(datetime_str, sizeof(datetime_str), "%d %b %H:%M", tm_info);
    int dt_width = strlen(datetime_str) * 8 * FONT_SCALE;
    draw_text(fb, display_width - dt_width - 10, row1_y, datetime_str, 255, 255, 255, FONT_SCALE);

    // Anime rows (one per line)
    for (int row = 0; row < status_cache.num_lines; row++) {
        // Separator between rows
        int sep_y = STATUS_ROW_HEIGHT * (1 + row) - 1;
        for (int x = 0; x < display_width; x++) {
            set_pixel(fb, x, sep_y, 35, 35, 45);
        }

        int row_y = STATUS_ROW_HEIGHT * (1 + row) + (STATUS_ROW_HEIGHT - 16 * FONT_SCALE) / 2;
        draw_text(fb, 10, row_y, status_cache.anime_lines[row], 180, 150, 255, FONT_SCALE);
    }
}

// Status bar refresh thread
void *status_refresh_thread(void *arg) {
    while (running) {
        sleep(30);
        if (!running) break;
        render_notes();
    }
    return NULL;
}

// Check if all notes fit at a given scale. Returns 1 if they fit, 0 if overflow.
int notes_fit_at_scale(float scale, int display_width, int display_height) {
    int padding = 20;
    int current_x = padding;
    int current_y = get_status_bar_height() + padding;
    int row_height = 0;
    int max_notes_per_row = 10;
    int notes_in_current_row = 0;

    for (int n = 0; n < store.num_notes; n++) {
        Note *note = &store.notes[n];
        if (note->is_linebreak) {
            current_x = padding;
            current_y += row_height + padding;
            row_height = 0;
            notes_in_current_row = 0;
            continue;
        }

        int scaled_width = (int)(note->width * scale + 0.5f);
        int scaled_height = (int)(note->height * scale + 0.5f);

        if (current_x + scaled_width + padding > display_width ||
            notes_in_current_row >= max_notes_per_row) {
            current_x = padding;
            current_y += row_height + padding;
            row_height = 0;
            notes_in_current_row = 0;
        }

        if (current_y + scaled_height > display_height) {
            return 0;
        }

        current_x += scaled_width + padding;
        if (scaled_height > row_height) row_height = scaled_height;
        notes_in_current_row++;
    }
    return 1;
}

// Render all notes
void render_notes() {
    pthread_mutex_lock(&store.lock);
    clear_screen(&fb);

    // Draw status bar at top
    draw_status_bar(&fb);

    // Get logical display size based on orientation
    int display_width, display_height;
    get_display_size(&fb, &display_width, &display_height);

    // Find the best scale: start at configured scale, shrink if notes overflow
    float scale = config.note_scale;
    while (scale > MIN_NOTE_SCALE && !notes_fit_at_scale(scale, display_width, display_height)) {
        scale -= 0.05f;
    }
    if (scale < MIN_NOTE_SCALE) scale = MIN_NOTE_SCALE;

    int padding = 20;
    int current_x = padding;
    int current_y = get_status_bar_height() + padding;
    int row_height = 0;
    int max_notes_per_row = 10;
    int notes_in_current_row = 0;

    for (int n = 0; n < store.num_notes; n++) {
        Note *note = &store.notes[n];

        if (note->is_linebreak) {
            current_x = padding;
            current_y += row_height + padding;
            row_height = 0;
            notes_in_current_row = 0;
            continue;
        }

        int scaled_width = (int)(note->width * scale + 0.5f);
        int scaled_height = (int)(note->height * scale + 0.5f);

        if (current_x + scaled_width + padding > display_width ||
            notes_in_current_row >= max_notes_per_row) {
            current_x = padding;
            current_y += row_height + padding;
            row_height = 0;
            notes_in_current_row = 0;
        }

        // Stop drawing if we've hit the bottom (at min scale)
        if (current_y + scaled_height > display_height) break;

        int anchor_x = current_x;
        int anchor_y = current_y;

        // Draw strokes (set_pixel handles coordinate transformation)
        for (int s = 0; s < note->num_strokes; s++) {
            Stroke *stroke = &note->strokes[s];
            if (stroke->num_points < 2) continue;

            int x_prev = (int)(stroke->points[0].x * scale + anchor_x + 0.5f);
            int y_prev = (int)(stroke->points[0].y * scale + anchor_y + 0.5f);
            int thickness = (int)(8 * scale);
            if (thickness < 1) thickness = 1;

            for (int p = 1; p < stroke->num_points; p++) {
                int x_curr = (int)(stroke->points[p].x * scale + anchor_x + 0.5f);
                int y_curr = (int)(stroke->points[p].y * scale + anchor_y + 0.5f);

                draw_line(&fb, x_prev, y_prev, x_curr, y_curr, 255, 255, 255, thickness);

                x_prev = x_curr;
                y_prev = y_curr;
            }
        }

        current_x += scaled_width + padding;
        if (scaled_height > row_height) row_height = scaled_height;
        notes_in_current_row++;
    }

    // Flip back buffer to screen in one shot
    fb_flip(&fb);

    pthread_mutex_unlock(&store.lock);
}

// Parse JSON note
int parse_json_note(const char *json, Note *note) {
    memset(note, 0, sizeof(Note));
    
    // Parse width and height
    const char *w = strstr(json, "\"width\":");
    const char *h = strstr(json, "\"height\":");
    if (w) sscanf(w + 8, "%f", &note->width);
    if (h) sscanf(h + 9, "%f", &note->height);
    
   const char *lb = strstr(json, "\"linebreak\":true");
   note->is_linebreak = (lb != NULL) ? 1 : 0;
 
// Find strokes
    const char *strokes_start = strstr(json, "\"strokes\":[");
    if (!strokes_start) return -1;
    
    strokes_start += 10;
    
    // Parse each stroke
    const char *ptr = strokes_start;
    int stroke_idx = 0;
    
    while (*ptr && stroke_idx < MAX_STROKES_PER_NOTE) {
        const char *stroke_obj = strstr(ptr, "{\"points\":");
        if (!stroke_obj) break;
        
        const char *points_start = strchr(stroke_obj, '[');
        if (!points_start) break;
        
        Stroke *stroke = &note->strokes[stroke_idx];
        stroke->num_points = 0;
        
        const char *p = points_start + 1;
        int bracket_depth = 1;
        
        while (*p && bracket_depth > 0 && stroke->num_points < MAX_POINTS_PER_STROKE) {
            if (*p == '[') {
                bracket_depth++;
            } else if (*p == ']') {
                bracket_depth--;
                if (bracket_depth == 0) break;
            } else if (*p == '{') {
                float x = 0, y = 0;
                
                const char *x_pos = strstr(p, "\"x\":");
                if (x_pos) {
                    const char *x_val = x_pos + 4;
                    while (*x_val && (*x_val == ' ' || *x_val == '\t')) x_val++;
                    sscanf(x_val, "%f", &x);
                }
                
                const char *y_pos = strstr(p, "\"y\":");
                if (y_pos) {
                    const char *y_val = y_pos + 4;
                    while (*y_val && (*y_val == ' ' || *y_val == '\t')) y_val++;
                    sscanf(y_val, "%f", &y);
                }
                
                stroke->points[stroke->num_points].x = x;
                stroke->points[stroke->num_points].y = y;
                stroke->num_points++;
                
                p = strchr(p, '}');
                if (!p) break;
            }
            p++;
        }
        
        stroke_idx++;
        ptr = strchr(stroke_obj, '}');
        if (!ptr) break;
        ptr++;
    }
    
    note->num_strokes = stroke_idx;
    return (note->num_strokes > 0 || note->is_linebreak) ? 0 : -1;
}

// Read full HTTP request
int read_full_request(int client_socket, char *buffer, int buffer_size) {
    int total_read = 0;
    int content_length = -1;
    
    // Read headers
    while (total_read < buffer_size - 1) {
        int n = read(client_socket, buffer + total_read, buffer_size - total_read - 1);
        if (n <= 0) break;
        
        total_read += n;
        buffer[total_read] = '\0';
        
        if (strstr(buffer, "\r\n\r\n")) {
            char *cl_header = strstr(buffer, "Content-Length: ");
            if (cl_header) {
                sscanf(cl_header + 16, "%d", &content_length);
            }
            break;
        }
    }
    
    // Read body if needed
    if (content_length > 0) {
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            int body_read = total_read - (body_start - buffer);
            
            while (body_read < content_length && total_read < buffer_size - 1) {
                int n = read(client_socket, buffer + total_read, 
                           buffer_size - total_read - 1);
                if (n <= 0) break;
                
                total_read += n;
                body_read += n;
                buffer[total_read] = '\0';
            }
        }
    }
    
    return total_read;
}

// Handle HTTP request
void handle_request(int client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_read = read_full_request(client_socket, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    
    buffer[bytes_read] = '\0';
    
    char method[16] = {0};
    char path[256] = {0};
    sscanf(buffer, "%15s %255s", method, path);
    
    char *body = strstr(buffer, "\r\n\r\n");
    if (body) body += 4;
    
    // Handle /receive_note
    if (strcmp(method, "POST") == 0 && strcmp(path, "/receive_note") == 0) {
        if (body && store.num_notes < MAX_NOTES) {
            Note note;
            if (parse_json_note(body, &note) == 0) {
                pthread_mutex_lock(&store.lock);
                store.notes[store.num_notes++] = note;
                save_notes();
                pthread_mutex_unlock(&store.lock);

                render_notes();
                
                const char *response = 
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "\r\n"
                    "{\"status\":\"success\"}";
                write(client_socket, response, strlen(response));
            } else {
                const char *response = "HTTP/1.1 400 Bad Request\r\n\r\n";
                write(client_socket, response, strlen(response));
            }
        } else {
            const char *response = "HTTP/1.1 507 Insufficient Storage\r\n\r\n";
            write(client_socket, response, strlen(response));
        }
    }
    // Handle /clear_notes
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/clear_notes") == 0) {
        pthread_mutex_lock(&store.lock);
        store.num_notes = 0;
        save_notes();
        pthread_mutex_unlock(&store.lock);

        render_notes();
        
        const char *response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
            "{\"status\":\"success\"}";
        write(client_socket, response, strlen(response));
    }
    // Handle OPTIONS
    else if (strcmp(method, "OPTIONS") == 0) {
        const char *response = 
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "\r\n";
        write(client_socket, response, strlen(response));
    }
    else {
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        write(client_socket, response, strlen(response));
    }
    
    close(client_socket);
}

// Server thread
void *server_thread(void *arg) {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) return NULL;
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(server_socket);
        return NULL;
    }
    
    if (listen(server_socket, 10) < 0) {
        close(server_socket);
        return NULL;
    }
    
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) continue;
        
        handle_request(client_socket);
    }
    
    close(server_socket);
    return NULL;
}

int main() {
    // Hide cursor
    printf("\033[?25l");
    fflush(stdout);
    
    // Initialize
    pthread_mutex_init(&store.lock, NULL);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    server_start_time = time(NULL);

    // Load config and saved notes
    load_config(CONFIG_PATH, &config);
    load_notes();

    if (fb_init(&fb, "/dev/fb0") != 0) {
        fprintf(stderr, "Failed to initialize framebuffer\n");
        return 1;
    }
    
    // Initial render with status bar
    render_notes();

    // Start server
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, server_thread, NULL);

    // Start status bar refresh thread
    pthread_t status_tid;
    pthread_create(&status_tid, NULL, status_refresh_thread, NULL);

    // Wait
    pthread_join(server_tid, NULL);
    pthread_join(status_tid, NULL);
    
    // Cleanup
    printf("\033[?25h");
    fflush(stdout);
    munmap(fb.fbp, fb.screensize);
    free(fb.backbuf);
    close(fb.fd);
    pthread_mutex_destroy(&store.lock);
    
    return 0;
}