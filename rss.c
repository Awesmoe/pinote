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

// How many display rows an item needs (1 or 2 if wrapping)
static int item_rows(int item_idx, int max_title_chars) {
    if (!config.rss_wrap || max_title_chars <= 0) return 1;
    int title_len = strlen(rss_cache.items[item_idx].title);
    if (config.rss_truncate > 0 && title_len > config.rss_truncate)
        title_len = config.rss_truncate;
    return (title_len > max_title_chars) ? 2 : 1;
}

int get_rss_height(void) {
    if (rss_cache.num_items == 0) return 0;
    int max_items = config.max_rss_items > 0 ? config.max_rss_items : 6;
    int count = rss_cache.num_items < max_items ? rss_cache.num_items : max_items;
    int per_line = config.rss_per_line <= 0 ? 1 : config.rss_per_line;

    int display_w, display_h;
    get_display_size(&fb, &display_w, &display_h);
    int fs = FONT_SCALE;
    int char_w = 8 * fs;
    int avail_w = (per_line >= 2) ? (display_w / 2 - 10) : (display_w - 20);
    int max_title_chars = (avail_w / char_w) - 8;
    if (max_title_chars < 3) max_title_chars = 3;

    int rows = 0;
    for (int i = 0; i < count; i += per_line) {
        int row_h = 1;
        if (config.rss_wrap) {
            for (int j = 0; j < per_line && i + j < count; j++) {
                int h = item_rows(i + j, max_title_chars);
                if (h > row_h) row_h = h;
            }
        }
        rows += row_h;
    }
    return rows * STATUS_ROW_HEIGHT;
}

// Draw a single RSS item (date in accent color, title in light gray)
// When wrap is enabled and title overflows, draws second line below
static void draw_rss_item(Framebuffer *fb, int draw_x, int draw_y, int item_idx,
                          int max_title_chars, int fs) {
    RssItem *item = &rss_cache.items[item_idx];
    int char_w = 8 * fs;

    // Draw date in accent color
    int tx = draw_text(fb, draw_x, draw_y, item->date_str, 100, 200, 255, fs);
    tx = draw_text(fb, tx, draw_y, "  ", 100, 200, 255, fs);

    // Prepare title
    char title[256];
    strncpy(title, item->title, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    if (config.rss_truncate > 0) truncate_with_dots(title, config.rss_truncate);

    if (config.rss_wrap && max_title_chars > 0 && (int)strlen(title) > max_title_chars) {
        // First line: break at last space before max_title_chars
        int break_at = max_title_chars;
        for (int b = max_title_chars - 1; b > 0; b--) {
            if (title[b] == ' ') { break_at = b; break; }
        }
        char line1[256];
        strncpy(line1, title, break_at);
        line1[break_at] = '\0';
        draw_text(fb, tx, draw_y, line1, 220, 220, 230, fs);

        // Second line: remainder, indented to align with title start
        int indent = (strlen(item->date_str) + 2) * char_w;
        char *line2 = title + break_at;
        while (*line2 == ' ') line2++;  // skip the space at the break
        // Truncate second line to same width
        char line2_buf[256];
        strncpy(line2_buf, line2, sizeof(line2_buf) - 1);
        line2_buf[sizeof(line2_buf) - 1] = '\0';
        truncate_with_dots(line2_buf, max_title_chars);
        int line2_y = draw_y + STATUS_ROW_HEIGHT;
        draw_text(fb, draw_x + indent, line2_y, line2_buf, 220, 220, 230, fs);
    } else {
        if (max_title_chars > 0) truncate_with_dots(title, max_title_chars);
        draw_text(fb, tx, draw_y, title, 220, 220, 230, fs);
    }
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

    // Calculate max title chars based on available width
    int avail_w = (per_line >= 2) ? w / 2 - 10 : w - 20;
    // Reserve space for date ("Mar 13") + "  " = ~8 chars
    int max_title_chars = (avail_w / char_w) - 8;
    if (max_title_chars < 3) max_title_chars = 3;

    int cur_y = y;
    for (int i = 0; i < count; i += per_line) {
        // Determine row height (max of items in this row)
        int row_h = 1;
        if (config.rss_wrap) {
            for (int j = 0; j < per_line && i + j < count; j++) {
                int h2 = item_rows(i + j, max_title_chars);
                if (h2 > row_h) row_h = h2;
            }
        }
        int row_px = row_h * line_height;

        if (cur_y + row_px > y + h) break;

        // Separator
        fill_rect(fb, x, cur_y, w, 1, SEP_MINOR_R, SEP_MINOR_G, SEP_MINOR_B);

        int text_y = cur_y + (line_height - char_h) / 2;

        // Left entry
        draw_rss_item(fb, x + 10, text_y, i, max_title_chars, fs);

        // Right entry (if paired and exists)
        if (per_line >= 2 && i + 1 < count) {
            int tw = measure_rss_item(i + 1, max_title_chars, char_w);
            draw_rss_item(fb, x + w - tw - 10, text_y, i + 1, max_title_chars, fs);
        }

        cur_y += row_px;
    }

    // Bottom separator
    fill_rect(fb, x, y + h - 1, w, 1, SEP_MAJOR_R, SEP_MAJOR_G, SEP_MAJOR_B);
}
