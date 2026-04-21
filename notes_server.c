/*
 * Pi Notepad Server
 *
 * Modules: fb_draw.c, config.c, api_fetch.c, chart.c, rss.c
 */
#include "pinote.h"

// ============================================================
// Globals
// ============================================================

NoteStore store = {0};
Framebuffer fb = {0};
AppConfig config = {0};
StatusCache status_cache = {0};
volatile int running = 1;
volatile int first_render_done = 0;
int server_socket = -1;
ChartData chart_data = {0};

// Signal handler
static void handle_signal(int sig) {
    (void)sig;
    running = 0;
    if (server_socket != -1) shutdown(server_socket, SHUT_RDWR);
}

// ============================================================
// System info helpers
// ============================================================

// Get the local IP address of the active network interface
static void get_local_ip(char *buf, int bufsize) {
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
static int get_wifi_signal(void) {
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
static void format_signal_bars(int dbm, char *buf, int bufsize) {
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

// ============================================================
// Note persistence
// ============================================================

void save_notes(void) {
    FILE *f = fopen(NOTES_FILE, "wb");
    if (!f) return;
    fwrite(&store.num_notes, sizeof(int), 1, f);
    fwrite(store.notes, sizeof(Note), store.num_notes, f);
    fclose(f);
}

void load_notes(void) {
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
// Status bar (system info only — always full width at top)
// ============================================================

void draw_status_bar(Framebuffer *fb, int width) {
    int bar_height = STATUS_ROW_HEIGHT;

    // Draw background
    if (!config.modules_transparent)
        fill_rect(fb, 0, 0, width, bar_height, BG_MODULE_R, BG_MODULE_G, BG_MODULE_B);

    // Separator line at bottom
    fill_rect(fb, 0, bar_height - 1, width, 1, SEP_MAJOR_R, SEP_MAJOR_G, SEP_MAJOR_B);

    int row1_y = (STATUS_ROW_HEIGHT - 16 * FONT_SCALE) / 2;
    int section_x = 10;

    // IP address + WiFi signal (cached, refreshed with API data)
    static char cached_ip_str[80] = {0};
    static char cached_bars[16] = {0};
    static time_t last_sysinfo = 0;
    time_t now_si = time(NULL);
    if (now_si - last_sysinfo >= config.refresh_interval || !cached_ip_str[0]
        || strstr(cached_ip_str, "No IP")) {
        char ip[64];
        get_local_ip(ip, sizeof(ip));
        snprintf(cached_ip_str, sizeof(cached_ip_str), "IP: %s:%d", ip, PORT);
        int signal_dbm = get_wifi_signal();
        format_signal_bars(signal_dbm, cached_bars, sizeof(cached_bars));
        last_sysinfo = now_si;
    }

    section_x = draw_text(fb, section_x, row1_y, cached_ip_str, 100, 200, 255, FONT_SCALE);
    section_x += 30;
    section_x = draw_text(fb, section_x, row1_y, cached_bars, 100, 255, 100, FONT_SCALE);
    section_x += 30;

    // Temperatures in alphabetical order (matching chart legend)
    for (int s = 0; s < chart_data.num_sensors; s++) {
        SensorData *sd = &chart_data.sensors[s];
        const char *val = NULL;

        if (strcmp(sd->name, "Outdoor") == 0) {
            if (status_cache.weather_temp[0]) val = status_cache.weather_temp;
        } else {
            for (int i = 0; i < status_cache.num_hue_temps; i++) {
                if (status_cache.hue_names[i][0] &&
                    strcmp(sd->name, status_cache.hue_names[i]) == 0) {
                    val = status_cache.hue_temps[i];
                    break;
                }
            }
        }

        if (val) {
            section_x = draw_text(fb, section_x, row1_y, val, sd->r, sd->g, sd->b, FONT_SCALE);
            section_x += 20;
        }
    }

    // Right side: "Upd HH:MM  DD Mon HH:MM" right-aligned together
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char datetime_str[16];
    strftime(datetime_str, sizeof(datetime_str), "%d %b %H:%M", &tm_buf);

    char right_str[64];
    if (status_cache.last_fetched > 0) {
        struct tm upd_buf;
        localtime_r(&status_cache.last_fetched, &upd_buf);
        char upd_str[16];
        strftime(upd_str, sizeof(upd_str), "Upd %H:%M", &upd_buf);
        snprintf(right_str, sizeof(right_str), "%s  %s", upd_str, datetime_str);
    } else {
        snprintf(right_str, sizeof(right_str), "%s", datetime_str);
    }

    int right_w = strlen(right_str) * 8 * FONT_SCALE;
    int right_x = width - right_w - 10;

    // Draw "Upd HH:MM" in dim, then clock in white
    if (status_cache.last_fetched > 0) {
        struct tm upd_buf2;
        localtime_r(&status_cache.last_fetched, &upd_buf2);
        char upd_str[16];
        strftime(upd_str, sizeof(upd_str), "Upd %H:%M", &upd_buf2);
        int upd_end = draw_text(fb, right_x, row1_y, upd_str, 150, 150, 160, FONT_SCALE);
        upd_end = draw_text(fb, upd_end, row1_y, "  ", 150, 150, 160, FONT_SCALE);
        draw_text(fb, upd_end, row1_y, datetime_str, 255, 255, 255, FONT_SCALE);
    } else {
        draw_text(fb, right_x, row1_y, datetime_str, 255, 255, 255, FONT_SCALE);
    }
}

// ============================================================
// Anime module
// ============================================================

int get_anime_height(void) {
    if (status_cache.num_anime_entries == 0) {
        return (config.num_anime > 0) ? STATUS_ROW_HEIGHT : 0;
    }
    int per_line = config.anime_per_line <= 0 ? 2 : config.anime_per_line;
    int rows = (status_cache.num_anime_entries + per_line - 1) / per_line;
    return rows * STATUS_ROW_HEIGHT;
}

// Pick countdown text color based on how soon it airs
static void anime_countdown_color(long airing_at, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (airing_at <= 0) {
        // TBA or already aired — dim gray
        *r = 150; *g = 150; *b = 160;
        return;
    }
    long diff = airing_at - time(NULL);
    if (diff <= 0) {
        // Just aired — green
        *r = 100; *g = 255; *b = 100;
    } else if (diff < 24 * 3600) {
        // Under 24h — green (urgent)
        *r = 100; *g = 255; *b = 100;
    } else if (diff < 2 * 24 * 3600) {
        // Under 48h — warm yellow/orange (soon)
        *r = 255; *g = 200; *b = 100;
    } else {
        // More than 3 days — default blue accent
        *r = 100; *g = 200; *b = 255;
    }
}

void draw_anime(Framebuffer *fb, int x, int y, int w, int h) {
    if (status_cache.num_anime_entries == 0) {
        if (config.num_anime > 0) {
            if (!config.modules_transparent)
                fill_rect(fb, x, y, w, h, BG_MODULE_R, BG_MODULE_G, BG_MODULE_B);
            fill_rect(fb, x, y, w, 1, SEP_MINOR_R, SEP_MINOR_G, SEP_MINOR_B);
            int text_y = y + (STATUS_ROW_HEIGHT - 16 * FONT_SCALE) / 2;
            draw_text(fb, x + 10, text_y, "Anime: waiting for data...", 90, 90, 100, FONT_SCALE);
        }
        return;
    }

    // Background
    if (!config.modules_transparent)
        fill_rect(fb, x, y, w, h, BG_MODULE_R, BG_MODULE_G, BG_MODULE_B);

    // Fade colors when data is stale (API down, showing cached data)
    int stale = status_cache.anime_last_fetched &&
        status_cache.last_fetched - status_cache.anime_last_fetched >= config.refresh_interval;
    uint8_t title_r = stale ? 100 : 220, title_g = stale ? 100 : 220, title_b = stale ? 110 : 230;
    uint8_t sep_r   = stale ?  70 : 120, sep_g   = stale ?  70 : 110, sep_b   = stale ?  80 : 160;

    int per_line = config.anime_per_line <= 0 ? 2 : config.anime_per_line;
    int row = 0;
    for (int i = 0; i < status_cache.num_anime_entries; i += per_line) {
        // Separator at top of each row
        fill_rect(fb, x, y + row * STATUS_ROW_HEIGHT, w, 1, SEP_MINOR_R, SEP_MINOR_G, SEP_MINOR_B);

        int row_y = y + row * STATUS_ROW_HEIGHT + (STATUS_ROW_HEIGHT - 16 * FONT_SCALE) / 2;

        int char_w = 8 * FONT_SCALE;
        int entry_w = (per_line >= 2) ? w / 2 : w;
        int avail_chars = (entry_w - 20) / char_w;

        // Left entry
        {
            // Reserve space for ": " + countdown, truncate title to fit remainder
            int cd_len = strlen(status_cache.anime_countdowns[i]);
            int reserved = 2 + cd_len;  // ": " + countdown
            int max_title = avail_chars - reserved;
            if (max_title < 3) max_title = 3;

            char title[128];
            strncpy(title, status_cache.anime_titles[i], sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';
            truncate_with_dots(title, max_title);

            int tx = draw_text(fb, x + 10, row_y, title, title_r, title_g, title_b, FONT_SCALE);
            tx = draw_text(fb, tx, row_y, ": ", sep_r, sep_g, sep_b, FONT_SCALE);
            uint8_t cr, cg, cb;
            anime_countdown_color(status_cache.anime_airing_at[i], &cr, &cg, &cb);
            if (stale) { cr = 90; cg = 90; cb = 90; }
            draw_text(fb, tx, row_y, status_cache.anime_countdowns[i], cr, cg, cb, FONT_SCALE);
        }

        // Right entry (if paired and exists)
        if (per_line >= 2 && i + 1 < status_cache.num_anime_entries) {
            int cd_len = strlen(status_cache.anime_countdowns[i + 1]);
            int reserved = 2 + cd_len;
            int max_title = avail_chars - reserved;
            if (max_title < 3) max_title = 3;

            char title[128];
            strncpy(title, status_cache.anime_titles[i + 1], sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';
            truncate_with_dots(title, max_title);

            // Measure full width for right-alignment
            int full_w = (strlen(title) + 2 + cd_len) * char_w;
            int rx = x + w - full_w - 10;

            rx = draw_text(fb, rx, row_y, title, title_r, title_g, title_b, FONT_SCALE);
            rx = draw_text(fb, rx, row_y, ": ", sep_r, sep_g, sep_b, FONT_SCALE);
            uint8_t cr, cg, cb;
            anime_countdown_color(status_cache.anime_airing_at[i + 1], &cr, &cg, &cb);
            if (stale) { cr = 90; cg = 90; cb = 90; }
            draw_text(fb, rx, row_y, status_cache.anime_countdowns[i + 1], cr, cg, cb, FONT_SCALE);
        }

        row++;
    }

    // Bottom separator
    fill_rect(fb, x, y + h - 1, w, 1, SEP_MAJOR_R, SEP_MAJOR_G, SEP_MAJOR_B);
}

// ============================================================
// Notes module
// ============================================================

// Layout notes at a given scale. If draw_fb is non-NULL, also draws them.
// Returns 1 if all notes fit within the bounding area, 0 if they overflow.
static int layout_notes(Framebuffer *draw_fb, float scale, int ax, int ay, int aw, int ah) {
    int padding = 20;
    int current_x = ax + padding;
    int current_y = ay + padding;
    int row_height = 0;
    int max_notes_per_row = 10;
    int notes_in_current_row = 0;

    for (int n = 0; n < store.num_notes; n++) {
        Note *note = &store.notes[n];
        if (note->is_linebreak) {
            current_x = ax + padding;
            current_y += row_height + padding;
            row_height = 0;
            notes_in_current_row = 0;
            continue;
        }

        int scaled_width = (int)(note->width * scale + 0.5f);
        int scaled_height = (int)(note->height * scale + 0.5f);

        if (current_x + scaled_width + padding > ax + aw ||
            notes_in_current_row >= max_notes_per_row) {
            current_x = ax + padding;
            current_y += row_height + padding;
            row_height = 0;
            notes_in_current_row = 0;
        }

        if (current_y + scaled_height > ay + ah)
            return 0;

        if (draw_fb) {
            for (int s = 0; s < note->num_strokes; s++) {
                Stroke *stroke = &note->strokes[s];
                if (stroke->num_points < 2) continue;

                int x_prev = (int)(stroke->points[0].x * scale + current_x + 0.5f);
                int y_prev = (int)(stroke->points[0].y * scale + current_y + 0.5f);
                int thickness = (int)(8 * scale);
                if (thickness < 1) thickness = 1;

                for (int p = 1; p < stroke->num_points; p++) {
                    int x_curr = (int)(stroke->points[p].x * scale + current_x + 0.5f);
                    int y_curr = (int)(stroke->points[p].y * scale + current_y + 0.5f);
                    draw_line(draw_fb, x_prev, y_prev, x_curr, y_curr, 255, 255, 255, thickness);
                    x_prev = x_curr;
                    y_prev = y_curr;
                }
            }
        }

        current_x += scaled_width + padding;
        if (scaled_height > row_height) row_height = scaled_height;
        notes_in_current_row++;
    }
    return 1;
}

void draw_notes_area(Framebuffer *fb, int x, int y, int w, int h) {
    if (store.num_notes == 0) return;

    // Find the best scale that fits
    float scale = config.note_scale;
    while (scale > MIN_NOTE_SCALE && !layout_notes(NULL, scale, x, y, w, h))
        scale -= 0.05f;
    if (scale < MIN_NOTE_SCALE) scale = MIN_NOTE_SCALE;

    layout_notes(fb, scale, x, y, w, h);
}

// ============================================================
// Layout manager
// ============================================================

// Get the height a module needs (w = anticipated render width)
static int get_module_height(int type, int w) {
    switch (type) {
        case MODULE_CHART:
            if (config.chart_height <= 0 || chart_data.num_sensors == 0) return 0;
            return config.chart_height;
        case MODULE_ANIME:
            return get_anime_height();
        case MODULE_NOTES:
            return MODULE_HEIGHT_FILL;
        case MODULE_RSS:
            return get_rss_height(w);
        case MODULE_FORECAST:
            return get_forecast_height();
        default:
            return 0;
    }
}

// Draw a module into its bounding rectangle
static void draw_module(Framebuffer *fb, int type, int x, int y, int w, int h) {
    switch (type) {
        case MODULE_CHART:
            draw_chart(fb, x, y, w, h);
            break;
        case MODULE_ANIME:
            draw_anime(fb, x, y, w, h);
            break;
        case MODULE_NOTES:
            draw_notes_area(fb, x, y, w, h);
            break;
        case MODULE_RSS:
            draw_rss(fb, x, y, w, h);
            break;
        case MODULE_FORECAST:
            draw_forecast(fb, x, y, w, h);
            break;
    }
}

void render_screen(void) {
    pthread_mutex_lock(&store.lock);
    clear_screen(&fb);

    int display_width, display_height;
    get_display_size(&fb, &display_width, &display_height);

    // Status bar always at top, full width
    draw_status_bar(&fb, display_width);
    int y = STATUS_ROW_HEIGHT;

    // Pre-scan: group half-width modules
    // group_leader[i] = index of the leader module for this group, or -1
    // partners[i][0..3] = indices of stacked partners for leader i
    // partner_count[i] = number of partners for leader i
    int group_leader[MAX_MODULES];
    int partners[MAX_MODULES][4];
    int partner_count[MAX_MODULES];
    for (int i = 0; i < config.num_modules; i++) {
        group_leader[i] = -1;
        partner_count[i] = 0;
    }

    // Collect half-width module indices in config order
    int halves[MAX_MODULES];
    int num_halves = 0;
    for (int i = 0; i < config.num_modules; i++)
        if (config.modules[i].width == MODULE_WIDTH_HALF)
            halves[num_halves++] = i;

    // Pass 1: span >= 2 modules claim nearest unclaimed halves as partners
    for (int h = 0; h < num_halves; h++) {
        int idx = halves[h];
        if (config.modules[idx].span < 2) continue;

        group_leader[idx] = idx;
        int claimed = 0;
        for (int j = 0; j < num_halves && claimed < config.modules[idx].span; j++) {
            int pidx = halves[j];
            if (pidx == idx || group_leader[pidx] >= 0) continue;
            group_leader[pidx] = idx;
            partners[idx][claimed++] = pidx;
        }
        partner_count[idx] = claimed;
    }

    // Pass 2: remaining span-1 modules pair 1:1
    for (int h = 0; h < num_halves; h++) {
        int idx = halves[h];
        if (group_leader[idx] >= 0) continue;

        group_leader[idx] = idx;
        for (int j = h + 1; j < num_halves; j++) {
            int pidx = halves[j];
            if (group_leader[pidx] >= 0) continue;
            group_leader[pidx] = idx;
            partners[idx][0] = pidx;
            partner_count[idx] = 1;
            break;
        }
    }

    // Render: process modules in config order
    int drawn[MAX_MODULES] = {0};

    for (int i = 0; i < config.num_modules; i++) {
        if (drawn[i]) continue;
        ModuleConfig *m = &config.modules[i];

        if (m->width == MODULE_WIDTH_FULL) {
            // Full-width module
            int h = get_module_height(m->type, display_width);
            if (h == MODULE_HEIGHT_FILL) h = display_height - y;
            if (y + h > display_height) h = display_height - y;
            if (h <= 0) continue;
            draw_module(&fb, m->type, 0, y, display_width, h);
            y += h;

        } else if (group_leader[i] >= 0 && group_leader[i] != i) {
            // Claimed partner — will be drawn by its leader, skip
            continue;

        } else if (group_leader[i] == i && partner_count[i] > 0) {
            // Leader of a group (span or 1:1 pair)
            int half_w = display_width / 2;
            int pc = partner_count[i];

            // Determine sides: if leader comes after first partner in config,
            // leader goes right (partners stack left). Otherwise leader left.
            int leader_right = (partners[i][0] < i);
            int span_x = leader_right ? half_w : 0;
            int stack_x = leader_right ? 0 : half_w;
            int span_w = leader_right ? (display_width - half_w) : half_w;
            int stack_w = leader_right ? half_w : (display_width - half_w);

            // Calculate stacked-side heights
            int span_h_raw = get_module_height(m->type, span_w);
            int sh_raw[4], sh[4];
            int stack_fixed_total = 0;
            int stack_fill_count = 0;
            for (int s = 0; s < pc; s++) {
                sh_raw[s] = get_module_height(config.modules[partners[i][s]].type, stack_w);
                if (sh_raw[s] == MODULE_HEIGHT_FILL)
                    stack_fill_count++;
                else
                    stack_fixed_total += sh_raw[s];
            }

            // Total group height: fixed side wins over fill
            int total_h;
            if (span_h_raw != MODULE_HEIGHT_FILL)
                total_h = span_h_raw;
            else if (stack_fixed_total > 0)
                total_h = stack_fixed_total;
            else
                total_h = display_height - y;

            if (y + total_h > display_height) total_h = display_height - y;
            if (total_h <= 0) {
                for (int s = 0; s < pc; s++) drawn[partners[i][s]] = 1;
                continue;
            }

            // Resolve individual stacked heights
            int remaining = total_h;
            for (int s = 0; s < pc; s++) {
                if (sh_raw[s] != MODULE_HEIGHT_FILL) {
                    sh[s] = sh_raw[s];
                    remaining -= sh[s];
                }
            }
            for (int s = 0; s < pc; s++) {
                if (sh_raw[s] == MODULE_HEIGHT_FILL) {
                    sh[s] = stack_fill_count > 0 ? remaining / stack_fill_count : remaining;
                    remaining -= sh[s];
                    stack_fill_count--;
                }
            }

            // Draw spanning module (full group height)
            draw_module(&fb, m->type, span_x, y, span_w, total_h);

            // Draw stacked partners
            int sy = y;
            for (int s = 0; s < pc; s++) {
                if (sh[s] > 0)
                    draw_module(&fb, config.modules[partners[i][s]].type,
                                stack_x, sy, stack_w, sh[s]);
                drawn[partners[i][s]] = 1;
                sy += sh[s];
            }
            y += total_h;

        } else {
            // Unpaired half-width: render on left side only
            int h = get_module_height(m->type, display_width / 2);
            if (h == MODULE_HEIGHT_FILL) h = display_height - y;
            if (y + h > display_height) h = display_height - y;
            if (h <= 0) continue;
            draw_module(&fb, m->type, 0, y, display_width / 2, h);
            y += h;
        }
    }

    fb_flip(&fb);
    sprite_redraw_after_flip();
    first_render_done = 1;
    pthread_mutex_unlock(&store.lock);
}

// ============================================================
// Status bar refresh thread
// ============================================================

static int network_ready(void) {
    char buf[64];
    // Quick DNS check — if this resolves, the network is up
    return run_cmd("getent hosts api.open-meteo.com 2>/dev/null", buf, sizeof(buf));
}

static void *status_refresh_thread(void *arg) {
    (void)arg;
    // On boot, the service may start before the network is up.
    // Wait here so the first refresh doesn't fire into a dead network
    // and then lock out retries for the full refresh_interval.
    while (running && !network_ready()) sleep(5);
    while (running) {
        time_t now = time(NULL);
        if (now - status_cache.last_fetched >= config.refresh_interval) {
            refresh_status_cache();
        }
        render_screen();
        sleep(30);
        if (!running) break;
    }
    return NULL;
}

// ============================================================
// JSON note parser
// ============================================================

static int parse_json_note(const char *json, Note *note) {
    memset(note, 0, sizeof(Note));

    // Parse width and height
    const char *w = strstr(json, "\"width\":");
    const char *h = strstr(json, "\"height\":");
    if (w) sscanf(w + strlen("\"width\":"), "%f", &note->width);
    if (h) sscanf(h + strlen("\"height\":"), "%f", &note->height);

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
                // Find this point object's closing brace
                const char *pt_end = strchr(p, '}');
                if (!pt_end) break;

                float x = 0, y = 0;

                const char *x_pos = strstr(p, "\"x\":");
                if (x_pos && x_pos < pt_end) {
                    const char *x_val = x_pos + 4;
                    while (*x_val && (*x_val == ' ' || *x_val == '\t')) x_val++;
                    sscanf(x_val, "%f", &x);
                }

                const char *y_pos = strstr(p, "\"y\":");
                if (y_pos && y_pos < pt_end) {
                    const char *y_val = y_pos + 4;
                    while (*y_val && (*y_val == ' ' || *y_val == '\t')) y_val++;
                    sscanf(y_val, "%f", &y);
                }

                stroke->points[stroke->num_points].x = x;
                stroke->points[stroke->num_points].y = y;
                stroke->num_points++;

                p = pt_end;
            }
            p++;
        }

        stroke_idx++;
        // Advance past the stroke object's closing brace (p is past the points array)
        ptr = strchr(p, '}');
        if (!ptr) break;
        ptr++;
    }

    note->num_strokes = stroke_idx;
    return (note->num_strokes > 0 || note->is_linebreak) ? 0 : -1;
}

// ============================================================
// HTTP server
// ============================================================

static int read_full_request(int client_socket, char *buffer, int buffer_size) {
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
                sscanf(cl_header + strlen("Content-Length: "), "%d", &content_length);
                if (content_length > buffer_size - 1) content_length = buffer_size - 1;
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

static void send_response(int sock, const char *resp) {
    write(sock, resp, strlen(resp));
}

static const char *RESP_JSON_OK =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "\r\n"
    "{\"status\":\"success\"}";

static void handle_request(int client_socket) {
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

                render_screen();
                send_response(client_socket, RESP_JSON_OK);
            } else {
                send_response(client_socket, "HTTP/1.1 400 Bad Request\r\n\r\n");
            }
        } else {
            send_response(client_socket, "HTTP/1.1 507 Insufficient Storage\r\n\r\n");
        }
    }
    // Handle /clear_notes
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/clear_notes") == 0) {
        pthread_mutex_lock(&store.lock);
        store.num_notes = 0;
        save_notes();
        pthread_mutex_unlock(&store.lock);

        render_screen();
        send_response(client_socket, RESP_JSON_OK);
    }
    // Handle OPTIONS
    else if (strcmp(method, "OPTIONS") == 0) {
        send_response(client_socket,
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "\r\n");
    }
    else {
        send_response(client_socket, "HTTP/1.1 404 Not Found\r\n\r\n");
    }

    close(client_socket);
}

static void *server_thread(void *arg) {
    (void)arg;
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

        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_request(client_socket);
    }

    close(server_socket);
    return NULL;
}

// ============================================================
// Main
// ============================================================

int main(void) {
    // Hide cursor
    printf("\033[?25l");
    fflush(stdout);

    // Initialize
    pthread_mutex_init(&store.lock, NULL);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Load config and saved notes
    load_config(CONFIG_PATH, &config);
    load_notes();

    if (fb_init(&fb, "/dev/fb0") != 0) {
        fprintf(stderr, "Failed to initialize framebuffer\n");
        return 1;
    }

    // Report logical display size (useful for sizing a background image)
    {
        int dw, dh;
        get_display_size(&fb, &dw, &dh);
        fprintf(stderr, "display: logical size %dx%d\n", dw, dh);
    }

    // Optional background image (24-bit BMP, logical display size)
    if (config.background_image[0])
        fb_load_background(&fb, config.background_image);

    // Initial render
    render_screen();

    // Start server
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, server_thread, NULL);

    // Start status bar refresh thread
    pthread_t status_tid;
    pthread_create(&status_tid, NULL, status_refresh_thread, NULL);

    // Start sprite animation thread (only if enabled)
    pthread_t sprite_tid = 0;
    if (config.sprite_enabled)
        pthread_create(&sprite_tid, NULL, sprite_thread, NULL);

    // Wait
    pthread_join(server_tid, NULL);
    pthread_join(status_tid, NULL);
    if (sprite_tid) pthread_join(sprite_tid, NULL);

    // Cleanup
    printf("\033[?25h");
    fflush(stdout);
    munmap(fb.fbp, fb.screensize);
    free(fb.backbuf);
    close(fb.fd);
    pthread_mutex_destroy(&store.lock);

    return 0;
}
