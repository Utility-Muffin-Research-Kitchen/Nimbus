/*
 * Nimbus — weather app for Leaf (Miniloong Pocket 1)
 * Catastrophe UI + libcurl + cJSON, weather from Open-Meteo (no API key).
 * Port of the NextUI Nimbus; see PLAN.md.
 */

#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#define CJSON_HIDE_SYMBOLS
#include "cJSON.h"
#include "cJSON.c"

#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define NIMBUS_VERSION   "0.1.0"
#define MAX_PATH_LEN     1280
#define MAX_LOCATION     256
#define MAX_LINE         512
#define MAX_URL          1024
#define MAX_LABEL        256
#define MAX_FORECAST_DAYS 3
#define MAX_HOURS_PER_DAY 24
#define MAX_HOURS        72
#define MAX_LOCATIONS    10

#define DEFAULT_BG_R     30
#define DEFAULT_BG_G     30
#define DEFAULT_BG_B     35
#define DEFAULT_TEXT_R   220
#define DEFAULT_TEXT_G   220
#define DEFAULT_TEXT_B   220
#define DEFAULT_HINT_R   140
#define DEFAULT_HINT_G   140
#define DEFAULT_HINT_B   150

#define SCROLL_STEP      40

#define TAB_CURRENT  0
#define TAB_FORECAST 1
#define TAB_HOURLY   2
#define TAB_ASTRO    3
#define TAB_COUNT    4

/* -----------------------------------------------------------------------
 * Data structures
 * ----------------------------------------------------------------------- */

typedef struct {
    char name[MAX_LOCATION];
    char lat_lon[64];
    int  id;
    int  is_home;
} location_t;

typedef struct {
    char   time[32];        /* "2024-01-15 14:00" */
    char   hour_label[8];   /* "2 PM" */
    double temp_f;
    double temp_c;
    double feels_like_f;
    double feels_like_c;
    int    humidity;
    double wind_mph;
    double wind_kph;
    char   wind_dir[16];
    int    chance_rain;
    int    chance_snow;
    double precip_in;
    double precip_mm;
    int    cloud;
    double uv;
    int    is_day;
    char   condition_text[MAX_LABEL];
    int    condition_code;
    char   icon_url[MAX_URL];
    SDL_Texture *icon_texture;
} hourly_t;

typedef struct {
    char   date[32];
    char   day_name[16];
    double max_temp_f;
    double max_temp_c;
    double min_temp_f;
    double min_temp_c;
    double feels_max_f;
    double feels_max_c;
    double feels_min_f;
    double feels_min_c;
    double max_wind_mph;
    double max_wind_kph;
    int    chance_rain;
    int    chance_snow;
    double total_precip_in;
    double total_precip_mm;
    int    avg_humidity;
    double uv;
    char   condition_text[MAX_LABEL];
    int    condition_code;
    char   icon_url[MAX_URL];

    char   sunrise[16];
    char   sunset[16];
    char   moonrise[16];
    char   moonset[16];
    char   moon_phase[32];
    int    moon_illumination;

    SDL_Texture *icon_texture;
} forecast_day_t;

typedef struct {
    char location_name[MAX_LOCATION];
    char region[MAX_LOCATION];
    char country[MAX_LOCATION];

    double loc_lat;
    double loc_lon;
    double temp_f;
    double temp_c;
    double feels_like_f;
    double feels_like_c;
    int    humidity;
    double wind_mph;
    double wind_kph;
    char   wind_dir[16];
    char   condition_text[MAX_LABEL];
    int    condition_code;
    int    is_day;
    char   icon_url[MAX_URL];
    double precip_in;
    double precip_mm;
    int    cloud;
    double uv;
    double wind_gust_mph;
    double wind_gust_kph;
    double pressure_hpa;
    double dew_point_f;
    double dew_point_c;
    int    aqi;             /* US AQI, -1 if unavailable */
    char   last_updated[64];

    forecast_day_t forecast[MAX_FORECAST_DAYS];
    int            forecast_count;

    hourly_t       hours[MAX_HOURS];
    int            hour_count;

    int    valid;
    int    is_cached;
    char   cached_time[32];
    SDL_Texture *icon_texture;
} weather_data_t;

typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} fetch_buf_t;

typedef struct {
    int use_fahrenheit;
    int use_24h;          /* 1 = 24-hour clock, 0 = 12-hour (no leading zero) */
} app_settings_t;

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */

static location_t      g_locations[MAX_LOCATIONS];
static int             g_location_count = 0;
static int             g_current_location = 0;
static weather_data_t  g_weather_cache[MAX_LOCATIONS];

static char            g_config_dir[MAX_PATH_LEN] = {0};
static char            g_cache_dir[MAX_PATH_LEN]  = {0};
static app_settings_t  g_settings;
static SDL_Texture    *g_sunrise_icon             = NULL;
static SDL_Texture    *g_sunset_icon              = NULL;

/* Background weather fetch: a worker thread fills g_weather_cache so the UI never
   blocks on the network. g_data_lock guards g_weather_cache + g_locations writes;
   g_fetch_active marks a fetch in flight (shown as an "Updating" indicator). */
static pthread_mutex_t g_data_lock   = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_fetch_active = 0;

/* Whether to draw footer button hints — mirrors Leaf's global "Show Hints"
   setting (CAT_SHOW_HINTS, exported by jawakad). Read once at startup. */
static int g_show_hints = 1;
#define NB_HINTS(n) (g_show_hints ? (n) : 0)

/* -----------------------------------------------------------------------
 * String helpers
 * ----------------------------------------------------------------------- */

static void trim_inplace(char *s) {
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static const char *day_name_from_date(const char *date_str) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3) return "???";
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = y - 1900;
    tm_val.tm_mon  = m - 1;
    tm_val.tm_mday = d;
    mktime(&tm_val);
    static const char *names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    return names[tm_val.tm_wday % 7];
}

/* Format hour:minute per the 12/24h setting. 12-hour drops the leading zero
   ("6:27 AM"); 24-hour is zero-padded ("06:27"). */
static void format_clock(int hour, int min, char *out, size_t n) {
    if (g_settings.use_24h) {
        snprintf(out, n, "%02d:%02d", hour, min);
    } else {
        const char *ap = (hour >= 12) ? "PM" : "AM";
        int h12 = hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(out, n, "%d:%02d %s", h12, min, ap);
    }
}

/* "HH:MM" (24-hour) -> a clock string in the current format. */
static void format_hhmm(const char *hhmm, char *out, size_t n) {
    int h = 0, m = 0;
    if (sscanf(hhmm, "%d:%d", &h, &m) == 2) format_clock(h, m, out, n);
    else snprintf(out, n, "%s", hhmm);
}

/* Compact on-the-hour label for the Hourly list: "2 PM" or "14:00". */
static void format_hour_label(const char *time_str, char *out, size_t out_size) {
    /* Input: "2024-01-15 14:00" */
    out[0] = '\0';
    const char *space = strchr(time_str, ' ');
    if (!space) return;
    int hour = 0;
    sscanf(space + 1, "%d", &hour);
    if (g_settings.use_24h) { snprintf(out, out_size, "%02d:00", hour); return; }
    if (hour == 0) snprintf(out, out_size, "12 AM");
    else if (hour < 12) snprintf(out, out_size, "%d AM", hour);
    else if (hour == 12) snprintf(out, out_size, "12 PM");
    else snprintf(out, out_size, "%d PM", hour - 12);
}

/* -----------------------------------------------------------------------
 * WiFi check
 * ----------------------------------------------------------------------- */

static int check_wifi(void) {
#ifdef PLATFORM_MAC
    return 1; /* desktop dev build has no wifi-strength backend; assume online */
#else
    return cat__get_wifi_strength() > 0;
#endif
}

/* -----------------------------------------------------------------------
 * Local UI helpers (Catastrophe-native)
 * ----------------------------------------------------------------------- */

/* One-frame "working…" splash drawn before a blocking network call. */
static void nb_loading(const char *message) {
    cat_theme *th = cat_get_theme();
    TTF_Font *f = cat_get_font(CAT_FONT_MEDIUM);
    int sw = cat_get_screen_width(), sh = cat_get_screen_height();
    cat_clear_screen();
    cat_draw_background();
    if (message) {
        int w = cat_measure_text(f, message);
        cat_draw_text(f, message, (sw - w) / 2, sh / 2 - TTF_FontHeight(f) / 2, th->text);
    }
    cat_present();
}

/* Acknowledgement message (single button) via the native confirmation widget. */
static void nb_message(const char *message, const char *button_label) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = button_label ? button_label : "OK", .is_confirm = true },
    };
    cat_message_opts opts = { .message = message, .footer = footer, .footer_count = NB_HINTS(1) };
    cat_confirm_result r = {0};
    cat_confirmation(&opts, &r);
}

/* Yes/No confirmation. Returns 1 on confirm (A), 0 on cancel (B). */
static int nb_confirm(const char *message, const char *confirm_label, const char *cancel_label) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = confirm_label ? confirm_label : "Confirm", .is_confirm = true },
        { .button = CAT_BTN_B, .label = cancel_label ? cancel_label : "Cancel" },
    };
    cat_message_opts opts = { .message = message, .footer = footer, .footer_count = NB_HINTS(2) };
    cat_confirm_result r = {0};
    return (cat_confirmation(&opts, &r) == CAT_OK) ? 1 : 0;
}

/* Custom-content vertical scroll for the weather tabs: a small eased offset.
   The tab bodies are hand-drawn under a clip rect, so they steer a scroll_y
   directly rather than use cat_draw_scroll_view's content callback. */
typedef struct { int scroll_y; int target_scroll_y; int last_max_scroll; } nb_scroll_state;

static void nb_scroll_handle_input(nb_scroll_state *s, int direction, int step) {
    if (!s) return;
    s->target_scroll_y += direction * step;
    if (s->target_scroll_y < 0) s->target_scroll_y = 0;
    if (s->target_scroll_y > s->last_max_scroll) s->target_scroll_y = s->last_max_scroll;
}
static void nb_scroll_animate(nb_scroll_state *s) {
    if (!s) return;
    int d = s->target_scroll_y - s->scroll_y;
    if (d > -2 && d < 2) { s->scroll_y = s->target_scroll_y; return; }
    s->scroll_y += d * 15 / 100;  /* ~0.15 ease toward target */
}
static void nb_scroll_update(nb_scroll_state *s, int content_height, int viewport_height) {
    if (!s) return;
    int maxs = content_height - viewport_height;
    if (maxs < 0) maxs = 0;
    s->last_max_scroll = maxs;
    if (s->target_scroll_y > maxs) s->target_scroll_y = maxs;
    if (s->scroll_y > maxs) s->scroll_y = maxs;
    if (s->scroll_y < 0) s->scroll_y = 0;
}

/* -----------------------------------------------------------------------
 * Offline weather cache (JSON per location)
 * ----------------------------------------------------------------------- */

static void weather_cache_path(int loc_idx, char *out, size_t out_size) {
    if (g_cache_dir[0] == '\0' || loc_idx < 0 || loc_idx >= g_location_count) {
        out[0] = '\0';
        return;
    }
    /* Use a hash of location name + lat_lon for a stable filename */
    location_t *loc = &g_locations[loc_idx];
    unsigned long hash = 5381;
    const char *p = loc->name;
    while (*p) { hash = ((hash << 5) + hash) + (unsigned char)*p; p++; }
    p = loc->lat_lon;
    while (*p) { hash = ((hash << 5) + hash) + (unsigned char)*p; p++; }
    snprintf(out, out_size, "%s/weather_%08lx.json", g_cache_dir, hash);
}

static void save_weather_json(int loc_idx, const char *json_str) {
    char path[MAX_PATH_LEN];
    weather_cache_path(loc_idx, path, sizeof(path));
    if (path[0] == '\0') return;
    FILE *f = fopen(path, "w");
    if (!f) { cat_log("weather_cache: could not write %s", path); return; }
    fprintf(f, "%s", json_str);
    fclose(f);
    cat_log("weather_cache: saved for location %d (%s)", loc_idx, g_locations[loc_idx].name);
}

static char *load_weather_json(int loc_idx) {
    char path[MAX_PATH_LEN];
    weather_cache_path(loc_idx, path, sizeof(path));
    if (path[0] == '\0') return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return NULL; }
    char *data = malloc((size_t)fsize + 1);
    if (!data) { fclose(f); return NULL; }
    size_t read_bytes = fread(data, 1, (size_t)fsize, f);
    fclose(f);
    data[read_bytes] = '\0';
    cat_log("weather_cache: loaded %zu bytes for location %d", read_bytes, loc_idx);
    return data;
}

static void get_weather_cache_time(int loc_idx, char *out, size_t out_size) {
    out[0] = '\0';
    char path[MAX_PATH_LEN];
    weather_cache_path(loc_idx, path, sizeof(path));
    if (path[0] == '\0') return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    struct tm *tm_info = localtime(&st.st_mtime);
    if (!tm_info) return;
    format_clock(tm_info->tm_hour, tm_info->tm_min, out, out_size);
}

/* -----------------------------------------------------------------------
 * Settings load/save
 * ----------------------------------------------------------------------- */

static void settings_set_defaults(void) {
    g_settings.use_fahrenheit = 1;
    g_settings.use_24h = 0;
}

static void settings_save(void) {
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/settings.txt", g_config_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "units=%s\n", g_settings.use_fahrenheit ? "F" : "C");
    fprintf(f, "clock=%s\n", g_settings.use_24h ? "24" : "12");
    fclose(f);
}

static void settings_load(void) {
    settings_set_defaults();
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/settings.txt", g_config_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line; char *val = eq + 1;
        trim_inplace(key); trim_inplace(val);
        if (strcmp(key, "units") == 0)
            g_settings.use_fahrenheit = (val[0] == 'F' || val[0] == 'f') ? 1 : 0;
        else if (strcmp(key, "clock") == 0)
            g_settings.use_24h = (strncmp(val, "24", 2) == 0) ? 1 : 0;
    }
    fclose(f);
    cat_log("settings: loaded (units=%s clock=%s)",
            g_settings.use_fahrenheit ? "F" : "C", g_settings.use_24h ? "24" : "12");
}

/* -----------------------------------------------------------------------
 * Config: Locations (multi-location)
 * ----------------------------------------------------------------------- */

static int get_home_index(void) {
    for (int i = 0; i < g_location_count; i++) {
        if (g_locations[i].is_home) return i;
    }
    return 0;
}

static int load_locations(void) {
    g_location_count = 0;
    if (g_config_dir[0] == '\0') return -1;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/locations.txt", g_config_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "%s/location.txt", g_config_dir);
        f = fopen(path, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            trim_inplace(line);
            if (line[0] == '#' || line[0] == '\0') continue;
            char *sep = strchr(line, '|');
            if (sep) {
                *sep = '\0';
                location_t *loc = &g_locations[0];
                memset(loc, 0, sizeof(*loc));
                snprintf(loc->name, MAX_LOCATION, "%s", line);
                trim_inplace(loc->name);
                char *rest = sep + 1;
                char *sep2 = strchr(rest, '|');
                if (sep2) {
                    *sep2 = '\0';
                    snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
                    trim_inplace(loc->lat_lon);
                    loc->id = atoi(sep2 + 1);
                } else {
                    snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
                    trim_inplace(loc->lat_lon);
                }
                loc->is_home = 1;
                g_location_count = 1;
            }
            break;
        }
        fclose(f);
        if (g_location_count > 0) {
            cat_log("config: migrated legacy location: %s", g_locations[0].name);
            return 0;
        }
        return -1;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && g_location_count < MAX_LOCATIONS) {
        trim_inplace(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        location_t *loc = &g_locations[g_location_count];
        memset(loc, 0, sizeof(*loc));

        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = '\0';
        snprintf(loc->name, MAX_LOCATION, "%s", line);
        trim_inplace(loc->name);

        char *rest = p1 + 1;
        char *p2 = strchr(rest, '|');
        if (p2) {
            *p2 = '\0';
            snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
            trim_inplace(loc->lat_lon);
            rest = p2 + 1;
            char *p3 = strchr(rest, '|');
            if (p3) {
                *p3 = '\0';
                loc->id = atoi(rest);
                loc->is_home = atoi(p3 + 1);
            } else {
                loc->id = atoi(rest);
            }
        } else {
            snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
            trim_inplace(loc->lat_lon);
        }

        if (loc->name[0]) {
            cat_log("config: location %d: %s (id:%d, home:%d)",
                   g_location_count, loc->name, loc->id, loc->is_home);
            g_location_count++;
        }
    }
    fclose(f);

    int home_found = 0;
    for (int i = 0; i < g_location_count; i++) {
        if (g_locations[i].is_home) {
            if (home_found) g_locations[i].is_home = 0;
            home_found = 1;
        }
    }
    if (!home_found && g_location_count > 0)
        g_locations[0].is_home = 1;

    cat_log("config: loaded %d location(s)", g_location_count);
    return (g_location_count > 0) ? 0 : -1;
}

static void save_locations(void) {
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/locations.txt", g_config_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Nimbus — Locations\n");
    fprintf(f, "# Format: Name|lat,lon|id|home\n");
    for (int i = 0; i < g_location_count; i++) {
        fprintf(f, "%s|%s|%d|%d\n",
                g_locations[i].name, g_locations[i].lat_lon,
                g_locations[i].id, g_locations[i].is_home);
    }
    fclose(f);
    cat_log("config: saved %d location(s)", g_location_count);
}

/* -----------------------------------------------------------------------
 * HTTP fetch via libcurl
 * ----------------------------------------------------------------------- */

static size_t fetch_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    fetch_buf_t *buf = (fetch_buf_t *)userdata;
    size_t bytes = size * nmemb;
    while (buf->size + bytes + 1 > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap == 0) new_cap = 8192;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, ptr, bytes);
    buf->size += bytes;
    buf->data[buf->size] = '\0';
    return bytes;
}

static int fetch_url(const char *url, fetch_buf_t *buf) {
    buf->data = NULL; buf->size = 0; buf->capacity = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nimbus/0.3");
    const char *ca = getenv("CURL_CA_BUNDLE");
    if (ca) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        free(buf->data); buf->data = NULL; buf->size = 0;
        return -1;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Icon fetch + cache
 * ----------------------------------------------------------------------- */

static void free_weather_textures(weather_data_t *w) {
    if (w->icon_texture) { SDL_DestroyTexture(w->icon_texture); w->icon_texture = NULL; }
    for (int i = 0; i < w->forecast_count; i++) {
        if (w->forecast[i].icon_texture) {
            SDL_DestroyTexture(w->forecast[i].icon_texture);
            w->forecast[i].icon_texture = NULL;
        }
    }
    for (int i = 0; i < w->hour_count; i++) {
        if (w->hours[i].icon_texture) {
            SDL_DestroyTexture(w->hours[i].icon_texture);
            w->hours[i].icon_texture = NULL;
        }
    }
}

/* -----------------------------------------------------------------------
 * Open-Meteo backend (keyless): unit/format helpers, WMO codes, moon phase,
 * IP geolocation for the default location, and the forecast JSON parser.
 * ----------------------------------------------------------------------- */

static double c2f(double c)     { return c * 9.0 / 5.0 + 32.0; }
static double kmh2mph(double k) { return k * 0.621371; }
static double mm2in(double m)   { return m * 0.0393701; }

/* WMO weather interpretation code -> short label. */
static const char *wmo_text(int code) {
    switch (code) {
        case 0:  return "Clear";
        case 1:  return "Mainly clear";
        case 2:  return "Partly cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: case 63: case 65: return "Rain";
        case 66: case 67: return "Freezing rain";
        case 71: case 73: case 75: return "Snow";
        case 77: return "Snow grains";
        case 80: case 81: case 82: return "Rain showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunderstorm, hail";
        default: return "\xe2\x80\x94"; /* em dash */
    }
}

static void deg_to_compass(double deg, char *out, size_t n) {
    static const char *dirs[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                                 "S","SSW","SW","WSW","W","WNW","NW","NNW"};
    int idx = (int)((deg + 11.25) / 22.5) & 15;
    snprintf(out, n, "%s", dirs[idx]);
}

/* Extract "HH:MM" from an ISO time like "2024-01-15T06:30". */
static void iso_hhmm(const char *iso, char *out, size_t n) {
    const char *t = strchr(iso, 'T');
    if (t && strlen(t) >= 6) snprintf(out, n, "%.5s", t + 1);
    else out[0] = '\0';
}

/* Moon phase + illumination for a Y-M-D date (synodic-month approximation,
   +/- ~1 day). Open-Meteo doesn't provide moon data, so we compute it. */
static void compute_moon_phase(int y, int m, int d, char *out, size_t n, int *illum) {
    long a = (14 - m) / 12;
    long yy = y + 4800 - a;
    long mm = m + 12 * a - 3;
    long jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    double age = fmod((double)jdn - 2451550.1, 29.530588853);
    if (age < 0) age += 29.530588853;
    double frac = age / 29.530588853;            /* 0..1 through the cycle */
    if (illum) *illum = (int)((1.0 - cos(2.0 * M_PI * frac)) / 2.0 * 100.0 + 0.5);
    const char *name =
        frac < 0.0625 ? "New Moon"        : frac < 0.1875 ? "Waxing Crescent" :
        frac < 0.3125 ? "First Quarter"   : frac < 0.4375 ? "Waxing Gibbous"  :
        frac < 0.5625 ? "Full Moon"       : frac < 0.6875 ? "Waning Gibbous"  :
        frac < 0.8125 ? "Last Quarter"    : frac < 0.9375 ? "Waning Crescent" : "New Moon";
    snprintf(out, n, "%s", name);
}

/* Keyless IP geolocation (ip-api.com) for the "auto:ip" default location. */
static int ip_geolocate(double *lat, double *lon, char *city, size_t city_n,
                        char *region, size_t region_n) {
    fetch_buf_t buf;
    if (fetch_url("http://ip-api.com/json/?fields=status,lat,lon,city,regionName", &buf) != 0)
        return -1;
    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) return -1;
    int rc = -1;
    cJSON *st = cJSON_GetObjectItem(root, "status");
    if (st && cJSON_IsString(st) && strcmp(st->valuestring, "success") == 0) {
        cJSON *la = cJSON_GetObjectItem(root, "lat");
        cJSON *lo = cJSON_GetObjectItem(root, "lon");
        cJSON *ci = cJSON_GetObjectItem(root, "city");
        cJSON *re = cJSON_GetObjectItem(root, "regionName");
        if (la && lo) {
            *lat = la->valuedouble; *lon = lo->valuedouble;
            if (ci && cJSON_IsString(ci) && city)   snprintf(city, city_n, "%s", ci->valuestring);
            if (re && cJSON_IsString(re) && region) snprintf(region, region_n, "%s", re->valuestring);
            rc = 0;
        }
    }
    cJSON_Delete(root);
    return rc;
}

static double arr_num(cJSON *arr, int i) {
    cJSON *it = cJSON_GetArrayItem(arr, i);
    return (it && cJSON_IsNumber(it)) ? it->valuedouble : 0.0;
}
static const char *arr_str(cJSON *arr, int i) {
    cJSON *it = cJSON_GetArrayItem(arr, i);
    return (it && cJSON_IsString(it)) ? it->valuestring : "";
}
static double obj_num(cJSON *o, const char *k) {
    cJSON *it = o ? cJSON_GetObjectItem(o, k) : NULL;
    return (it && cJSON_IsNumber(it)) ? it->valuedouble : 0.0;
}

/* Parse an Open-Meteo /v1/forecast response into weather. The caller sets the
   display name afterward (Open-Meteo doesn't return one). */
static int parse_weather_data(const char *json_str, weather_data_t *weather) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;
    memset(weather, 0, sizeof(*weather));
    weather->aqi = -1;   /* set later by the (separate) air-quality fetch */
    weather->loc_lat = obj_num(root, "latitude");
    weather->loc_lon = obj_num(root, "longitude");

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        weather->temp_c       = obj_num(cur, "temperature_2m");
        weather->feels_like_c = obj_num(cur, "apparent_temperature");
        weather->humidity     = (int)obj_num(cur, "relative_humidity_2m");
        weather->is_day       = (int)obj_num(cur, "is_day");
        weather->precip_mm    = obj_num(cur, "precipitation");
        weather->cloud        = (int)obj_num(cur, "cloud_cover");
        weather->wind_kph     = obj_num(cur, "wind_speed_10m");
        weather->uv           = obj_num(cur, "uv_index");
        weather->wind_gust_kph = obj_num(cur, "wind_gusts_10m");
        weather->pressure_hpa  = obj_num(cur, "pressure_msl");
        weather->dew_point_c   = obj_num(cur, "dew_point_2m");
        weather->condition_code = (int)obj_num(cur, "weather_code");
        deg_to_compass(obj_num(cur, "wind_direction_10m"), weather->wind_dir, sizeof(weather->wind_dir));
        weather->temp_f       = c2f(weather->temp_c);
        weather->feels_like_f = c2f(weather->feels_like_c);
        weather->wind_mph     = kmh2mph(weather->wind_kph);
        weather->wind_gust_mph = kmh2mph(weather->wind_gust_kph);
        weather->dew_point_f  = c2f(weather->dew_point_c);
        weather->precip_in    = mm2in(weather->precip_mm);
        snprintf(weather->condition_text, sizeof(weather->condition_text), "%s",
                 wmo_text(weather->condition_code));
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily) {
        cJSON *dt=cJSON_GetObjectItem(daily,"time"), *wc=cJSON_GetObjectItem(daily,"weather_code");
        cJSON *tmax=cJSON_GetObjectItem(daily,"temperature_2m_max"), *tmin=cJSON_GetObjectItem(daily,"temperature_2m_min");
        cJSON *sr=cJSON_GetObjectItem(daily,"sunrise"), *ss=cJSON_GetObjectItem(daily,"sunset");
        cJSON *uvm=cJSON_GetObjectItem(daily,"uv_index_max"), *psum=cJSON_GetObjectItem(daily,"precipitation_sum");
        cJSON *pp=cJSON_GetObjectItem(daily,"precipitation_probability_max"), *wmax=cJSON_GetObjectItem(daily,"wind_speed_10m_max");
        cJSON *amax=cJSON_GetObjectItem(daily,"apparent_temperature_max"), *amin=cJSON_GetObjectItem(daily,"apparent_temperature_min");
        int n = dt ? cJSON_GetArraySize(dt) : 0;
        if (n > MAX_FORECAST_DAYS) n = MAX_FORECAST_DAYS;
        for (int i = 0; i < n; i++) {
            forecast_day_t *fd = &weather->forecast[i];
            snprintf(fd->date, sizeof(fd->date), "%s", arr_str(dt, i));
            snprintf(fd->day_name, sizeof(fd->day_name), "%s", day_name_from_date(fd->date));
            fd->condition_code = (int)arr_num(wc, i);
            snprintf(fd->condition_text, sizeof(fd->condition_text), "%s", wmo_text(fd->condition_code));
            fd->max_temp_c = arr_num(tmax, i); fd->max_temp_f = c2f(fd->max_temp_c);
            fd->min_temp_c = arr_num(tmin, i); fd->min_temp_f = c2f(fd->min_temp_c);
            fd->feels_max_c = arr_num(amax, i); fd->feels_max_f = c2f(fd->feels_max_c);
            fd->feels_min_c = arr_num(amin, i); fd->feels_min_f = c2f(fd->feels_min_c);
            fd->uv = arr_num(uvm, i);
            fd->total_precip_mm = arr_num(psum, i); fd->total_precip_in = mm2in(fd->total_precip_mm);
            fd->chance_rain = (int)arr_num(pp, i);
            fd->max_wind_kph = arr_num(wmax, i); fd->max_wind_mph = kmh2mph(fd->max_wind_kph);
            iso_hhmm(arr_str(sr, i), fd->sunrise, sizeof(fd->sunrise));
            iso_hhmm(arr_str(ss, i), fd->sunset, sizeof(fd->sunset));
            int yy=0,mm=0,dd=0; sscanf(fd->date, "%d-%d-%d", &yy,&mm,&dd);
            compute_moon_phase(yy, mm, dd, fd->moon_phase, sizeof(fd->moon_phase), &fd->moon_illumination);
        }
        weather->forecast_count = n;
    }

    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (hourly) {
        cJSON *ht=cJSON_GetObjectItem(hourly,"time"), *htemp=cJSON_GetObjectItem(hourly,"temperature_2m");
        cJSON *hwc=cJSON_GetObjectItem(hourly,"weather_code"), *hpp=cJSON_GetObjectItem(hourly,"precipitation_probability");
        cJSON *hday=cJSON_GetObjectItem(hourly,"is_day");
        int total = ht ? cJSON_GetArraySize(ht) : 0;
        time_t now = time(NULL);
        char nowstr[20];
        strftime(nowstr, sizeof(nowstr), "%Y-%m-%dT%H:00", localtime(&now));
        int start = 0;
        for (int i = 0; i < total; i++) if (strcmp(arr_str(ht, i), nowstr) >= 0) { start = i; break; }
        int hc = 0;
        for (int i = start; i < total && hc < MAX_HOURS && hc < 24; i++, hc++) {
            hourly_t *h = &weather->hours[hc];
            snprintf(h->time, sizeof(h->time), "%s", arr_str(ht, i));
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%s", h->time);
            char *T = strchr(tmp, 'T'); if (T) *T = ' ';
            format_hour_label(tmp, h->hour_label, sizeof(h->hour_label));
            h->temp_c = arr_num(htemp, i); h->temp_f = c2f(h->temp_c);
            h->condition_code = (int)arr_num(hwc, i);
            snprintf(h->condition_text, sizeof(h->condition_text), "%s", wmo_text(h->condition_code));
            h->chance_rain = (int)arr_num(hpp, i);
            h->is_day = (int)arr_num(hday, i);
        }
        weather->hour_count = hc;
    }

    weather->valid = 1;
    cJSON_Delete(root);
    return 0;
}

/* Open-Meteo returns WMO codes, not icon URLs, so there's nothing to download;
   the condition shows as text. A bundled WMO icon set lands in Phase 3. */
static void load_weather_icons(weather_data_t *weather, int offline) {
    (void)weather; (void)offline;
}

static int load_weather_from_cache(int loc_idx) {
    if (loc_idx < 0 || loc_idx >= g_location_count) return -1;
    char *json = load_weather_json(loc_idx);
    if (!json) return -1;

    weather_data_t *weather = &g_weather_cache[loc_idx];
    int rc = parse_weather_data(json, weather);
    free(json);
    if (rc != 0) return -1;
    snprintf(weather->location_name, sizeof(weather->location_name), "%s",
             g_locations[loc_idx].name);

    weather->is_cached = 1;
    get_weather_cache_time(loc_idx, weather->cached_time, sizeof(weather->cached_time));
    load_weather_icons(weather, 1);

    cat_log("weather_cache: loaded cached data for %s (cached at %s)",
           g_locations[loc_idx].name, weather->cached_time);
    return 0;
}

/* A fetch request: location index + a snapshot of its name/coords, so the worker
   never reads g_locations while the main thread may be mutating it. */
typedef struct {
    int  idx;
    char name[MAX_LOCATION];
    char lat_lon[64];
} fetch_req_t;

/* Pure network + parse for one request into *out. No SDL, no global mutation.
   On a fresh auto:ip resolve, fills res_name/res_ll with the discovered city. */
static int fetch_weather_core(const fetch_req_t *req, weather_data_t *out,
                              char *res_name, size_t name_n, char *res_ll, size_t ll_n) {
    double lat = 0, lon = 0;
    char city[MAX_LOCATION] = {0}, region[MAX_LOCATION] = {0};
    int resolved_name = 0;
    if (req->lat_lon[0] && sscanf(req->lat_lon, "%lf,%lf", &lat, &lon) == 2) {
        /* already have coordinates */
    } else if (strcmp(req->name, "auto:ip") == 0) {
        if (ip_geolocate(&lat, &lon, city, sizeof(city), region, sizeof(region)) != 0) return -1;
        resolved_name = 1;
    } else {
        return -1; /* no coordinates and not auto — needs a geocoding search */
    }

    char url[MAX_URL];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
        "precipitation,weather_code,cloud_cover,wind_speed_10m,wind_direction_10m,uv_index,"
        "wind_gusts_10m,pressure_msl,dew_point_2m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,"
        "uv_index_max,precipitation_sum,precipitation_probability_max,wind_speed_10m_max,"
        "apparent_temperature_max,apparent_temperature_min"
        "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
        "&timezone=auto&forecast_days=3", lat, lon);

    fetch_buf_t buf;
    if (fetch_url(url, &buf) != 0) return -1;
    int rc = parse_weather_data(buf.data, out);
    if (rc == 0) save_weather_json(req->idx, buf.data);
    free(buf.data);
    if (rc != 0) return rc;

    /* Air quality is a separate (keyless) Open-Meteo endpoint; best-effort, so a
       failure here never fails the weather fetch. */
    char aq_url[MAX_URL];
    snprintf(aq_url, sizeof(aq_url),
        "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
        "&current=us_aqi", lat, lon);
    fetch_buf_t aq;
    if (fetch_url(aq_url, &aq) == 0) {
        cJSON *aroot = cJSON_Parse(aq.data);
        if (aroot) {
            cJSON *acur = cJSON_GetObjectItem(aroot, "current");
            cJSON *aqi = acur ? cJSON_GetObjectItem(acur, "us_aqi") : NULL;
            if (aqi && cJSON_IsNumber(aqi)) out->aqi = (int)(aqi->valuedouble + 0.5);
            cJSON_Delete(aroot);
        }
        free(aq.data);
    }

    if (resolved_name && city[0]) {
        if (region[0]) snprintf(res_name, name_n, "%s, %s", city, region);
        else           snprintf(res_name, name_n, "%s", city);
        snprintf(res_ll, ll_n, "%.4f,%.4f", lat, lon);
    }
    return 0;
}

/* Background worker: fetch, then publish into g_weather_cache under the lock. */
static void *fetch_worker(void *arg) {
    fetch_req_t req = *(fetch_req_t *)arg;
    free(arg);

    weather_data_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    char rname[MAX_LOCATION] = {0}, rll[64] = {0};
    int rc = fetch_weather_core(&req, &tmp, rname, sizeof(rname), rll, sizeof(rll));

    pthread_mutex_lock(&g_data_lock);
    if (rc == 0 && req.idx >= 0 && req.idx < g_location_count) {
        if (rname[0]) {
            snprintf(g_locations[req.idx].name, MAX_LOCATION, "%s", rname);
            snprintf(g_locations[req.idx].lat_lon, sizeof(g_locations[req.idx].lat_lon), "%s", rll);
            g_locations[req.idx].id = 0;
            save_locations();
        }
        snprintf(tmp.location_name, sizeof(tmp.location_name), "%s", g_locations[req.idx].name);
        tmp.is_cached = 0;
        tmp.cached_time[0] = '\0';
        g_weather_cache[req.idx] = tmp;
        cat_log("weather: %s  %.0fC (%s)  %dh %dd", tmp.location_name,
                tmp.temp_c, tmp.condition_text, tmp.hour_count, tmp.forecast_count);
    }
    g_fetch_active = 0;
    pthread_mutex_unlock(&g_data_lock);
    return NULL;
}

/* Kick off a non-blocking refresh for a location (no-op if offline or one is
   already running). The UI keeps rendering cached data meanwhile. */
static void request_refresh(int idx) {
    if (idx < 0 || idx >= g_location_count) return;
    if (!check_wifi()) return;

    pthread_mutex_lock(&g_data_lock);
    if (g_fetch_active) { pthread_mutex_unlock(&g_data_lock); return; }
    g_fetch_active = 1;
    fetch_req_t *req = malloc(sizeof(*req));
    if (!req) { g_fetch_active = 0; pthread_mutex_unlock(&g_data_lock); return; }
    req->idx = idx;
    snprintf(req->name, sizeof(req->name), "%s", g_locations[idx].name);
    snprintf(req->lat_lon, sizeof(req->lat_lon), "%s", g_locations[idx].lat_lon);
    pthread_mutex_unlock(&g_data_lock);

    pthread_t t;
    if (pthread_create(&t, NULL, fetch_worker, req) == 0) {
        pthread_detach(t);
    } else {
        free(req);
        pthread_mutex_lock(&g_data_lock);
        g_fetch_active = 0;
        pthread_mutex_unlock(&g_data_lock);
    }
}

/* -----------------------------------------------------------------------
 * Screens: About
 * ----------------------------------------------------------------------- */

static void show_about(void) {
    cat_detail_info_pair info[] = {
        { .key = "Version",  .value = NIMBUS_VERSION },
        { .key = "Platform", .value = CAT_PLATFORM_NAME },
        { .key = "UI",       .value = "Catastrophe" },
        { .key = "Data",     .value = "Open-Meteo" },
        { .key = "License",  .value = "MIT" },
    };
    cat_detail_section sections[] = {
        { .type = CAT_SECTION_DESCRIPTION, .description = "Weather app for Leaf" },
        { .type = CAT_SECTION_INFO, .title = "Info",
          .info_pairs = info, .info_count = 5 },
        { .type = CAT_SECTION_DESCRIPTION, .title = "Credits",
          .description = "Nimbus by Eric Reinsmidt\n"
                         "Built with Catastrophe for Leaf\n"
                         "Weather data by Open-Meteo (CC-BY 4.0)\n"
                         "Weather Icons by Erik Flowers (OFL 1.1)" },
    };
    cat_footer_item footer[] = { { .button = CAT_BTN_B, .label = "Back" } };
    cat_theme *theme = cat_get_theme();
    cat_detail_opts opts = {
        .title = "Nimbus",
        .sections = sections,
        .section_count = 3,
        .footer = footer,
        .footer_count = NB_HINTS(1),
        /* Section headers ("Info"/"Credits") default to a faint accent; use the
           saturated theme color (highlight) so they read darker but stay on-theme. */
        .section_title_color = &theme->highlight,
    };
    cat_detail_result res;
    cat_detail_screen(&opts, &res);
}

/* -----------------------------------------------------------------------
 * Location search + management
 * ----------------------------------------------------------------------- */

#define MAX_SEARCH_RESULTS 10

typedef struct {
    char name[MAX_LOCATION];
    char region[MAX_LOCATION];
    char country[MAX_LOCATION];
    double lat;
    double lon;
    int    id;
} search_result_t;

static int search_locations(const char *query, search_result_t *results, int max_results) {
    if (!query || !query[0]) return 0;

    /* URL-encode the query (alnum + a few safe chars pass; others -> %XX). */
    char enc[256]; size_t ei = 0;
    for (const char *p = query; *p && ei + 4 < sizeof(enc); p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            enc[ei++] = (char)c;
        } else {
            static const char *hex = "0123456789ABCDEF";
            enc[ei++] = '%'; enc[ei++] = hex[c >> 4]; enc[ei++] = hex[c & 15];
        }
    }
    enc[ei] = '\0';

    char url[MAX_URL];
    snprintf(url, sizeof(url),
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json",
        enc, max_results > 0 ? max_results : 10);

    nb_loading("Searching...");
    fetch_buf_t buf;
    if (fetch_url(url, &buf) != 0) return 0;
    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) return 0;

    int count = 0;
    cJSON *arr = cJSON_GetObjectItem(root, "results");
    if (arr && cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n && count < max_results; i++) {
            cJSON *it = cJSON_GetArrayItem(arr, i);
            if (!it) continue;
            search_result_t *r = &results[count];
            memset(r, 0, sizeof(*r));
            cJSON *nm = cJSON_GetObjectItem(it, "name");
            cJSON *a1 = cJSON_GetObjectItem(it, "admin1");
            cJSON *co = cJSON_GetObjectItem(it, "country");
            cJSON *la = cJSON_GetObjectItem(it, "latitude");
            cJSON *lo = cJSON_GetObjectItem(it, "longitude");
            if (nm && cJSON_IsString(nm)) snprintf(r->name, sizeof(r->name), "%s", nm->valuestring);
            if (a1 && cJSON_IsString(a1)) snprintf(r->region, sizeof(r->region), "%s", a1->valuestring);
            if (co && cJSON_IsString(co)) snprintf(r->country, sizeof(r->country), "%s", co->valuestring);
            if (la && cJSON_IsNumber(la)) r->lat = la->valuedouble;
            if (lo && cJSON_IsNumber(lo)) r->lon = lo->valuedouble;
            r->id = 0;
            count++;
        }
    }
    cJSON_Delete(root);
    return count;
}

static int search_and_add_location(void) {
    if (g_location_count >= MAX_LOCATIONS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Maximum of %d locations reached.", MAX_LOCATIONS);
        nb_message(msg, "OK");
        return -1;
    }
    cat_keyboard_result kb_result;
    int rc = cat_keyboard("", "City, zip, or postal code", CAT_KB_GENERAL, &kb_result);
    if (rc != CAT_OK || kb_result.text[0] == '\0') return -1;
    search_result_t results[MAX_SEARCH_RESULTS];
    int count = search_locations(kb_result.text, results, MAX_SEARCH_RESULTS);
    if (count == 0) {
        nb_message("No locations found.\nTry a different search.", "OK");
        return -1;
    }
    static char result_labels[MAX_SEARCH_RESULTS][512];
    cat_list_item items[MAX_SEARCH_RESULTS];
    memset(items, 0, sizeof(items));
    for (int i = 0; i < count; i++) {
        if (results[i].region[0])
            snprintf(result_labels[i], sizeof(result_labels[i]), "%s, %s, %s",
                     results[i].name, results[i].region, results[i].country);
        else
            snprintf(result_labels[i], sizeof(result_labels[i]), "%s, %s",
                     results[i].name, results[i].country);
        items[i].label = result_labels[i];
    }
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Cancel" },
        { .button = CAT_BTN_A, .label = "Select", .is_confirm = true },
    };
    cat_list_opts opts = cat_list_default_opts("Select Location", items, count);
    opts.footer = footer;
    opts.footer_count = NB_HINTS(2);
    cat_list_result result;
    if (cat_list(&opts, &result) != CAT_OK || result.selected_index < 0) return -1;
    search_result_t *sel = &results[result.selected_index];
    location_t *loc = &g_locations[g_location_count];
    memset(loc, 0, sizeof(*loc));
    if (sel->region[0])
        snprintf(loc->name, MAX_LOCATION, "%s, %s", sel->name, sel->region);
    else
        snprintf(loc->name, MAX_LOCATION, "%s", sel->name);
    snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%.2f,%.2f", sel->lat, sel->lon);
    loc->id = sel->id;
    loc->is_home = (g_location_count == 0) ? 1 : 0;
    memset(&g_weather_cache[g_location_count], 0, sizeof(weather_data_t));
    g_location_count++;
    save_locations();
    return g_location_count - 1;
}

static void show_location_options(int loc_idx) {
    if (loc_idx < 0 || loc_idx >= g_location_count) return;
    char title[256];
    snprintf(title, sizeof(title), "%s", g_locations[loc_idx].name);
    cat_list_item items[2];
    memset(items, 0, sizeof(items));
    items[0].label = "Set as Home";
    items[1].label = "Delete";
    int item_count = (g_location_count <= 1) ? 1 : 2;
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = "Back" },
        { .button = CAT_BTN_A, .label = "Select", .is_confirm = true },
    };
    cat_list_opts opts = cat_list_default_opts(title, items, item_count);
    opts.footer = footer;
    opts.footer_count = NB_HINTS(2);
    cat_list_result result;
    if (cat_list(&opts, &result) != CAT_OK) return;
    switch (result.selected_index) {
        case 0:
            for (int i = 0; i < g_location_count; i++) g_locations[i].is_home = 0;
            g_locations[loc_idx].is_home = 1;
            save_locations();
            break;
        case 1: {
            char msg[256];
            snprintf(msg, sizeof(msg), "Delete \"%s\"?", g_locations[loc_idx].name);
            if (nb_confirm(msg, "Delete", "Cancel")) {
                int was_home = g_locations[loc_idx].is_home;
                free_weather_textures(&g_weather_cache[loc_idx]);
                for (int i = loc_idx; i < g_location_count - 1; i++) {
                    g_locations[i] = g_locations[i + 1];
                    g_weather_cache[i] = g_weather_cache[i + 1];
                }
                g_location_count--;
                memset(&g_weather_cache[g_location_count], 0, sizeof(weather_data_t));
                if (was_home && g_location_count > 0) g_locations[0].is_home = 1;
                if (g_current_location >= g_location_count)
                    g_current_location = g_location_count > 0 ? g_location_count - 1 : 0;
                save_locations();
            }
            break;
        }
    }
}

static void show_locations(void) {
    int focus = 0;
    while (1) {
        static char loc_labels[MAX_LOCATIONS][MAX_LOCATION + 16];
        cat_list_item items[MAX_LOCATIONS];
        memset(items, 0, sizeof(items));
        for (int i = 0; i < g_location_count; i++) {
            if (g_locations[i].is_home)
                snprintf(loc_labels[i], sizeof(loc_labels[i]), "%s (Home)", g_locations[i].name);
            else
                snprintf(loc_labels[i], sizeof(loc_labels[i]), "%s", g_locations[i].name);
            items[i].label = loc_labels[i];
        }
        cat_footer_item footer[] = {
            { .button = CAT_BTN_B, .label = "Back" },
            { .button = CAT_BTN_X, .label = "Add" },
            { .button = CAT_BTN_A, .label = "Options", .is_confirm = true },
        };
        cat_list_opts opts = cat_list_default_opts("Locations", items, g_location_count);
        opts.footer = footer;
        opts.footer_count = NB_HINTS(3);
        opts.secondary_action_button = CAT_BTN_X;
        opts.initial_index = focus;
        cat_list_result result;
        cat_list(&opts, &result);
        if (result.selected_index >= 0) focus = result.selected_index;
        if (result.action == CAT_ACTION_BACK) return;
        if (result.action == CAT_ACTION_SECONDARY_TRIGGERED) { search_and_add_location(); continue; }
        if (result.action == CAT_ACTION_SELECTED && result.selected_index >= 0)
            show_location_options(result.selected_index);
    }
}

/* -----------------------------------------------------------------------
 * Screens: Settings
 * ----------------------------------------------------------------------- */

static void show_settings(void) {
    int focus = 0;
    while (1) {
        /* Units is a cycleable option (Left/Right or A); Locations/About are
           navigation rows (A opens them). Native cat_options_list. */
        cat_option unit_opts[] = {
            { .label = "Fahrenheit", .value = "\xc2\xb0""F" },
            { .label = "Celsius",    .value = "\xc2\xb0""C" },
        };
        cat_option clock_opts[] = {
            { .label = "12-hour", .value = "12h" },
            { .label = "24-hour", .value = "24h" },
        };
        cat_options_item items[4];
        memset(items, 0, sizeof(items));
        items[0].label = "Units";
        items[0].type = CAT_OPT_STANDARD;
        items[0].options = unit_opts;
        items[0].option_count = 2;
        items[0].selected_option = g_settings.use_fahrenheit ? 0 : 1;
        items[1].label = "Time";
        items[1].type = CAT_OPT_STANDARD;
        items[1].options = clock_opts;
        items[1].option_count = 2;
        items[1].selected_option = g_settings.use_24h ? 1 : 0;
        items[2].label = "Locations";
        items[2].type = CAT_OPT_CLICKABLE;
        items[3].label = "About";
        items[3].type = CAT_OPT_CLICKABLE;

        cat_footer_item footer[] = {
            { .button = CAT_BTN_B, .label = "Back" },
            { .button = CAT_BTN_A, .label = "Select", .is_confirm = true },
        };
        cat_options_list_opts opts = {
            .title = "Settings",
            .items = items,
            .item_count = 4,
            .footer = footer,
            .footer_count = NB_HINTS(2),
            .initial_selected_index = focus,
        };
        cat_options_list_result res;
        cat_options_list(&opts, &res);
        focus = res.focused_index;

        /* Commit the cycleable options. */
        g_settings.use_fahrenheit = (items[0].selected_option == 0) ? 1 : 0;
        g_settings.use_24h = (items[1].selected_option == 1) ? 1 : 0;
        settings_save();

        if (res.action == CAT_ACTION_SELECTED) {
            if (res.focused_index == 2) show_locations();
            else if (res.focused_index == 3) show_about();
            continue;
        }
        return;   /* B / back closes Settings */
    }
}

/* -----------------------------------------------------------------------
 * Weather screen: tab drawing helpers
 * ----------------------------------------------------------------------- */

/* Draw text horizontally centered on cx. */
static void draw_centered_text(TTF_Font *f, const char *s, int cx, int y, cat_draw_color col) {
    int w = cat_measure_text(f, s);
    cat_draw_text(f, s, cx - w / 2, y, col);
}

/* ── Nimbus type scale ───────────────────────────────────────────────────
   We inherit Leaf's font *family* but set our own sizes: the CAT_FONT_* tiers
   are just presets (EXTRA_LARGE = 24 base = 48px on MLP1). A handheld you glance
   at wants big, chunky type, so Nimbus carries its own larger ladder. Sizes are
   "base" units (× device_scale at runtime, exactly like the tiers), opened from
   the active theme font and cached by (path, px). */
#define NB_SZ_TEMP   76   /* hero temperature           (~152px on MLP1) */
#define NB_SZ_GLYPH  90   /* weather glyph centerpiece  (~180px) */
#define NB_SZ_COND   30   /* condition line             (~60px) */
#define NB_SZ_VALUE  28   /* stat value                 (~56px) */
#define NB_SZ_LABEL  17   /* stat / feels-like caption  (~34px) */
#define NB_SZ_DETAIL 15   /* Current secondary details line (~30px) */

#define NB_FONT_CACHE 16
static struct { char path[512]; int px; TTF_Font *f; } nb_fcache[NB_FONT_CACHE];
static int nb_fcache_n = 0;

/* Open `path` at the resolution-scaled size for `base`, cached for the process
   lifetime (no per-frame re-open). Returns NULL if the file can't be opened. */
static TTF_Font *nb_open_font(const char *path, int base) {
    if (!path || !path[0]) return NULL;
    int px = cat_font_size_for_resolution(base);
    for (int i = 0; i < nb_fcache_n; i++)
        if (nb_fcache[i].px == px && strcmp(nb_fcache[i].path, path) == 0)
            return nb_fcache[i].f;
    TTF_Font *f = TTF_OpenFont(path, px);
    if (!f) return NULL;
    if (nb_fcache_n < NB_FONT_CACHE) {
        int i = nb_fcache_n++;
        snprintf(nb_fcache[i].path, sizeof(nb_fcache[i].path), "%s", path);
        nb_fcache[i].px = px;
        nb_fcache[i].f = f;
    }
    return f;
}

/* Theme UI font at a Nimbus size (falls back to the nearest tier if the theme
   font path is unavailable, e.g. desktop dev without a Leaf snapshot). */
static TTF_Font *nb_font(int base) {
    cat_theme *th = cat_get_theme();
    TTF_Font *f = nb_open_font(th->font_path, base);
    return f ? f : cat_get_font(CAT_FONT_EXTRA_LARGE);
}

/* Weather Icons glyph font (bundled in res/fonts). NULL if missing. */
static TTF_Font *nb_glyph_font(int base) {
    static char path[512] = {0};
    if (!path[0]) {
        const char *pak = getenv("NIMBUS_PAK_DIR");
        if (pak) snprintf(path, sizeof(path), "%s/res/fonts/weathericons.ttf", pak);
        else     snprintf(path, sizeof(path), "res/fonts/weathericons.ttf");
    }
    return nb_open_font(path, base);
}

/* UTF-8 encode a codepoint into one of a few rotating buffers (so two glyphs
   can be live at once). */
static const char *nb_utf8(unsigned cp) {
    static char bufs[4][5];
    static int slot = 0;
    slot = (slot + 1) & 3;
    char *b = bufs[slot];
    if (cp < 0x80) { b[0] = (char)cp; b[1] = 0; }
    else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); b[2] = 0;
    } else {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        b[3] = 0;
    }
    return b;
}

/* WMO weather code (+ day/night) -> Weather Icons glyph codepoint. */
static unsigned wmo_glyph_cp(int code, int is_day) {
    switch (code) {
        case 0:  case 1:  return is_day ? 0xF00D : 0xF02E; /* clear */
        case 2:           return is_day ? 0xF002 : 0xF086; /* partly cloudy */
        case 3:           return 0xF013;                   /* overcast */
        case 45: case 48: return is_day ? 0xF003 : 0xF04A; /* fog */
        case 51: case 53: case 55: return is_day ? 0xF00B : 0xF01C; /* drizzle */
        case 56: case 57: return 0xF0B5;                   /* freezing drizzle */
        case 61: case 63: case 65: return is_day ? 0xF008 : 0xF028; /* rain */
        case 66: case 67: return 0xF0B5;                   /* freezing rain */
        case 71: case 73: case 75: case 77: return is_day ? 0xF00A : 0xF01B; /* snow */
        case 80: case 81: case 82: return is_day ? 0xF009 : 0xF01A; /* rain showers */
        case 85: case 86: return is_day ? 0xF00A : 0xF01B; /* snow showers */
        case 95:          return is_day ? 0xF010 : 0xF01E; /* thunderstorm */
        case 96: case 99: return 0xF01E;                   /* thunderstorm + hail */
        default:          return 0xF07B;                   /* n/a */
    }
}

/* Moon-phase name -> Weather Icons moon glyph codepoint. */
static unsigned moon_glyph_cp(const char *phase) {
    if (!phase) return 0xF0A3;
    if (strstr(phase, "New"))            return 0xF095;
    if (strstr(phase, "Waxing Crescent"))return 0xF098;
    if (strstr(phase, "First"))          return 0xF09C;
    if (strstr(phase, "Waxing Gibbous")) return 0xF09F;
    if (strstr(phase, "Full"))           return 0xF0A3;
    if (strstr(phase, "Waning Gibbous")) return 0xF0A6;
    if (strstr(phase, "Last"))           return 0xF0AA;
    if (strstr(phase, "Waning Crescent"))return 0xF0AD;
    return 0xF0A3;
}

/* Draw a glyph centered on (cx, cy) both horizontally and vertically; no-op if
   the glyph font is missing. */
static void draw_glyph_mid(unsigned cp, int base, int cx, int cy, cat_draw_color col) {
    TTF_Font *gf = nb_glyph_font(base);
    if (!gf) return;
    const char *g = nb_utf8(cp);
    int w = cat_measure_text(gf, g);
    cat_draw_text(gf, g, cx - w / 2, cy - TTF_FontHeight(gf) / 2, col);
}

/* Current tab — big, glanceable hero for a handheld: a large weather glyph
   beside a huge temperature, the condition and feels-like below, then one row
   of oversized stats. Built on the Nimbus type scale above. */
static void draw_tab_current(weather_data_t *weather, int content_y, int content_h,
                              int scroll_y, int *total_h) {
    int sw = cat_get_screen_width();
    int cx = sw / 2;

    TTF_Font *f_temp  = nb_font(NB_SZ_TEMP);
    TTF_Font *f_cond  = nb_font(NB_SZ_COND);
    TTF_Font *f_cap   = nb_font(NB_SZ_LABEL);
    TTF_Font *f_val   = nb_font(NB_SZ_VALUE);
    TTF_Font *f_glyph = nb_glyph_font(NB_SZ_GLYPH);

    cat_theme *theme = cat_get_theme();
    cat_draw_color text_color = theme->text;
    cat_draw_color hint_color = theme->hint;
    cat_draw_color glyph_color = theme->highlight;

    int y = content_y - scroll_y + CAT_DS(6);

    /* Glyph + temperature, centered together on one row. */
    char temp_str[32];
    snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0",
             g_settings.use_fahrenheit ? weather->temp_f : weather->temp_c);

    const char *glyph = f_glyph
        ? nb_utf8(wmo_glyph_cp(weather->condition_code, weather->is_day)) : NULL;
    int gap     = CAT_DS(10);
    int glyph_w = glyph ? cat_measure_text(f_glyph, glyph) + gap : 0;
    int temp_w  = cat_measure_text(f_temp, temp_str);
    int gx      = cx - (glyph_w + temp_w) / 2;

    /* Row advances by the temperature height only. The Weather Icons font has
       very tall internal metrics, so its glyph box is centered on the same row
       center as the temperature and its empty padding overflows into the
       surrounding whitespace (invisible) — using its line height as row_h would
       inflate the hero ~2x and push the stats off the bottom. */
    int temp_h  = TTF_FontHeight(f_temp);
    int glyph_h = glyph ? TTF_FontHeight(f_glyph) : 0;
    int row_h   = temp_h;
    int row_cy  = y + row_h / 2;

    if (glyph) {
        cat_draw_text(f_glyph, glyph, gx, row_cy - glyph_h / 2, glyph_color);
        gx += glyph_w;
    }
    cat_draw_text(f_temp, temp_str, gx, row_cy - temp_h / 2, text_color);
    /* Advance to just below the visible temp/glyph, trimming the font's descent
       padding (the digits have no descenders) so the hero isn't needlessly tall. */
    y = row_cy + temp_h * 36 / 100 + CAT_DS(8);

    /* Condition + feels-like, centered. */
    draw_centered_text(f_cond, weather->condition_text, cx, y, text_color);
    y += TTF_FontHeight(f_cond) + CAT_DS(2);

    char feels_str[40];
    snprintf(feels_str, sizeof(feels_str), "Feels like %.0f\xc2\xb0",
             g_settings.use_fahrenheit ? weather->feels_like_f : weather->feels_like_c);
    draw_centered_text(f_cap, feels_str, cx, y, hint_color);
    y += TTF_FontHeight(f_cap);

    /* One row of four big stats, plus a subtle details line for the extras. */
    int use_f = g_settings.use_fahrenheit;
    struct { char value[24]; const char *label; } stats[4];
    snprintf(stats[0].value, sizeof(stats[0].value), "%d%%", weather->humidity);
    stats[0].label = "Humidity";
    snprintf(stats[1].value, sizeof(stats[1].value), "%.0f %s",
             use_f ? weather->wind_mph : weather->wind_kph, weather->wind_dir);
    stats[1].label = use_f ? "Wind mph" : "Wind kmh";
    snprintf(stats[2].value, sizeof(stats[2].value), "%.0f", weather->uv);
    stats[2].label = "UV Index";
    if (weather->aqi >= 0) snprintf(stats[3].value, sizeof(stats[3].value), "%d", weather->aqi);
    else                   stats[3].value[0] = '\0';
    stats[3].label = "Air Quality";

    char details[96];
    if (use_f)
        snprintf(details, sizeof(details),
                 "Gust %.0f mph  \xc2\xb7  %.1f in  \xc2\xb7  Dew %.0f\xc2\xb0",
                 weather->wind_gust_mph, weather->pressure_hpa * 0.02953, weather->dew_point_f);
    else
        snprintf(details, sizeof(details),
                 "Gust %.0f kmh  \xc2\xb7  %.0f mb  \xc2\xb7  Dew %.0f\xc2\xb0",
                 weather->wind_gust_kph, weather->pressure_hpa, weather->dew_point_c);

    /* Pin the stat row + details line to the bottom of the content area so it's
       always fully on screen under the big hero (no hidden scroll). */
    TTF_Font *f_det = nb_font(NB_SZ_DETAIL);
    int val_h = TTF_FontHeight(f_val), lbl_h = TTF_FontHeight(f_cap), det_h = TTF_FontHeight(f_det);
    int block_h = val_h + lbl_h + CAT_DS(4) + det_h;
    int stat_y = content_y + content_h - block_h - CAT_DS(6);
    if (stat_y < y + CAT_DS(8)) stat_y = y + CAT_DS(8);  /* never overlap the hero */

    int margin = CAT_DS(16);
    int col_w  = (sw - margin * 2) / 4;
    for (int i = 0; i < 4; i++) {
        int scx = margin + col_w * i + col_w / 2;
        draw_centered_text(f_val, stats[i].value[0] ? stats[i].value : "\xe2\x80\x94", scx, stat_y, text_color);
        draw_centered_text(f_cap, stats[i].label, scx, stat_y + val_h, hint_color);
    }
    draw_centered_text(f_det, details, cx, stat_y + val_h + lbl_h + CAT_DS(4), hint_color);

    /* Content fits the viewport — no scrolling on Current. */
    *total_h = content_h;
}

static void draw_tab_forecast(weather_data_t *weather, int content_y, int content_h,
                               int scroll_y, int *total_h) {
    int sw = cat_get_screen_width();

    TTF_Font *f_day  = nb_font(NB_SZ_COND);   /* day name */
    TTF_Font *f_cap  = nb_font(NB_SZ_LABEL);  /* condition / rain */
    TTF_Font *f_temp = nb_font(NB_SZ_VALUE);  /* hi / lo */

    cat_theme *theme = cat_get_theme();
    cat_draw_color text_color = theme->text;
    cat_draw_color hint_color = theme->hint;
    cat_draw_color glyph_color = theme->highlight;

    int n = weather->forecast_count;
    int margin    = CAT_DS(18);
    int glyph_base = NB_SZ_COND + 8;          /* a touch bigger than the day text */
    int glyph_cx  = margin + CAT_DS(26);
    int text_x    = margin + CAT_DS(58);
    int row_h     = CAT_DS(88);               /* chunky fixed rows */

    int y = content_y - scroll_y + CAT_DS(4);
    int day_h = TTF_FontHeight(f_day), cap_h = TTF_FontHeight(f_cap), temp_h = TTF_FontHeight(f_temp);

    for (int i = 0; i < n; i++) {
        forecast_day_t *day = &weather->forecast[i];
        int cy = y + row_h / 2;

        draw_glyph_mid(wmo_glyph_cp(day->condition_code, 1), glyph_base, glyph_cx, cy, glyph_color);

        int use_f = g_settings.use_fahrenheit;

        /* Left: day, condition, rain — vertically centered as a block. */
        const char *dname = (i == 0) ? "Today" : day->day_name;
        char rain[24];
        snprintf(rain, sizeof(rain), "Rain %d%%", day->chance_rain);
        int lblock = day_h + cap_h + cap_h + CAT_DS(4);
        int ly = y + (row_h - lblock) / 2;
        cat_draw_text(f_day, dname, text_x, ly, text_color);
        cat_draw_text_ellipsized(f_cap, day->condition_text, text_x, ly + day_h + CAT_DS(2),
                                 hint_color, sw / 2 - text_x);
        cat_draw_text(f_cap, rain, text_x, ly + day_h + cap_h + CAT_DS(4), hint_color);

        /* Right: high/low + feels-like range, right-aligned and centered. */
        char hilo[48], feels[48];
        snprintf(hilo, sizeof(hilo), "%.0f\xc2\xb0 / %.0f\xc2\xb0",
                 use_f ? day->max_temp_f : day->max_temp_c,
                 use_f ? day->min_temp_f : day->min_temp_c);
        snprintf(feels, sizeof(feels), "Feels %.0f\xc2\xb0 / %.0f\xc2\xb0",
                 use_f ? day->feels_max_f : day->feels_max_c,
                 use_f ? day->feels_min_f : day->feels_min_c);
        int rblock = temp_h + CAT_DS(2) + cap_h;
        int ry = y + (row_h - rblock) / 2;
        int hilo_w = cat_measure_text(f_temp, hilo);
        int feels_w = cat_measure_text(f_cap, feels);
        cat_draw_text(f_temp, hilo, sw - margin - hilo_w, ry, text_color);
        cat_draw_text(f_cap, feels, sw - margin - feels_w, ry + temp_h + CAT_DS(2), hint_color);

        y += row_h;
        if (i < n - 1) cat_draw_rect(margin, y, sw - margin * 2, 1, hint_color);
    }

    y += CAT_DS(6);
    *total_h = y + scroll_y - content_y;
}

static void draw_tab_hourly(weather_data_t *weather, int content_y, int content_h,
                             int scroll_y, int *total_h) {
    int sw = cat_get_screen_width();

    TTF_Font *f_hour = nb_font(NB_SZ_LABEL);  /* hour label */
    TTF_Font *f_temp = nb_font(NB_SZ_VALUE);  /* temp */
    TTF_Font *f_cap  = nb_font(NB_SZ_LABEL);  /* rain */

    cat_theme *theme = cat_get_theme();
    cat_draw_color text_color = theme->text;
    cat_draw_color hint_color = theme->hint;
    cat_draw_color glyph_color = theme->highlight;

    int margin     = CAT_DS(18);
    int glyph_base = NB_SZ_VALUE + 4;
    int glyph_cx   = margin + CAT_DS(22);
    int hour_x     = margin + CAT_DS(48);
    int row_h      = CAT_DS(56);

    int y = content_y - scroll_y + CAT_DS(2);

    int start_hour = 0;
    if (weather->last_updated[0]) {
        int cur_hour = 0;
        const char *space = strchr(weather->last_updated, ' ');
        if (space) sscanf(space + 1, "%d", &cur_hour);
        char date_prefix[16] = {0};
        snprintf(date_prefix, sizeof(date_prefix), "%.10s", weather->last_updated);
        for (int i = 0; i < weather->hour_count; i++) {
            int h_hour = 0;
            const char *hs = strchr(weather->hours[i].time, ' ');
            if (hs) sscanf(hs + 1, "%d", &h_hour);
            if (strncmp(weather->hours[i].time, date_prefix, 10) == 0 && h_hour >= cur_hour) {
                start_hour = i;
                break;
            }
        }
    }

    int shown = 0;
    for (int i = start_hour; i < weather->hour_count && shown < 24; i++, shown++) {
        hourly_t *hr = &weather->hours[i];
        int cy = y + row_h / 2;

        draw_glyph_mid(wmo_glyph_cp(hr->condition_code, hr->is_day), glyph_base,
                       glyph_cx, cy, glyph_color);

        /* Reformat live so a 12/24h change applies without a re-fetch. */
        char hour_lbl[16];
        char tnorm[32];
        snprintf(tnorm, sizeof(tnorm), "%s", hr->time);
        char *Tsep = strchr(tnorm, 'T');
        if (Tsep) *Tsep = ' ';
        format_hour_label(tnorm, hour_lbl, sizeof(hour_lbl));
        cat_draw_text(f_hour, hour_lbl, hour_x, cy - TTF_FontHeight(f_hour) / 2, text_color);

        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0",
                 g_settings.use_fahrenheit ? hr->temp_f : hr->temp_c);
        int rain_reserve = CAT_DS(64);
        int temp_w = cat_measure_text(f_temp, temp_str);
        cat_draw_text(f_temp, temp_str, sw - margin - rain_reserve - temp_w,
                      cy - TTF_FontHeight(f_temp) / 2, text_color);

        if (hr->chance_rain > 0) {
            char rain[16];
            snprintf(rain, sizeof(rain), "%d%%", hr->chance_rain);
            int rain_w = cat_measure_text(f_cap, rain);
            cat_draw_text(f_cap, rain, sw - margin - rain_w, cy - TTF_FontHeight(f_cap) / 2, hint_color);
        }

        y += row_h;
        if (shown < 23 && i + 1 < weather->hour_count)
            cat_draw_rect(margin, y, sw - margin * 2, 1, hint_color);
    }

    if (shown == 0) {
        cat_draw_text(f_hour, "No hourly data available", margin, y, hint_color);
        y += TTF_FontHeight(f_hour) + CAT_DS(5);
    }

    y += CAT_DS(6);
    *total_h = y + scroll_y - content_y;
}

static void draw_tab_astro(weather_data_t *weather, int content_y, int content_h,
                            int scroll_y, int *total_h) {
    int sw = cat_get_screen_width();

    TTF_Font *f_day = nb_font(NB_SZ_COND);    /* day header */
    TTF_Font *f_val = nb_font(NB_SZ_VALUE);   /* times / phase */
    TTF_Font *f_cap = nb_font(NB_SZ_LABEL);   /* captions */

    cat_theme *theme = cat_get_theme();
    cat_draw_color text_color = theme->text;
    cat_draw_color hint_color = theme->hint;
    cat_draw_color glyph_color = theme->highlight;

    int margin     = CAT_DS(18);
    int glyph_base = NB_SZ_VALUE + 8;
    int day_h = TTF_FontHeight(f_day), val_h = TTF_FontHeight(f_val), cap_h = TTF_FontHeight(f_cap);

    int gx_l = margin + CAT_DS(22), tx_l = margin + CAT_DS(48);
    int gx_r = sw / 2 + CAT_DS(10), tx_r = gx_r + CAT_DS(26);

    int y = content_y - scroll_y + CAT_DS(4);

    for (int d = 0; d < weather->forecast_count; d++) {
        forecast_day_t *day = &weather->forecast[d];

        cat_draw_text(f_day, (d == 0) ? "Today" : day->day_name, margin, y, text_color);
        y += day_h + CAT_DS(8);

        /* Sun: sunrise (left) + sunset (right), big with accent glyphs. */
        int cy = y + val_h / 2;
        draw_glyph_mid(0xF051, glyph_base, gx_l, cy, glyph_color);   /* sunrise */
        char sr_str[16], ss_str[16];
        if (day->sunrise[0]) format_hhmm(day->sunrise, sr_str, sizeof(sr_str)); else snprintf(sr_str, sizeof(sr_str), "--");
        if (day->sunset[0])  format_hhmm(day->sunset,  ss_str, sizeof(ss_str)); else snprintf(ss_str, sizeof(ss_str), "--");
        cat_draw_text(f_val, sr_str, tx_l, y, text_color);
        draw_glyph_mid(0xF052, glyph_base, gx_r, cy, glyph_color);   /* sunset */
        cat_draw_text(f_val, ss_str, tx_r, y, text_color);
        y += val_h + CAT_DS(4);

        if (day->sunrise[0] && day->sunset[0]) {
            int sr_h=0, sr_m=0, ss_h=0, ss_m=0; char sa[4]={0}, ea[4]={0};
            sscanf(day->sunrise, "%d:%d %2s", &sr_h, &sr_m, sa);
            sscanf(day->sunset,  "%d:%d %2s", &ss_h, &ss_m, ea);
            if (sa[0]=='P' && sr_h!=12) sr_h += 12;
            if (sa[0]=='A' && sr_h==12) sr_h = 0;
            if (ea[0]=='P' && ss_h!=12) ss_h += 12;
            if (ea[0]=='A' && ss_h==12) ss_h = 0;
            int dl = (ss_h*60+ss_m) - (sr_h*60+sr_m);
            if (dl > 0) {
                char b[32]; snprintf(b, sizeof(b), "Day length %dh %dm", dl/60, dl%60);
                cat_draw_text(f_cap, b, tx_l, y, hint_color);
                y += cap_h + CAT_DS(6);
            }
        }

        /* Moon: big phase glyph + name + illumination. */
        if (day->moon_phase[0]) {
            int mcy = y + val_h / 2;
            draw_glyph_mid(moon_glyph_cp(day->moon_phase), glyph_base, gx_l, mcy, glyph_color);
            cat_draw_text(f_val, day->moon_phase, tx_l, y, text_color);
            y += val_h + CAT_DS(2);
            if (day->moon_illumination > 0) {
                char b[24]; snprintf(b, sizeof(b), "%d%% illuminated", day->moon_illumination);
                cat_draw_text(f_cap, b, tx_l, y, hint_color);
                y += cap_h + CAT_DS(2);
            }
        }

        y += CAT_DS(10);
        if (d < weather->forecast_count - 1) {
            cat_draw_rect(margin, y, sw - margin * 2, 1, hint_color);
            y += CAT_DS(12);
        }
    }

    *total_h = y + scroll_y - content_y;
}

/* -----------------------------------------------------------------------
 * Screens: Weather display
 * ----------------------------------------------------------------------- */

/* Leaf-style tab band: the four weather tabs in a centered row, the active one
   in a theme selection pill drawn with cat_draw_pill so it honors the theme's
   pill geometry (radius ratio / corner mask), exactly like the launcher. Returns
   the y just below the band. */
static int draw_tab_band(int active, int top) {
    static const char *labels[TAB_COUNT] = { "Current", "Forecast", "Hourly", "Astro" };
    cat_theme *th = cat_get_theme();
    TTF_Font *f = nb_font(NB_SZ_LABEL);
    int sw = cat_get_screen_width();
    int xpad = CAT_DS(12), gap = CAT_DS(8);
    int fh = TTF_FontHeight(f);
    int pill_h = fh + CAT_DS(10);

    int widths[TAB_COUNT], total = gap * (TAB_COUNT - 1);
    for (int i = 0; i < TAB_COUNT; i++) {
        widths[i] = cat_measure_text(f, labels[i]) + xpad * 2;
        total += widths[i];
    }
    int x = (sw - total) / 2;
    for (int i = 0; i < TAB_COUNT; i++) {
        bool sel = (i == active);
        if (sel)
            cat_draw_pill(x, top, widths[i], pill_h, th->highlight);
        cat_draw_text(f, labels[i], x + xpad, top + (pill_h - fh) / 2,
                      sel ? th->highlighted_text : th->hint);
        x += widths[i] + gap;
    }
    return top + pill_h;
}

static void show_weather_screen(void) {
    int running = 1;
    nb_scroll_state scroll = {0};
    int active_page = TAB_CURRENT;

    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (ev.pressed) {
                switch (ev.button) {
                    case CAT_BTN_B:
                        if (!ev.repeated) running = 0;
                        break;
                    case CAT_BTN_Y:
                        if (!ev.repeated) {
                            show_settings();
                            if (g_location_count > 0 && g_current_location >= g_location_count)
                                g_current_location = get_home_index();
                            pthread_mutex_lock(&g_data_lock);
                            if (!g_weather_cache[g_current_location].valid)
                                load_weather_from_cache(g_current_location);
                            pthread_mutex_unlock(&g_data_lock);
                            request_refresh(g_current_location);
                            scroll = (nb_scroll_state){0};
                        }
                        break;
                    case CAT_BTN_L1:
                        if (!ev.repeated) {
                            active_page--;
                            if (active_page < 0) active_page = TAB_COUNT - 1;
                            scroll = (nb_scroll_state){0};
                        }
                        break;
                    case CAT_BTN_R1:
                        if (!ev.repeated) {
                            active_page++;
                            if (active_page >= TAB_COUNT) active_page = 0;
                            scroll = (nb_scroll_state){0};
                        }
                        break;
                    case CAT_BTN_LEFT:
                    case CAT_BTN_RIGHT:
                        if (!ev.repeated && g_location_count > 1) {
                            int next = (ev.button == CAT_BTN_LEFT)
                                ? (g_current_location - 1 + g_location_count) % g_location_count
                                : (g_current_location + 1) % g_location_count;
                            /* Switch instantly to cached data, refresh in the
                               background (no blocking network call on input). */
                            g_current_location = next;
                            pthread_mutex_lock(&g_data_lock);
                            if (!g_weather_cache[next].valid)
                                load_weather_from_cache(next);
                            pthread_mutex_unlock(&g_data_lock);
                            request_refresh(next);
                            scroll = (nb_scroll_state){0};
                        }
                        break;
                    case CAT_BTN_UP:
                        nb_scroll_handle_input(&scroll, -1, SCROLL_STEP);
                        break;
                    case CAT_BTN_DOWN:
                        nb_scroll_handle_input(&scroll, 1, SCROLL_STEP);
                        break;
                    default:
                        break;
                }
            }
        }

        nb_scroll_animate(&scroll);

        /* Snapshot the (possibly background-updated) weather under the lock so a
           worker thread can't tear the struct mid-draw. */
        weather_data_t wlocal;
        pthread_mutex_lock(&g_data_lock);
        wlocal = g_weather_cache[g_current_location];
        pthread_mutex_unlock(&g_data_lock);
        weather_data_t *weather = &wlocal;
        int fetching = g_fetch_active;

        cat_clear_screen();
        cat_draw_background();

        int sw = cat_get_screen_width();
        int sh = cat_get_screen_height();
        int pad = CAT_DS(5);

        TTF_Font *font_tiny  = cat_get_font(CAT_FONT_TINY);

        cat_theme *theme = cat_get_theme();
        cat_draw_color hint_color = theme->hint;

        /* Reserve the footer's real height so content never draws under the
           hints and the scroll clamp can reach the bottom. With hints off
           (Leaf's global setting), reserve only a small bottom margin. */
        int footer_h = g_show_hints ? cat_get_footer_height() + pad : CAT_DS(8);

        /* Header line: location name (left) + dots (right) */
        /* Leaf-style tab band across the top, then a location subheader. */
        int y = draw_tab_band(active_page, pad * 2) + pad * 2;

        TTF_Font *font_loc = nb_font(NB_SZ_LABEL);
        if (weather->valid) {
            char location_full[512];
            if (weather->region[0])
                snprintf(location_full, sizeof(location_full), "%s, %s",
                         weather->location_name, weather->region);
            else
                snprintf(location_full, sizeof(location_full), "%s",
                         weather->location_name);

            char header_text[640];
            if (g_location_count > 1) {
                snprintf(header_text, sizeof(header_text), "%s (%d/%d)",
                         location_full, g_current_location + 1, g_location_count);
            } else {
                snprintf(header_text, sizeof(header_text), "%s", location_full);
            }
            cat_draw_text_ellipsized(font_loc, header_text, pad * 3, y, theme->text, sw - pad * 6);
        } else {
            cat_draw_text(font_loc, "Nimbus", pad * 3, y, theme->text);
        }

        y += TTF_FontHeight(font_loc) + pad;

        /* Divider */
        cat_draw_rect(pad * 3, y, sw - pad * 6, 1, hint_color);
        y += pad;

        /* Status line — left-aligned so it's always on screen: a live "Updating"
           while a background fetch runs, otherwise the cached-data age. */
        char status_buf[64];
        const char *status = NULL;
        if (fetching) {
            status = "Updating\xe2\x80\xa6";
        } else if (weather->valid && weather->is_cached && weather->cached_time[0]) {
            snprintf(status_buf, sizeof(status_buf), "Cached %s", weather->cached_time);
            status = status_buf;
        }
        if (status) {
            cat_draw_text(font_tiny, status, pad * 3, y, hint_color);
            y += TTF_FontHeight(font_tiny) + pad;
        }

        int content_y = y;
        int content_bottom = sh - footer_h;
        int content_h = content_bottom - content_y;

        SDL_Rect clip = { 0, content_y, sw, content_h };
        SDL_RenderSetClipRect(cat__g.renderer, &clip);

        if (!weather->valid) {
            scroll.scroll_y = 0;
            const char *msg = fetching ? "Fetching weather\xe2\x80\xa6" : "No weather data";
            TTF_Font *f_big = nb_font(NB_SZ_COND);
            int mw = cat_measure_text(f_big, msg);
            cat_draw_text(f_big, msg, (sw - mw) / 2, content_y + content_h / 3, theme->text);
            if (!fetching) {
                TTF_Font *f_small = nb_font(NB_SZ_LABEL);
                const char *hint = "Press Y for Settings";
                int hw = cat_measure_text(f_small, hint);
                cat_draw_text(f_small, hint, (sw - hw) / 2,
                              content_y + content_h / 3 + TTF_FontHeight(f_big) + pad, hint_color);
            }
        } else {
            int total_h = 0;
            switch (active_page) {
                case TAB_CURRENT:
                    draw_tab_current(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
                case TAB_FORECAST:
                    draw_tab_forecast(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
                case TAB_HOURLY:
                    draw_tab_hourly(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
                case TAB_ASTRO:
                    draw_tab_astro(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
            }

            /* Update cached max_scroll for next frame's input clamping */
            nb_scroll_update(&scroll, total_h, content_h);
        }

        SDL_RenderSetClipRect(cat__g.renderer, NULL);

        /* Hints — honor Leaf's global "Show Hints" setting. */
        if (g_show_hints && g_location_count > 1) {
            cat_footer_item hints[] = {
                { .button = CAT_BTN_B, .label = "Quit" },
                { .button = CAT_BTN_NONE, .label = "View", .button_text = "L1/R1" },
                { .button = CAT_BTN_NONE, .label = "City", .button_text = "L/R" },
                { .button = CAT_BTN_Y, .label = "Menu" },
            };
            cat_draw_footer(hints, 4);
        } else if (g_show_hints) {
            cat_footer_item hints[] = {
                { .button = CAT_BTN_B, .label = "Quit" },
                { .button = CAT_BTN_NONE, .label = "View", .button_text = "L1/R1" },
                { .button = CAT_BTN_Y, .label = "Menu" },
            };
            cat_draw_footer(hints, 3);
        }

        cat_present();
    }
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *config_env = getenv("NIMBUS_CONFIG_DIR");
    if (config_env) strncpy(g_config_dir, config_env, sizeof(g_config_dir) - 1);

    const char *cache_env = getenv("NIMBUS_CACHE_DIR");
    if (cache_env) strncpy(g_cache_dir, cache_env, sizeof(g_cache_dir) - 1);

    settings_load();

    const char *log_path = cat_resolve_log_path("nimbus");
    if (log_path) {
        FILE *lf = fopen(log_path, "w");
        if (lf) fclose(lf);
    }

    cat_config cfg = {.window_title       = "Nimbus",.log_path           = log_path,.disable_background = true,
    };
    if (cat_init(&cfg) != CAT_OK) {
        fprintf(stderr, "Failed to initialise Catastrophe\n");
        curl_global_cleanup();
        return 1;
    }

    /* Match Leaf's global hint visibility (Settings > "Show Hints"). */
    g_show_hints = cat_hints_enabled_from_env();

    /* Inherit Leaf's active theme: the user's color scheme + the system font
       (Catastrophe resolves both from the Leaf appearance snapshot via CAT_FONT_PATH;
       no overrides here so Nimbus matches the rest of Leaf). */

    /* Splash screen */
    {
        const char *pak_dir = getenv("NIMBUS_PAK_DIR");
        char splash_path[MAX_PATH_LEN];
        if (pak_dir)
            snprintf(splash_path, sizeof(splash_path), "%s/res/splash.png", pak_dir);
        else
            snprintf(splash_path, sizeof(splash_path), "res/splash.png");
        SDL_Texture *splash = cat_load_image(splash_path);
        if (splash) {
            int sw = cat_get_screen_width();
            int sh = cat_get_screen_height();
            int img_w, img_h;
            SDL_QueryTexture(splash, NULL, NULL, &img_w, &img_h);
            int max_w = sw - CAT_DS(5) * 8;
            int max_h = sh - CAT_DS(5) * 8;
            float scale_w = (float)max_w / (float)img_w;
            float scale_h = (float)max_h / (float)img_h;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;
            int draw_w = (int)(img_w * scale);
            int draw_h = (int)(img_h * scale);
            int x = (sw - draw_w) / 2;
            int y = (sh - draw_h) / 2;
            cat_clear_screen();
            cat_draw_background();
            cat_draw_image(splash, x, y, draw_w, draw_h);
            cat_present();
            int waited = 0;
            while (waited < 1000) {
                cat_input_event ev;
                while (cat_poll_input(&ev)) {
                    if (ev.pressed && !ev.repeated) waited = 1000;
                }
                SDL_Delay(16);
                waited += 16;
            }
            SDL_DestroyTexture(splash);
        }
    }

    cat_log("=== Nimbus v%s starting ===", NIMBUS_VERSION);
    memset(g_weather_cache, 0, sizeof(g_weather_cache));

    /* No API key needed (Open-Meteo is keyless) — go straight to locations. */
    if (load_locations() != 0) {
        g_locations[0] = (location_t){.is_home = 1 };
        snprintf(g_locations[0].name, MAX_LOCATION, "auto:ip");
        g_location_count = 1;
    }

    g_current_location = get_home_index();

    /* Non-blocking startup: show last-known (cached) weather instantly, then
       refresh in the background. No modal WiFi gate — if offline the screen
       shows cached data or a "Fetching"/"No data" state and recovers on its own
       once a refresh succeeds. */
    load_weather_from_cache(g_current_location);
    request_refresh(g_current_location);

    show_weather_screen();

    /* Let any in-flight background fetch finish before tearing down curl/SDL. */
    for (int i = 0; i < 300 && g_fetch_active; i++) SDL_Delay(10);

    for (int i = 0; i < g_location_count; i++)
        free_weather_textures(&g_weather_cache[i]);
    if (g_sunrise_icon) SDL_DestroyTexture(g_sunrise_icon);
    if (g_sunset_icon) SDL_DestroyTexture(g_sunset_icon);

    cat_log("=== Nimbus shutting down ===");
    cat_quit();
    curl_global_cleanup();
    return 0;
}