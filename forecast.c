/*
 * forecast.c - Weather forecast module rendering
 */
#include "pinote.h"

// ============================================================
// Global
// ============================================================

ForecastCache forecast_cache = {0};

// ============================================================
// Weather code mapping
// ============================================================

// Map WMO weather code to short label and color
static void weather_condition(int code, const char **label,
                              uint8_t *r, uint8_t *g, uint8_t *b) {
    if (code == 0) {
        *label = "Clear";   *r = 255; *g = 220; *b = 50;
    } else if (code <= 3) {
        *label = "Cloudy";  *r = 180; *g = 180; *b = 200;
    } else if (code == 45 || code == 48) {
        *label = "Fog";     *r = 150; *g = 150; *b = 170;
    } else if (code <= 57) {
        *label = "Drizzle"; *r = 100; *g = 180; *b = 255;
    } else if (code <= 67) {
        *label = "Rain";    *r = 50;  *g = 130; *b = 255;
    } else if (code <= 77) {
        *label = "Snow";    *r = 230; *g = 240; *b = 255;
    } else if (code <= 82) {
        *label = "Showers"; *r = 70;  *g = 160; *b = 255;
    } else if (code <= 86) {
        *label = "Sleet";   *r = 150; *g = 200; *b = 255;
    } else if (code <= 94) {
        *label = "Storm";   *r = 255; *g = 100; *b = 100;
    } else if (code >= 95) {
        *label = "Storm";   *r = 255; *g = 100; *b = 100;
    } else {
        *label = "?";       *r = 180; *g = 180; *b = 180;
    }
}

// ============================================================
// Forecast module
// ============================================================

int get_forecast_height(void) {
    if (forecast_cache.num_days == 0) return 0;
    return forecast_cache.num_days * STATUS_ROW_HEIGHT;
}

void draw_forecast(Framebuffer *fb, int x, int y, int w, int h) {
    if (forecast_cache.num_days == 0) return;

    // Background
    fill_rect(fb, x, y, w, h, BG_MODULE_R, BG_MODULE_G, BG_MODULE_B);

    int line_height = STATUS_ROW_HEIGHT;
    int fs = FONT_SCALE;
    int char_w = 8 * fs;
    int char_h = 16 * fs;
    int full_mode = (w >= char_w * 35);

    // Stale data indicator: dim * in top-right if forecast hasn't refreshed recently
    if (forecast_cache.last_fetched > 0 &&
        status_cache.last_fetched - forecast_cache.last_fetched >= config.refresh_interval) {
        draw_text(fb, x + w - char_w - 4, y + 2, "*", 100, 100, 100, fs);
    }

    for (int i = 0; i < forecast_cache.num_days; i++) {
        if (y + i * line_height + line_height > y + h) break;

        ForecastDay *day = &forecast_cache.days[i];

        // Separator
        fill_rect(fb, x, y + i * line_height, w, 1, SEP_MINOR_R, SEP_MINOR_G, SEP_MINOR_B);

        int text_y = y + i * line_height + (line_height - char_h) / 2;
        int tx = x + 10;

        // Day name in accent color
        tx = draw_text(fb, tx, text_y, day->day_name, 100, 200, 255, fs);
        tx += char_w * 2;

        // Condition in weather color
        const char *cond_label;
        uint8_t cr, cg, cb;
        weather_condition(day->weather_code, &cond_label, &cr, &cg, &cb);
        tx = draw_text(fb, tx, text_y, cond_label, cr, cg, cb, fs);
        tx += char_w * 2;

        // Temps: "14\xf8/8\xf8"
        char temp_str[32];
        snprintf(temp_str, sizeof(temp_str), "%d\xf8/%d\xf8",
                 (int)roundf(day->temp_max), (int)roundf(day->temp_min));
        tx = draw_text(fb, tx, text_y, temp_str, 220, 220, 230, fs);

        // Extra fields in full mode
        if (full_mode) {
            tx += char_w * 2;

            char wind_str[32];
            snprintf(wind_str, sizeof(wind_str), "Wind %dkm/h", (int)roundf(day->wind_max));
            tx = draw_text(fb, tx, text_y, wind_str, 160, 160, 170, fs);
            tx += char_w * 2;

            char rain_str[32];
            snprintf(rain_str, sizeof(rain_str), "Rain %dmm", (int)roundf(day->precip_mm));
            tx = draw_text(fb, tx, text_y, rain_str, 160, 160, 170, fs);
            tx += char_w * 2;

            char uv_str[16];
            snprintf(uv_str, sizeof(uv_str), "UV %d", (int)roundf(day->uv_max));
            draw_text(fb, tx, text_y, uv_str, 160, 160, 170, fs);
        }
    }

    // Bottom separator
    fill_rect(fb, x, y + h - 1, w, 1, SEP_MAJOR_R, SEP_MAJOR_G, SEP_MAJOR_B);
}
