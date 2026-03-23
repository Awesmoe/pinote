/*
 * sprite.c - Animated sprite overlay
 *
 * Draws a small animated sprite directly to the front buffer at ~10fps.
 * Each pixel is either the sprite pixel or restored from the back buffer
 * in a single pass — no flicker.
 *
 * The sprite data is a const array in sprite_data.h, generated from a BMP
 * sprite sheet by convert_sprite.c. Green-screen pixels are transparent.
 */
#include "pinote.h"
#include "sprite_data.h"

// Current sprite state (read by render_screen to redraw after fb_flip)
static int sprite_frame = 0;
static int sprite_x = -1;
static int sprite_y = -1;

// Sprite position (logical coordinates, bottom-right of screen)
static void get_sprite_position(int *sx, int *sy) {
    int w, h;
    get_display_size(&fb, &w, &h);
    *sx = w - SPRITE_W - 10;
    *sy = h - SPRITE_H - 10;
    if (*sx < 0) *sx = 0;
    if (*sy < 0) *sy = 0;
}

// Fuzzy transparency check — catches any green-screen pixel (high G, low R, low B)
static inline int is_transparent(uint8_t r, uint8_t g, uint8_t b) {
    return (g > 150 && r < 100 && b < 100);
}

// Draw sprite frame to front buffer
// If restore_bg is set, transparent pixels are restored from backbuf (for animation).
// If not, transparent pixels are skipped (after fb_flip, backbuf already matches).
static void draw_sprite_to_front(int frame, int sx, int sy, int restore_bg) {
    for (int row = 0; row < SPRITE_H; row++) {
        for (int col = 0; col < SPRITE_W; col++) {
            uint8_t r = sprite_pixels[frame][row][col][0];
            uint8_t g = sprite_pixels[frame][row][col][1];
            uint8_t b = sprite_pixels[frame][row][col][2];
            if (is_transparent(r, g, b)) {
                if (restore_bg)
                    fb_restore_pixel(&fb, sx + col, sy + row);
            } else {
                set_pixel_front(&fb, sx + col, sy + row, r, g, b);
            }
        }
    }
}

// Called from render_screen() right after fb_flip() to prevent sprite disappearing
void sprite_redraw_after_flip(void) {
    if (!config.sprite_enabled || sprite_x < 0) return;
    draw_sprite_to_front(sprite_frame, sprite_x, sprite_y, 0);
}

void *sprite_thread(void *arg) {
    (void)arg;
    struct timespec ts;

    if (!config.sprite_enabled) return NULL;

    // Wait for first render so backbuf has valid content and position is correct
    while (!first_render_done && running)
        usleep(50000);

    get_sprite_position(&sprite_x, &sprite_y);

    while (running) {
        // Draw current frame (combined restore + draw, no flicker)
        draw_sprite_to_front(sprite_frame, sprite_x, sprite_y, 1);

        // Advance frame
        sprite_frame = (sprite_frame + 1) % SPRITE_FRAMES;

        // Sleep for frame duration
        long ms = SPRITE_FRAME_MS;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }

    return NULL;
}
