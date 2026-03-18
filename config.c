/*
 * config.c - JSON config file parsing
 */
#include "pinote.h"

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

// Helper: extract a JSON integer value by key
static int json_get_int(const char *json, const char *key, int *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    return sscanf(pos, "%d", out) == 1;
}

// Helper: extract a JSON float value by key
static int json_get_float(const char *json, const char *key, float *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    return sscanf(pos, "%f", out) == 1;
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
            snprintf(cfg->hue_sensor_ids[0], sizeof(cfg->hue_sensor_ids[0]), "%s", single_id);
            cfg->num_hue_sensors = 1;
        }
    }

    // Parse anime_truncate (max chars, 0 = no truncation; also accepts true as 20)
    if (strstr(buf, "\"anime_truncate\":true"))
        cfg->anime_truncate = 20;
    else
        json_get_int(buf, "anime_truncate", &cfg->anime_truncate);

    // Parse anime_per_line (default: 1)
    cfg->anime_per_line = 1;
    json_get_int(buf, "anime_per_line", &cfg->anime_per_line);

    // Parse note_scale (default: 0.6)
    cfg->note_scale = 0.6f;
    if (json_get_float(buf, "note_scale", &cfg->note_scale)) {
        if (cfg->note_scale < 0.1f) cfg->note_scale = 0.1f;
        if (cfg->note_scale > 1.0f) cfg->note_scale = 1.0f;
    }

    // Parse location (latitude/longitude for Open-Meteo weather)
    if (json_get_float(buf, "latitude", &cfg->latitude) &&
        json_get_float(buf, "longitude", &cfg->longitude)) {
        cfg->has_location = 1;
    }

    // Parse chart data API settings
    json_get_string(buf, "chart_api_url", cfg->chart_api_url, sizeof(cfg->chart_api_url));
    json_get_string(buf, "chart_api_key", cfg->chart_api_key, sizeof(cfg->chart_api_key));
    cfg->chart_height = DEFAULT_CHART_HEIGHT;
    json_get_int(buf, "chart_height", &cfg->chart_height);
    if (cfg->chart_height < 0) cfg->chart_height = 0;

    cfg->refresh_interval = CACHE_TTL;
    json_get_int(buf, "refresh_interval", &cfg->refresh_interval);
    if (cfg->refresh_interval < 300) cfg->refresh_interval = 300; // minimum 5 minutes

    // Parse RSS feed settings
    json_get_string(buf, "rss_url", cfg->rss_url, sizeof(cfg->rss_url));
    cfg->max_rss_items = 6;
    json_get_int(buf, "max_rss_items", &cfg->max_rss_items);
    if (cfg->max_rss_items < 0) cfg->max_rss_items = 0;
    if (cfg->max_rss_items > MAX_RSS_ITEMS) cfg->max_rss_items = MAX_RSS_ITEMS;
    cfg->rss_per_line = 1;
    json_get_int(buf, "rss_per_line", &cfg->rss_per_line);
    if (cfg->rss_per_line < 1) cfg->rss_per_line = 1;
    if (cfg->rss_per_line > 2) cfg->rss_per_line = 2;
    json_get_int(buf, "rss_truncate", &cfg->rss_truncate);

    // Parse sprite toggle (default: off)
    json_get_int(buf, "sprite_enabled", &cfg->sprite_enabled);

    // Parse orientation (1=landscape, 2=portrait, 3=landscape_flip, 4=portrait_flip)
    json_get_int(buf, "orientation", &cfg->orientation);
    if (cfg->orientation < 1 || cfg->orientation > 4) cfg->orientation = 0;

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

    // Parse modules array (default: anime full, chart full, notes full)
    cfg->num_modules = 0;
    const char *mods = strstr(buf, "\"modules\"");
    if (mods) {
        const char *bracket = strchr(mods, '[');
        if (bracket) {
            const char *p = bracket + 1;
            while (*p && *p != ']' && cfg->num_modules < MAX_MODULES) {
                const char *obj = strchr(p, '{');
                if (!obj) break;
                const char *obj_end = strchr(obj, '}');
                if (!obj_end) break;

                ModuleConfig *m = &cfg->modules[cfg->num_modules];
                m->type = -1;
                m->width = MODULE_WIDTH_FULL;

                // Parse "type"
                char type_str[32] = {0};
                const char *tp = strstr(obj, "\"type\"");
                if (tp && tp < obj_end) {
                    tp += 6;
                    while (*tp && (*tp == ' ' || *tp == ':' || *tp == '\t')) tp++;
                    if (*tp == '"') {
                        tp++;
                        int i = 0;
                        while (*tp && *tp != '"' && i < 31) type_str[i++] = *tp++;
                        type_str[i] = '\0';
                    }
                }

                if (strcmp(type_str, "chart") == 0) m->type = MODULE_CHART;
                else if (strcmp(type_str, "anime") == 0) m->type = MODULE_ANIME;
                else if (strcmp(type_str, "notes") == 0) m->type = MODULE_NOTES;
                else if (strcmp(type_str, "rss") == 0) m->type = MODULE_RSS;

                // Parse "width"
                const char *wp = strstr(obj, "\"width\"");
                if (wp && wp < obj_end) {
                    if (strstr(wp, "\"half\"") && strstr(wp, "\"half\"") < obj_end)
                        m->width = MODULE_WIDTH_HALF;
                }

                // Parse "span" (default 1)
                m->span = 1;
                const char *sp = strstr(obj, "\"span\"");
                if (sp && sp < obj_end) {
                    sp += 6;
                    while (*sp && (*sp == ' ' || *sp == ':' || *sp == '\t')) sp++;
                    int sv = 0;
                    if (sscanf(sp, "%d", &sv) == 1 && sv >= 2)
                        m->span = sv;
                }

                if (m->type >= 0)
                    cfg->num_modules++;

                p = obj_end + 1;
            }
        }
    }

    // Default module layout if none specified
    if (cfg->num_modules == 0) {
        cfg->modules[0] = (ModuleConfig){MODULE_ANIME, MODULE_WIDTH_FULL, 1};
        cfg->modules[1] = (ModuleConfig){MODULE_CHART, MODULE_WIDTH_FULL, 1};
        cfg->modules[2] = (ModuleConfig){MODULE_NOTES, MODULE_WIDTH_FULL, 1};
        cfg->num_modules = 3;
    }
}
