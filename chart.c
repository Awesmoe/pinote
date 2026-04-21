/*
 * chart.c - Chart rendering on framebuffer
 */
#include "pinote.h"

void draw_chart(Framebuffer *fb, int chart_x, int chart_y, int chart_w, int chart_h) {
    if (chart_data.num_sensors == 0 || config.chart_height <= 0) return;

    int fs = FONT_SCALE;           // all labels use the same scale
    int char_w = 8 * fs;           // pixel width of one character
    int char_h = 16 * fs;          // pixel height of one character

    int margin_left = char_w * 4;  // room for Y labels like "-10"
    int margin_right = char_w * 4; // room for right Y labels
    int margin_top = 5;
    int margin_bottom = char_h * 2 + 20; // X labels + gap + legend

    int plot_x = chart_x + margin_left;
    int plot_y = chart_y + margin_top;
    int plot_w = chart_w - margin_left - margin_right;
    int plot_h = chart_h - margin_top - margin_bottom;

    if (plot_w < 50 || plot_h < 30) return;

    // Find min/max value and time across all sensors
    float val_min = 999, val_max = -999;
    time_t time_min = 0, time_max = 0;

    for (int s = 0; s < chart_data.num_sensors; s++) {
        SensorData *sd = &chart_data.sensors[s];
        for (int i = 0; i < sd->num_points; i++) {
            if (sd->points[i].value < val_min) val_min = sd->points[i].value;
            if (sd->points[i].value > val_max) val_max = sd->points[i].value;
            if (time_min == 0 || sd->points[i].timestamp < time_min)
                time_min = sd->points[i].timestamp;
            if (sd->points[i].timestamp > time_max)
                time_max = sd->points[i].timestamp;
        }
    }

    double time_range = difftime(time_max, time_min);
    if (time_range <= 0) return;

    // Pick a nice Y-axis tick step aiming for ~4-6 ticks
    int range_raw = (int)ceilf(val_max) - (int)floorf(val_min);
    int tick_step;
    if (range_raw <= 8)       tick_step = 2;
    else if (range_raw <= 20) tick_step = 5;
    else                      tick_step = 10;

    // Align axis boundaries to tick step
    int y_min = (int)(floorf(val_min / tick_step) * tick_step);
    int y_max = (int)(ceilf(val_max / tick_step) * tick_step);
    if (y_min == y_max) { y_min -= tick_step; y_max += tick_step; }
    float axis_min = (float)y_min;
    float axis_max = (float)y_max;
    float axis_range = axis_max - axis_min;

    // Subtle chart background
    if (!config.modules_transparent)
        fill_rect(fb, chart_x, chart_y, chart_w, chart_h, BG_CHART_R, BG_CHART_G, BG_CHART_B);

    // Horizontal grid lines and Y labels at tick values
    for (int t = y_min; t <= y_max; t += tick_step) {
        int y = plot_y + plot_h - (int)(((float)t - axis_min) / axis_range * plot_h);

        // Dotted grid line
        for (int x = plot_x; x < plot_x + plot_w; x += 4)
            set_pixel(fb, x, y, 45, 45, 55);

        // Y label
        char label[16];
        snprintf(label, sizeof(label), "%d", t);
        int lw = strlen(label) * char_w;
        draw_text(fb, plot_x - lw - 6, y - char_h / 2, label, 140, 140, 140, fs);
    }

    // Vertical grid lines and X labels — snapped to midnight (or 6h for short ranges)
    int num_days = (int)(time_range / 86400.0);
    int xlabel_y = plot_y + plot_h + 6;

    if (num_days <= 2) {
        // Short range: snap to 6-hour intervals, show HH:MM
        struct tm tm_start;
        localtime_r(&time_min, &tm_start);
        // Round up to next 6-hour boundary
        int h = tm_start.tm_hour;
        int next_h = ((h / 6) + 1) * 6;
        struct tm tm_tick = tm_start;
        tm_tick.tm_hour = next_h;
        tm_tick.tm_min = 0;
        tm_tick.tm_sec = 0;
        time_t tick = mktime(&tm_tick);

        while (tick <= time_max) {
            double t_off = difftime(tick, time_min);
            int x = plot_x + (int)(t_off / time_range * plot_w);
            if (x >= plot_x && x <= plot_x + plot_w) {
                for (int y = plot_y; y < plot_y + plot_h; y += 4)
                    set_pixel(fb, x, y, 45, 45, 55);
                struct tm tm_buf;
                localtime_r(&tick, &tm_buf);
                char label[16];
                strftime(label, sizeof(label), "%H:%M", &tm_buf);
                int lw = strlen(label) * char_w;
                draw_text(fb, x - lw / 2, xlabel_y, label, 140, 140, 140, fs);
            }
            tick += 6 * 3600;
        }
    } else {
        // Multi-day range: snap to midnight boundaries, show day name
        struct tm tm_start;
        localtime_r(&time_min, &tm_start);
        // Round up to next midnight
        struct tm tm_tick = tm_start;
        tm_tick.tm_hour = 0;
        tm_tick.tm_min = 0;
        tm_tick.tm_sec = 0;
        tm_tick.tm_mday += 1;  // next day
        time_t tick = mktime(&tm_tick);

        while (tick <= time_max) {
            double t_off = difftime(tick, time_min);
            int x = plot_x + (int)(t_off / time_range * plot_w);
            if (x >= plot_x && x <= plot_x + plot_w) {
                for (int y = plot_y; y < plot_y + plot_h; y += 4)
                    set_pixel(fb, x, y, 45, 45, 55);
                struct tm tm_buf;
                localtime_r(&tick, &tm_buf);
                char label[16];
                strftime(label, sizeof(label), "%a", &tm_buf);
                int lw = strlen(label) * char_w;
                draw_text(fb, x - lw / 2, xlabel_y, label, 140, 140, 140, fs);
            }
            tick += 86400;
        }
    }

    // Axes (left, bottom, right)
    for (int y = plot_y; y <= plot_y + plot_h; y++) {
        set_pixel(fb, plot_x, y, 80, 80, 90);
        set_pixel(fb, plot_x + plot_w, y, 80, 80, 90);
    }
    for (int x = plot_x; x <= plot_x + plot_w; x++)
        set_pixel(fb, x, plot_y + plot_h, 80, 80, 90);

    // Right-side Y labels (mirrored)
    for (int t = y_min; t <= y_max; t += tick_step) {
        int y = plot_y + plot_h - (int)(((float)t - axis_min) / axis_range * plot_h);
        char label[16];
        snprintf(label, sizeof(label), "%d", t);
        draw_text(fb, plot_x + plot_w + 6, y - char_h / 2, label, 140, 140, 140, fs);
    }

    // Data lines for each sensor
    for (int s = 0; s < chart_data.num_sensors; s++) {
        SensorData *sd = &chart_data.sensors[s];
        if (sd->num_points < 2) continue;

        int prev_px = -1, prev_py = -1;
        for (int i = 0; i < sd->num_points; i++) {
            double t_off = difftime(sd->points[i].timestamp, time_min);
            int px = plot_x + (int)(t_off / time_range * plot_w);
            int py = plot_y + plot_h - (int)((sd->points[i].value - axis_min)
                                              / axis_range * plot_h);

            // Clamp to plot area
            if (px < plot_x) px = plot_x;
            if (px > plot_x + plot_w) px = plot_x + plot_w;
            if (py < plot_y) py = plot_y;
            if (py > plot_y + plot_h) py = plot_y + plot_h;

            if (prev_px >= 0)
                draw_line(fb, prev_px, prev_py, px, py, sd->r, sd->g, sd->b, 2);

            prev_px = px;
            prev_py = py;
        }
    }

    // Legend below X labels (sensors are alphabetically sorted, colors consistent everywhere)
    int legend_y = xlabel_y + char_h + 6;
    int legend_x = plot_x;
    for (int s = 0; s < chart_data.num_sensors; s++) {
        SensorData *sd = &chart_data.sensors[s];
        fill_rect(fb, legend_x, legend_y + 4, 12, 12, sd->r, sd->g, sd->b);
        legend_x += 16;
        legend_x = draw_text(fb, legend_x, legend_y, sd->name, sd->r, sd->g, sd->b, fs);
        legend_x += 24;
    }

}
