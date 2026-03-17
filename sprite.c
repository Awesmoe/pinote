/*
 * sprite.c - Animated sprite overlay
 *
 * Draws a small animated sprite directly to the front buffer at ~10fps.
 * Each pixel is either the sprite pixel or restored from the back buffer
 * in a single pass — no flicker.
 *
 * The sprite data is a const array in sprite_data.h, generated from a BMP
 * sprite sheet by convert_sprite.c. Magenta (#FF00FF) pixels are transparent.
 */
#include "pinote.h"
#include "sprite_data.h"

// Current sprite state (read by render_screen to redraw after fb_flip)
static int sprite_frame = 0;
static int sprite_x = 0;
static int sprite_y = 0;

// Sprite position (logical coordinates, bottom-right of screen)
static void get_sprite_position(int *sx, int *sy) {
    int w, h;
    get_display_size(&fb, &w, &h);
    *sx = w - SPRITE_W - 10;
    *sy = h - SPRITE_H - 10;
}

// Fuzzy transparency check — catches any magenta-ish pixel (high R, high B, low G)
static inline int is_transparent(uint8_t r, uint8_t g, uint8_t b) {
    return (r > 150 && b > 150 && g < 100);
}

// Draw sprite frame to front buffer, restoring transparent pixels from backbuf
// Combined pass = no flicker
static void draw_sprite_composite(int frame, int sx, int sy) {
    for (int row = 0; row < SPRITE_H; row++) {
        for (int col = 0; col < SPRITE_W; col++) {
            uint8_t r = sprite_pixels[frame][row][col][0];
            uint8_t g = sprite_pixels[frame][row][col][1];
            uint8_t b = sprite_pixels[frame][row][col][2];
            if (is_transparent(r, g, b)) {
                // Transparent: restore this pixel from back buffer
                fb_restore_pixel(&fb, sx + col, sy + row);
            } else {
                set_pixel_front(&fb, sx + col, sy + row, r, g, b);
            }
        }
    }
}

// Called from render_screen() right after fb_flip() to prevent sprite disappearing
void sprite_redraw_after_flip(void) {
    if (!config.sprite_enabled) return;
    // After flip, front buffer matches back buffer (no sprite).
    // Just draw sprite pixels — transparent pixels already correct from flip.
    for (int row = 0; row < SPRITE_H; row++) {
        for (int col = 0; col < SPRITE_W; col++) {
            uint8_t r = sprite_pixels[sprite_frame][row][col][0];
            uint8_t g = sprite_pixels[sprite_frame][row][col][1];
            uint8_t b = sprite_pixels[sprite_frame][row][col][2];
            if (is_transparent(r, g, b))
                continue;
            set_pixel_front(&fb, sprite_x + col, sprite_y + row, r, g, b);
        }
    }
}

void *sprite_thread(void *arg) {
    (void)arg;
    struct timespec ts;

    if (!config.sprite_enabled) return NULL;

    get_sprite_position(&sprite_x, &sprite_y);

    while (running) {
        // Draw current frame (combined restore + draw, no flicker)
        draw_sprite_composite(sprite_frame, sprite_x, sprite_y);

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
