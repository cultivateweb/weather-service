#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "logger.h"

#define URL_MAX      256
#define MAX_RESPONSE (1024u * 1024u)   /* больше 1 МБ от погодного API не берём */

/* Состояние модуля. static: снаружи его не видно. */
static CURL *g_curl;
static char  g_geo_url[URL_MAX];
static char  g_weather_url[URL_MAX];

/* ---------- Низкий уровень: HTTP ---------- */

typedef struct {
    char  *data;
    size_t len;
} Buf;

/* libcurl вызывает эту функцию, когда приходит очередной кусок ответа. */
static size_t on_data(char *chunk, size_t size, size_t count, void *userp)
{
    Buf   *b   = userp;
    size_t add = size * count;
    char  *p;

    if (add > MAX_RESPONSE || b->len > MAX_RESPONSE - add) return 0;  /* слишком большой ответ */
    p = realloc(b->data, b->len + add + 1);
    if (!p) return 0;
    memcpy(p + b->len, chunk, add);
    b->data = p;
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

/* GET-запрос. Возвращает тело ответа (потом его нужно free) или NULL. */
static char *http_get(const char *url)
{
    Buf      b = {NULL, 0};
    long     status = 0;
    CURLcode rc;

    curl_easy_reset(g_curl);
    curl_easy_setopt(g_curl, CURLOPT_URL, url);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 10L);   /* не ждём дольше 10 секунд */
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);   /* запрос идёт не из главного потока */

    rc = curl_easy_perform(g_curl);
    curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK || status != 200 || !b.data) {
        log_msg("ERROR", "weather API failed: %s (HTTP %ld)", curl_easy_strerror(rc), status);
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* Скачивает адрес и разбирает ответ как JSON. NULL, если что-то не так. */
static cJSON *get_json(const char *url)
{
    char  *text = http_get(url);
    cJSON *json;

    if (!text) return NULL;
    json = cJSON_Parse(text);
    free(text);
    if (!json) log_msg("ERROR", "weather API returned broken JSON");
    return json;
}

/* ---------- Средний уровень: два запроса к Open-Meteo ---------- */

/* Название города -> координаты. */
static WeatherStatus geocode(const char *city, char *name, size_t cap,
                             double *lat, double *lon)
{
    char          url[1024];
    char         *escaped = curl_easy_escape(g_curl, city, 0);   /* "Київ" -> "%D0%9A..." */
    cJSON        *json;
    const cJSON  *first, *la, *lo, *nm;
    WeatherStatus rc = WEATHER_NOT_FOUND;

    if (!escaped) return WEATHER_ERROR;
    snprintf(url, sizeof url, "%s?name=%s&count=1", g_geo_url, escaped);
    curl_free(escaped);

    json = get_json(url);
    if (!json) return WEATHER_ERROR;

    /* Если город не найден, поля "results" в ответе просто нет. */
    first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "results"), 0);
    la = cJSON_GetObjectItemCaseSensitive(first, "latitude");
    lo = cJSON_GetObjectItemCaseSensitive(first, "longitude");
    nm = cJSON_GetObjectItemCaseSensitive(first, "name");
    if (cJSON_IsNumber(la) && cJSON_IsNumber(lo) && cJSON_IsString(nm)) {
        *lat = la->valuedouble;
        *lon = lo->valuedouble;
        snprintf(name, cap, "%s", nm->valuestring);
        rc = WEATHER_OK;
    }
    cJSON_Delete(json);
    return rc;
}

/* Координаты -> текущая температура и ветер. */
static WeatherStatus fetch_current(double lat, double lon, double *temp, double *wind)
{
    char          url[1024];
    cJSON        *json;
    const cJSON  *cur, *t, *w;
    WeatherStatus rc = WEATHER_ERROR;

    snprintf(url, sizeof url,
             "%s?latitude=%.4f&longitude=%.4f&current=temperature_2m,wind_speed_10m",
             g_weather_url, lat, lon);

    json = get_json(url);
    if (!json) return WEATHER_ERROR;

    cur = cJSON_GetObjectItemCaseSensitive(json, "current");
    t   = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    w   = cJSON_GetObjectItemCaseSensitive(cur, "wind_speed_10m");
    if (cJSON_IsNumber(t) && cJSON_IsNumber(w)) {
        *temp = t->valuedouble;
        *wind = w->valuedouble;
        rc = WEATHER_OK;
    }
    cJSON_Delete(json);
    return rc;
}

/* ---------- Открытый интерфейс (то, что объявлено в weather.h) ---------- */

int weather_init(const char *geo_url, const char *weather_url)
{
    if (snprintf(g_geo_url, URL_MAX, "%s", geo_url) >= URL_MAX ||
        snprintf(g_weather_url, URL_MAX, "%s", weather_url) >= URL_MAX) {
        log_msg("ERROR", "weather: API address is too long");
        return -1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl = curl_easy_init();
    if (!g_curl) {
        curl_global_cleanup();
        log_msg("ERROR", "weather: cannot initialise libcurl");
        return -1;
    }
    return 0;
}

WeatherStatus weather_lookup(const char *city, WeatherResult *out)
{
    double        lat, lon;
    WeatherStatus st;

    if (!g_curl) return WEATHER_ERROR;           /* weather_init() не вызывали */
    st = geocode(city, out->place, sizeof out->place, &lat, &lon);
    if (st != WEATHER_OK) return st;
    return fetch_current(lat, lon, &out->temperature_c, &out->wind_kmh);
}

void weather_cleanup(void)
{
    if (!g_curl) return;
    curl_easy_cleanup(g_curl);
    g_curl = NULL;
    curl_global_cleanup();
}
