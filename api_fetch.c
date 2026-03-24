/*
 * api_fetch.c - All remote API fetchers (Hue, Open-Meteo, AniList, temp history)
 */
#include "pinote.h"

// Check if a string is safe to embed in a single-quoted shell argument.
// Rejects characters that could break out of single quotes or inject commands.
static int is_shell_safe(const char *str) {
    while (*str) {
        if (*str == '\'' || *str == '\\' || *str == '\n' || *str == '\r')
            return 0;
        str++;
    }
    return 1;
}

// Run a shell command and read its stdout into buf. Returns bytes read.
int run_cmd(const char *cmd, char *buf, int bufsize) {
    FILE *fp = popen(cmd, "r");
    if (!fp) { buf[0] = '\0'; return 0; }
    int len = 0;
    while (len < bufsize - 1) {
        int n = fread(buf + len, 1, bufsize - 1 - len, fp);
        if (n <= 0) break;
        len += n;
    }
    pclose(fp);
    buf[len] = '\0';
    return len;
}

// ============================================================
// Philips Hue temperature
// ============================================================

static void get_hue_temperatures(const AppConfig *cfg, StatusCache *cache) {
    cache->num_hue_temps = 0;
    if (!cfg->hue_bridge_ip[0] || !cfg->hue_api_key[0] || cfg->num_hue_sensors == 0)
        return;
    if (!is_shell_safe(cfg->hue_bridge_ip) || !is_shell_safe(cfg->hue_api_key))
        return;

    for (int s = 0; s < cfg->num_hue_sensors && s < 10; s++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "curl -s --connect-timeout 2 'http://%s/api/%s/sensors/%s' 2>/dev/null",
            cfg->hue_bridge_ip, cfg->hue_api_key, cfg->hue_sensor_ids[s]);

        char response[4096];
        if (!run_cmd(cmd, response, sizeof(response))) continue;

        int idx = cache->num_hue_temps;

        // Extract sensor name (e.g. "name":"Hue temperature sensor 1")
        cache->hue_names[idx][0] = '\0';
        const char *name_pos = strstr(response, "\"name\":\"");
        if (name_pos) {
            name_pos += 8;  // skip "name":"
            int i = 0;
            while (*name_pos && *name_pos != '"' && i < 63)
                cache->hue_names[idx][i++] = *name_pos++;
            cache->hue_names[idx][i] = '\0';
        }

        const char *temp = strstr(response, "\"temperature\":");
        if (temp) {
            int raw = 0;
            sscanf(temp + strlen("\"temperature\":"), "%d", &raw);
            snprintf(cache->hue_temps[idx], 32,
                     "%d.%dC", raw / 100, (abs(raw) % 100) / 10);
        } else {
            snprintf(cache->hue_temps[idx], 32, "err");
        }
        cache->num_hue_temps++;
    }
}

// ============================================================
// Open-Meteo weather
// ============================================================

// Helper: walk a JSON array from *pos, extract the idx-th number as float
// Expects *pos to point at or before '['. Returns 0 on failure.
static int json_array_float(const char *arr, int idx, float *out) {
    const char *p = strchr(arr, '[');
    if (!p) return 0;
    p++;
    for (int i = 0; i <= idx; i++) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\n')) p++;
        if (!*p || *p == ']') return 0;
        if (i == idx) return sscanf(p, "%f", out) == 1;
        // skip this value
        if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
        else { while (*p && *p != ',' && *p != ']') p++; }
    }
    return 0;
}

// Helper: walk a JSON array, extract the idx-th string into out
static int json_array_string(const char *arr, int idx, char *out, int out_size) {
    const char *p = strchr(arr, '[');
    if (!p) return 0;
    p++;
    for (int i = 0; i <= idx; i++) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\n')) p++;
        if (!*p || *p == ']') return 0;
        if (*p == '"') {
            p++;
            if (i == idx) {
                int j = 0;
                while (*p && *p != '"' && j < out_size - 1) out[j++] = *p++;
                out[j] = '\0';
                return 1;
            }
            while (*p && *p != '"') p++;
            if (*p) p++;
        } else {
            if (i == idx) return 0; // expected string
            while (*p && *p != ',' && *p != ']') p++;
        }
    }
    return 0;
}

// Helper: walk a JSON array, extract the idx-th integer
static int json_array_int(const char *arr, int idx, int *out) {
    float f;
    if (!json_array_float(arr, idx, &f)) return 0;
    *out = (int)f;
    return 1;
}

static const char *day_names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static void fetch_weather(void) {
    if (!config.has_location) return;

    // Build URL — always fetch current temp, optionally add daily forecast
    char cmd[512];
    if (config.forecast_days > 0) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --connect-timeout 5 'https://api.open-meteo.com/v1/forecast?"
            "latitude=%.4f&longitude=%.4f"
            "&current=temperature_2m"
            "&daily=weather_code,temperature_2m_max,temperature_2m_min,wind_speed_10m_max,uv_index_max,precipitation_sum"
            "&timezone=auto&forecast_days=%d' 2>/dev/null",
            config.latitude, config.longitude, config.forecast_days);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s --connect-timeout 3 'https://api.open-meteo.com/v1/forecast?"
            "latitude=%.4f&longitude=%.4f&current=temperature_2m' 2>/dev/null",
            config.latitude, config.longitude);
    }

    char response[8192];
    if (!run_cmd(cmd, response, sizeof(response))) return;

    // Parse current temperature → status_cache.weather_temp (keep old value on failure)
    const char *current = strstr(response, "\"current\":{");
    if (current) {
        const char *temp = strstr(current, "\"temperature_2m\":");
        if (temp) {
            float val = 0;
            if (sscanf(temp + strlen("\"temperature_2m\":"), "%f", &val) == 1)
                snprintf(status_cache.weather_temp, sizeof(status_cache.weather_temp), "%.1fC", val);
        }
    }

    // Parse daily forecast → forecast_cache
    if (config.forecast_days <= 0) return;

    const char *daily = strstr(response, "\"daily\":{");
    if (!daily) return;

    // Find each array within the daily block
    const char *time_arr = strstr(daily, "\"time\":");
    const char *code_arr = strstr(daily, "\"weather_code\":");
    const char *tmax_arr = strstr(daily, "\"temperature_2m_max\":");
    const char *tmin_arr = strstr(daily, "\"temperature_2m_min\":");
    const char *wind_arr = strstr(daily, "\"wind_speed_10m_max\":");
    const char *uv_arr   = strstr(daily, "\"uv_index_max\":");
    const char *prec_arr = strstr(daily, "\"precipitation_sum\":");

    if (!time_arr || !code_arr || !tmax_arr || !tmin_arr) return;

    int count = 0;
    for (int i = 0; i < config.forecast_days && i < MAX_FORECAST_DAYS; i++) {
        char date_str[16] = {0};
        if (!json_array_string(time_arr, i, date_str, sizeof(date_str))) break;

        ForecastDay *day = &forecast_cache.days[count];
        memset(day, 0, sizeof(ForecastDay));

        // Derive day name from "YYYY-MM-DD"
        int year, month, mday;
        if (sscanf(date_str, "%d-%d-%d", &year, &month, &mday) == 3) {
            struct tm tm = {0};
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = mday;
            mktime(&tm);
            snprintf(day->day_name, sizeof(day->day_name), "%s", day_names[tm.tm_wday]);
        }

        json_array_int(code_arr, i, &day->weather_code);
        json_array_float(tmax_arr, i, &day->temp_max);
        json_array_float(tmin_arr, i, &day->temp_min);
        if (wind_arr) json_array_float(wind_arr, i, &day->wind_max);
        if (uv_arr)   json_array_float(uv_arr, i, &day->uv_max);
        if (prec_arr) json_array_float(prec_arr, i, &day->precip_mm);

        count++;
    }

    forecast_cache.num_days = count;
    forecast_cache.last_fetched = time(NULL);
}

// ============================================================
// Temperature history (chart data from remote API)
// ============================================================

static time_t parse_datetime(const char *str) {
    int year, month, day, hour, min, sec = 0;
    if (sscanf(str, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec) < 5)
        return 0;
    struct tm tm = {0};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

// Sensor color palette (single source of truth for chart, legend, and status bar)
static const uint8_t sensor_colors[][3] = {
    {0, 255, 255},      // cyan
    {255, 206, 86},      // yellow
    {255, 99, 132},      // pinkish red
    {75, 192, 192},      // teal
    {153, 102, 255},     // purple
};

// Find or create a sensor by name, returns index or -1 if full
static int find_or_create_sensor(ChartData *hist, const char *name, int namelen) {
    for (int i = 0; i < hist->num_sensors; i++) {
        if ((int)strlen(hist->sensors[i].name) == namelen &&
            strncmp(hist->sensors[i].name, name, namelen) == 0)
            return i;
    }

    if (hist->num_sensors >= MAX_SENSORS) return -1;
    int idx = hist->num_sensors++;
    SensorData *s = &hist->sensors[idx];
    if (namelen > 63) namelen = 63;
    memcpy(s->name, name, namelen);
    s->name[namelen] = '\0';
    s->num_points = 0;
    return idx;
}

// Parse flat array format:
// [{"time":"...","label":"Outdoor","value":"13.20"}, ...]
static void parse_chart_json(const char *json, ChartData *hist) {
    hist->num_sensors = 0;

    const char *p = strchr(json, '[');
    if (!p) return;
    p++;

    while (*p) {
        // Find next object
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        // Extract label
        char sensor_name[64] = {0};
        const char *sn = strstr(obj, "\"label\"");
        if (sn && sn < obj_end) {
            const char *v = sn + 7;
            while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
            if (*v == '"') {
                v++;
                int i = 0;
                while (*v && *v != '"' && i < 63) sensor_name[i++] = *v++;
                sensor_name[i] = '\0';
            }
        }

        // Extract time
        time_t timestamp = 0;
        const char *tk = strstr(obj, "\"time\"");
        if (tk && tk < obj_end) {
            const char *v = tk + 6;
            while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
            if (*v == '"') {
                v++;
                char tbuf[32];
                int i = 0;
                while (*v && *v != '"' && i < 31) tbuf[i++] = *v++;
                tbuf[i] = '\0';
                timestamp = parse_datetime(tbuf);
            }
        }

        // Extract value (may be string or number)
        float value = 0;
        const char *tc = strstr(obj, "\"value\"");
        if (tc && tc < obj_end) {
            const char *v = tc + 7;
            while (*v && (*v == ' ' || *v == ':' || *v == '\t')) v++;
            if (*v == '"') v++; // skip opening quote if string value
            sscanf(v, "%f", &value);
        }

        // Add to the right sensor
        if (sensor_name[0] && timestamp > 0) {
            int idx = find_or_create_sensor(hist, sensor_name, strlen(sensor_name));
            if (idx >= 0) {
                SensorData *s = &hist->sensors[idx];
                if (s->num_points < MAX_CHART_POINTS) {
                    s->points[s->num_points].timestamp = timestamp;
                    s->points[s->num_points].value = value;
                    s->num_points++;
                }
            }
        }

        p = obj_end + 1;
    }

    // Sort sensors alphabetically by name, then reassign colors
    for (int i = 0; i < hist->num_sensors - 1; i++) {
        for (int j = i + 1; j < hist->num_sensors; j++) {
            if (strcmp(hist->sensors[i].name, hist->sensors[j].name) > 0) {
                SensorData tmp = hist->sensors[i];
                hist->sensors[i] = hist->sensors[j];
                hist->sensors[j] = tmp;
            }
        }
    }
    for (int i = 0; i < hist->num_sensors; i++) {
        hist->sensors[i].r = sensor_colors[i % 5][0];
        hist->sensors[i].g = sensor_colors[i % 5][1];
        hist->sensors[i].b = sensor_colors[i % 5][2];
    }

    // Sort each sensor's points by timestamp (API returns newest-first)
    for (int s = 0; s < hist->num_sensors; s++) {
        SensorData *sd = &hist->sensors[s];
        // Simple insertion sort (data is mostly reverse-sorted)
        for (int i = 1; i < sd->num_points; i++) {
            ChartPoint tmp = sd->points[i];
            int j = i - 1;
            while (j >= 0 && sd->points[j].timestamp > tmp.timestamp) {
                sd->points[j + 1] = sd->points[j];
                j--;
            }
            sd->points[j + 1] = tmp;
        }
    }

    hist->last_fetched = time(NULL);
}

void fetch_chart_data(void) {
    if (!config.chart_api_url[0] || !config.chart_api_key[0] || config.chart_height <= 0) return;
    if (!is_shell_safe(config.chart_api_url) || !is_shell_safe(config.chart_api_key)) return;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "curl -s --connect-timeout 10 -H 'X-API-Key: %s' '%s' 2>/dev/null",
        config.chart_api_key, config.chart_api_url);

    static char response[131072]; // 128KB for a week of data
    if (!run_cmd(cmd, response, sizeof(response))) return;

    parse_chart_json(response, &chart_data);
}

// ============================================================
// AniList anime countdowns
// ============================================================

typedef struct {
    char title[128];
    char countdown[64];
    long airing_at;  // unix timestamp, 0 = TBA
    int finished;    // 1 if status is FINISHED
} AnimeInfo;

// Bounded strstr: search for needle in [haystack, end) only
static const char *strstr_bounded(const char *haystack, const char *end, const char *needle) {
    size_t nlen = strlen(needle);
    const char *p = strstr(haystack, needle);
    if (!p || p + nlen > end) return NULL;
    return p;
}

static void parse_one_anime(const char **pos, const char *end, AnimeInfo *info) {
    memset(info, 0, sizeof(AnimeInfo));
    const char *p = *pos;

    // Parse status
    const char *st = strstr_bounded(p, end, "\"status\":\"");
    if (st) {
        st += 10;
        if (strncmp(st, "FINISHED", 8) == 0) info->finished = 1;
    }

    // Parse title (romaji) - handle JSON escaped quotes (\")
    const char *romaji = strstr_bounded(p, end, "\"romaji\":\"");
    if (romaji) {
        romaji += 10;
        int i = 0;
        while (romaji < end && *romaji && i < (int)sizeof(info->title) - 1) {
            if (*romaji == '\\' && *(romaji + 1) == '"') {
                romaji++;
                info->title[i++] = *romaji++;
                continue;
            }
            if (*romaji == '"') break;
            info->title[i++] = *romaji++;
        }
        info->title[i] = '\0';
    }

    // Parse airingAt
    const char *airing = strstr_bounded(p, end, "\"airingAt\":");
    if (airing) {
        sscanf(airing + strlen("\"airingAt\":"), "%ld", &info->airing_at);

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

        const char *ep = strstr_bounded(p, end, "\"episode\":");
        if (ep) {
            int ep_num = 0;
            sscanf(ep + strlen("\"episode\":"), "%d", &ep_num);
            char ep_str[32];
            snprintf(ep_str, sizeof(ep_str), " Ep%d", ep_num);
            strncat(info->countdown, ep_str, sizeof(info->countdown) - strlen(info->countdown) - 1);
        }
    } else {
        snprintf(info->countdown, sizeof(info->countdown), "TBA");
    }
}

static void fetch_all_anime(const int *media_ids, int count, AnimeInfo *out) {
    if (count <= 0) return;

    char query[4096];
    int pos = 0;
    pos += snprintf(query + pos, sizeof(query) - pos, "{");
    for (int i = 0; i < count && pos < (int)sizeof(query) - 200; i++) {
        pos += snprintf(query + pos, sizeof(query) - pos,
            "a%d:Media(id:%d){status title{romaji}nextAiringEpisode{airingAt episode}}",
            i, media_ids[i]);
    }
    pos += snprintf(query + pos, sizeof(query) - pos, "}");

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "curl -s --connect-timeout 10 -X POST 'https://graphql.anilist.co' "
        "-H 'Content-Type: application/json' "
        "-d '{\"query\":\"%s\"}' 2>/dev/null", query);

    char response[32768];
    if (!run_cmd(cmd, response, sizeof(response))) {
        for (int i = 0; i < count; i++) {
            memset(&out[i], 0, sizeof(AnimeInfo));
            snprintf(out[i].countdown, sizeof(out[i].countdown), "TBA");
        }
        return;
    }

    const char *resp_end = response + strlen(response);
    for (int i = 0; i < count; i++) {
        char alias[16];
        snprintf(alias, sizeof(alias), "\"a%d\":", i);
        const char *entry = strstr(response, alias);
        if (entry) {
            // Find boundary: next alias or end of response
            const char *boundary = resp_end;
            for (int j = i + 1; j < count; j++) {
                char next_alias[16];
                snprintf(next_alias, sizeof(next_alias), "\"a%d\":", j);
                const char *next = strstr(entry + 1, next_alias);
                if (next) { boundary = next; break; }
            }
            parse_one_anime(&entry, boundary, &out[i]);
        } else {
            memset(&out[i], 0, sizeof(AnimeInfo));
            snprintf(out[i].countdown, sizeof(out[i].countdown), "TBA");
        }
    }
}

static int anime_sort_cmp(const void *a, const void *b) {
    const AnimeInfo *aa = (const AnimeInfo *)a;
    const AnimeInfo *bb = (const AnimeInfo *)b;
    if (aa->airing_at == 0 && bb->airing_at == 0) return 0;
    if (aa->airing_at == 0) return 1;
    if (bb->airing_at == 0) return -1;
    if (aa->airing_at < bb->airing_at) return -1;
    if (aa->airing_at > bb->airing_at) return 1;
    return 0;
}

// ============================================================
// RSS feed
// ============================================================

// Extract text between <tag> and </tag> within bounds
static int xml_get_text(const char *xml, const char *end, const char *tag, char *out, int out_size) {
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start || start >= end) return 0;
    start += strlen(open_tag);

    // Skip CDATA wrapper if present
    if (strncmp(start, "<![CDATA[", 9) == 0) start += 9;

    const char *stop = strstr(start, close_tag);
    if (!stop || stop > end) return 0;

    // Trim CDATA closing if present
    const char *cdata_end = stop;
    if (cdata_end >= start + 3 && strncmp(cdata_end - 3, "]]>", 3) == 0)
        cdata_end -= 3;

    int len = cdata_end - start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return len;
}

static const char *month_names[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

// Parse RFC 822 date "Thu, 13 Mar 2026 03:00:24 +0000" into "Mar 13"
static void format_rss_date(const char *pubdate, char *out, int out_size) {
    out[0] = '\0';
    int day = 0;
    char mon_str[16] = {0};

    // Try RFC 822: "Day, DD Mon YYYY ..."
    if (sscanf(pubdate, "%*[^,], %d %15s", &day, mon_str) == 2) {
        snprintf(out, out_size, "%s %d", mon_str, day);
        return;
    }

    // Try ISO: "YYYY-MM-DD ..."
    int year, month;
    if (sscanf(pubdate, "%d-%d-%d", &year, &month, &day) == 3 && month >= 1 && month <= 12) {
        snprintf(out, out_size, "%s %d", month_names[month - 1], day);
        return;
    }

    // Fallback: just copy first 6 chars
    snprintf(out, out_size, "%.6s", pubdate);
}

// Decode basic HTML entities in-place
static void decode_html_entities(const char *src, char *dst, int dst_size) {
    int i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '&') {
            if (strncmp(src, "&amp;", 5) == 0)  { dst[i++] = '&'; src += 5; }
            else if (strncmp(src, "&lt;", 4) == 0)   { dst[i++] = '<'; src += 4; }
            else if (strncmp(src, "&gt;", 4) == 0)   { dst[i++] = '>'; src += 4; }
            else if (strncmp(src, "&quot;", 6) == 0) { dst[i++] = '"'; src += 6; }
            else if (strncmp(src, "&#39;", 5) == 0)  { dst[i++] = '\''; src += 5; }
            else if (strncmp(src, "&apos;", 6) == 0) { dst[i++] = '\''; src += 6; }
            else { dst[i++] = *src++; }
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

// Parse RSS XML response into tmp cache. Returns number of items parsed.
static int parse_rss_xml(const char *response, RssCache *tmp) {
    const char *p = response;

    while (tmp->num_items < MAX_RSS_ITEMS) {
        const char *item_start = strstr(p, "<item>");
        if (!item_start) break;
        const char *item_end = strstr(item_start, "</item>");
        if (!item_end) break;

        RssItem *item = &tmp->items[tmp->num_items];

        char title_raw[256] = {0};
        if (xml_get_text(item_start, item_end, "title", title_raw, sizeof(title_raw)))
            decode_html_entities(title_raw, item->title, sizeof(item->title));
        strip_utf8_accents(item->title);

        char pubdate[64] = {0};
        if (xml_get_text(item_start, item_end, "pubDate", pubdate, sizeof(pubdate)))
            format_rss_date(pubdate, item->date_str, sizeof(item->date_str));

        if (item->title[0])
            tmp->num_items++;

        p = item_end + 7;
    }
    return tmp->num_items;
}

// Extract a JSON string value by key (simple, for rss2json responses)
static int rss_json_get_string(const char *json, const char *end, const char *key,
                               char *out, int out_size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos || pos >= end) return 0;
    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return 0;
    pos++;
    int i = 0;
    while (*pos && *pos != '"' && i < out_size - 1 && pos < end) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;  // skip backslash
            if (*pos == 'u' && pos + 4 < end) {
                // \uXXXX — parse hex codepoint, map common Unicode to ASCII
                unsigned int cp = 0;
                if (sscanf(pos + 1, "%4x", &cp) == 1) {
                    pos += 5;  // skip uXXXX
                    // Latin accented chars (U+00C0-U+00FF) → ASCII base
                    if (cp >= 0x00C0 && cp <= 0x00FF) {
                        out[i++] = latin_accent_map[cp - 0x00C0];
                    } else switch (cp) {
                        case 0x201C: case 0x201D: out[i++] = '"'; break;   // curly double quotes
                        case 0x2018: case 0x2019: out[i++] = '\''; break;  // curly single quotes
                        case 0x2013: case 0x2014: out[i++] = '-'; break;   // en/em dash
                        case 0x2026: // ellipsis
                            if (i + 2 < out_size - 1) { out[i++]='.'; out[i++]='.'; out[i++]='.'; }
                            break;
                        case 0x00A0: out[i++] = ' '; break;  // non-breaking space
                        default: out[i++] = '?'; break;  // unknown Unicode
                    }
                    continue;
                }
            }
            // Other escapes: \n, \t, \\, etc.
            if (*pos == 'n') out[i++] = ' ';
            else if (*pos == 't') out[i++] = ' ';
            else out[i++] = *pos;
            pos++;
        } else {
            out[i++] = *pos++;
        }
    }
    out[i] = '\0';
    return i;
}

// Parse rss2json JSON response into tmp cache. Returns number of items parsed.
static int parse_rss_json(const char *response, RssCache *tmp) {
    const char *items = strstr(response, "\"items\"");
    if (!items) return 0;
    const char *arr = strchr(items, '[');
    if (!arr) return 0;

    const char *p = arr + 1;
    while (tmp->num_items < MAX_RSS_ITEMS) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        RssItem *item = &tmp->items[tmp->num_items];

        char title_raw[256] = {0};
        rss_json_get_string(obj, obj_end, "title", title_raw, sizeof(title_raw));
        decode_html_entities(title_raw, item->title, sizeof(item->title));
        strip_utf8_accents(item->title);

        char pubdate[64] = {0};
        if (rss_json_get_string(obj, obj_end, "pubDate", pubdate, sizeof(pubdate)))
            format_rss_date(pubdate, item->date_str, sizeof(item->date_str));

        if (item->title[0])
            tmp->num_items++;

        p = obj_end + 1;
    }
    return tmp->num_items;
}

// Minimal URL encoding for rss2json query parameter
static void url_encode(const char *src, char *dst, int dst_size) {
    int i = 0;
    while (*src && i < dst_size - 4) {
        if (*src == ':')       { dst[i++]='%'; dst[i++]='3'; dst[i++]='A'; }
        else if (*src == '/')  { dst[i++]='%'; dst[i++]='2'; dst[i++]='F'; }
        else if (*src == '?')  { dst[i++]='%'; dst[i++]='3'; dst[i++]='F'; }
        else if (*src == '&')  { dst[i++]='%'; dst[i++]='2'; dst[i++]='6'; }
        else if (*src == '=')  { dst[i++]='%'; dst[i++]='3'; dst[i++]='D'; }
        else if (*src == ' ')  { dst[i++]='%'; dst[i++]='2'; dst[i++]='0'; }
        else { dst[i++] = *src; }
        src++;
    }
    dst[i] = '\0';
}

void fetch_rss_feed(void) {
    if (!config.rss_url[0]) return;
    if (!is_shell_safe(config.rss_url)) return;

    static char response[65536];  // 64KB
    RssCache tmp = {0};
    char cmd[1024];
    int method = rss_cache.fetch_method;

    // Try direct XML (skip if we already know it needs JSON)
    if (method != RSS_METHOD_JSON) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --connect-timeout 10 -A 'Mozilla/5.0' '%s' 2>/dev/null",
            config.rss_url);

        if (run_cmd(cmd, response, sizeof(response)) && strstr(response, "<item>")) {
            parse_rss_xml(response, &tmp);
            if (tmp.num_items > 0)
                method = RSS_METHOD_XML;
        }
    }

    // rss2json fallback (skip if we already know direct XML works)
    if (tmp.num_items == 0 && method != RSS_METHOD_XML) {
        char encoded_url[512];
        url_encode(config.rss_url, encoded_url, sizeof(encoded_url));
        snprintf(cmd, sizeof(cmd),
            "curl -s --connect-timeout 10 "
            "'https://api.rss2json.com/v1/api.json?rss_url=%s' 2>/dev/null",
            encoded_url);

        if (run_cmd(cmd, response, sizeof(response)))
            parse_rss_json(response, &tmp);
        if (tmp.num_items > 0)
            method = RSS_METHOD_JSON;
    }

    // Update cache if we got results
    if (tmp.num_items > 0) {
        tmp.fetch_method = method;
        tmp.last_fetched = time(NULL);
        rss_cache = tmp;
    }
}

// ============================================================
// Webhook notification
// ============================================================

static void send_webhook(const char *url, const char *message) {
    if (!url[0] || !is_shell_safe(url)) return;

    // Escape double quotes in message for JSON
    char safe_msg[512];
    int j = 0;
    for (int i = 0; message[i] && j < (int)sizeof(safe_msg) - 2; i++) {
        if (message[i] == '"') {
            safe_msg[j++] = '\\';
            safe_msg[j++] = '"';
        } else {
            safe_msg[j++] = message[i];
        }
    }
    safe_msg[j] = '\0';

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "curl -s -X POST '%s' -H 'Content-Type: application/json' "
        "-d '{\"content\":\"%s\"}' 2>/dev/null",
        url, safe_msg);

    char resp[256];
    run_cmd(cmd, resp, sizeof(resp));
}

// ============================================================
// Refresh all cached status data
// ============================================================

void refresh_status_cache(void) {
    // Hue temperatures (stored individually for per-sensor coloring)
    get_hue_temperatures(&config, &status_cache);
    if (!running) return;

    // Weather: current temperature + daily forecast (Open-Meteo)
    fetch_weather();
    if (!running) return;

    // Fetch all anime in one API call
    AnimeInfo anime[MAX_ANIME];
    int anime_count = config.num_anime;
    fetch_all_anime(config.anilist_media_ids, anime_count, anime);

    // Notify via webhook if any anime just aired since last refresh
    // Must happen BEFORE qsort — indices match config order (stable between refreshes)
    static long prev_airing_at[MAX_ANIME] = {0};
    static char prev_countdown[MAX_ANIME][64] = {{0}};
    static int prev_initialized = 0;

    if (config.webhook_url[0] && prev_initialized) {
        time_t now = time(NULL);
        for (int i = 0; i < anime_count; i++) {
            if (prev_airing_at[i] <= 0) continue;
            // Case 1: we passed the old airing time AND API advanced to next episode
            // Case 2: we passed the old airing time AND no next episode (final)
            int aired = ((long)now >= prev_airing_at[i] && anime[i].airing_at > prev_airing_at[i])
                     || ((long)now >= prev_airing_at[i] && anime[i].airing_at <= 0);
            if (aired) {
                char title[128];
                strncpy(title, anime[i].title, sizeof(title) - 1);
                title[sizeof(title) - 1] = '\0';
                truncate_with_dots(title, 60);
                char msg[256];
                // Use prev_countdown for episode number — current one already advanced
                const char *ep = strstr(prev_countdown[i], "Ep");
                if (anime[i].airing_at <= 0)
                    snprintf(msg, sizeof(msg), ep ? "%s %s just aired! (final)" : "%s just aired! (final)",
                             title, ep);
                else
                    snprintf(msg, sizeof(msg), ep ? "%s %s just aired!" : "%s just aired!",
                             title, ep);
                send_webhook(config.webhook_url, msg);
            }
        }
    }

    // Save current state for next comparison
    for (int i = 0; i < anime_count; i++) {
        prev_airing_at[i] = anime[i].airing_at;
        strncpy(prev_countdown[i], anime[i].countdown, sizeof(prev_countdown[i]) - 1);
        prev_countdown[i][sizeof(prev_countdown[i]) - 1] = '\0';
    }
    prev_initialized = 1;

    // Sort by airing time (soonest first, TBA last)
    qsort(anime, anime_count, sizeof(AnimeInfo), anime_sort_cmp);

    // Build individual anime title + countdown strings (separate for dual-color rendering)
    status_cache.num_anime_entries = 0;
    for (int i = 0; i < anime_count && i < MAX_ANIME; i++) {
        // Skip finished anime or ones with no upcoming episode
        if (anime[i].finished || anime[i].airing_at == 0) continue;

        char *title = anime[i].title;

        // Clean up title: strip accents, truncate if configured
        strip_utf8_accents(title);
        if (config.anime_truncate > 0)
            truncate_with_dots(title, config.anime_truncate);

        int idx = status_cache.num_anime_entries;
        snprintf(status_cache.anime_titles[idx], 128, "%s", title[0] ? title : "?");
        snprintf(status_cache.anime_countdowns[idx], 64, "%s", anime[i].countdown);
        status_cache.anime_airing_at[idx] = anime[i].airing_at;
        status_cache.num_anime_entries++;
    }

    if (!running) return;

    // Fetch chart data
    fetch_chart_data();
    if (!running) return;

    // Fetch RSS feed
    fetch_rss_feed();

    status_cache.last_fetched = time(NULL);
}
