/*
 * rss.c - RSS feed module rendering
 */
#include "pinote.h"

// ============================================================
// Global
// ============================================================

RssCache rss_cache = {0};

// ============================================================
// RSS module
// ============================================================

int get_rss_height(void) {
    if (rss_cache.num_items == 0) return 0;
    int max_items = config.max_rss_items > 0 ? config.max_rss_items : 6;
    int count = rss_cache.num_items < max_items ? rss_cache.num_items : max_items;
    int per_line = config.rss_per_line <= 0 ? 1 : config.rss_per_line;
    int rows = (count + per_line - 1) / per_line;
    return rows * STATUS_ROW_HEIGHT;
}

// Draw a single RSS item (date in accent color, title in light gray)
// Returns the total pixel width drawn (for right-alignment measurement)
static int draw_rss_item(Framebuffer *fb, int draw_x, int draw_y, int item_idx,
                         int max_title_chars, int fs) {
    RssItem *item = &rss_cache.items[item_idx];

    // Draw date in accent color
    int tx = draw_text(fb, draw_x, draw_y, item->date_str, 100, 200, 255, fs);
    tx = draw_text(fb, tx, draw_y, "  ", 100, 200, 255, fs);

    // Draw title in light gray, truncated if needed
    char title[256];
    strncpy(title, item->title, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    if (config.rss_truncate > 0) truncate_with_dots(title, config.rss_truncate);
    if (max_title_chars > 0) truncate_with_dots(title, max_title_chars);

    draw_text(fb, tx, draw_y, title, 220, 220, 230, fs);
    return 0;
}

// Measure the full width of an RSS item string (date + spacing + title)
static int measure_rss_item(int item_idx, int max_title_chars, int char_w) {
    RssItem *item = &rss_cache.items[item_idx];

    char title[256];
    strncpy(title, item->title, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    if (config.rss_truncate > 0) truncate_with_dots(title, config.rss_truncate);
    if (max_title_chars > 0) truncate_with_dots(title, max_title_chars);

    // date + "  " + title
    return (strlen(item->date_str) + 2 + strlen(title)) * char_w;
}

void draw_rss(Framebuffer *fb, int x, int y, int w, int h) {
    if (rss_cache.num_items == 0) return;

    // Background
    fill_rect(fb, x, y, w, h, BG_MODULE_R, BG_MODULE_G, BG_MODULE_B);

    int line_height = STATUS_ROW_HEIGHT;
    int max_items = config.max_rss_items > 0 ? config.max_rss_items : 6;
    int per_line = config.rss_per_line <= 0 ? 1 : config.rss_per_line;
    int count = rss_cache.num_items;
    if (count > max_items) count = max_items;

    int fs = FONT_SCALE;
    int char_w = 8 * fs;
    int char_h = 16 * fs;
    int row = 0;

    // Calculate max title chars based on available width
    int avail_w = (per_line >= 2) ? w / 2 - 10 : w - 20;
    // Reserve space for date ("Mar 13") + "  " = ~8 chars
    int max_title_chars = (avail_w / char_w) - 8;
    if (max_title_chars < 3) max_title_chars = 3;

    for (int i = 0; i < count; i += per_line) {
        if (y + row * line_height + line_height > y + h) break;

        // Separator
        fill_rect(fb, x, y + row * line_height, w, 1, SEP_MINOR_R, SEP_MINOR_G, SEP_MINOR_B);

        int text_y = y + row * line_height + (line_height - char_h) / 2;

        // Left entry
        draw_rss_item(fb, x + 10, text_y, i, max_title_chars, fs);

        // Right entry (if paired and exists)
        if (per_line >= 2 && i + 1 < count) {
            int tw = measure_rss_item(i + 1, max_title_chars, char_w);
            draw_rss_item(fb, x + w - tw - 10, text_y, i + 1, max_title_chars, fs);
        }

        row++;
    }

    // Bottom separator
    fill_rect(fb, x, y + h - 1, w, 1, SEP_MAJOR_R, SEP_MAJOR_G, SEP_MAJOR_B);
}
