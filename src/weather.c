#include "weather.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "logger.h"

#ifdef HAVE_CA_BUNDLE
/* Список центров сертификации, зашитый в программу (CMake, настройка CA_BUNDLE):
   по нему curl проверяет, что говорит с настоящим погодным сайтом. Так
   программе не нужен такой список в системе, и она работает даже на пустой ОС. */
extern const unsigned char ca_bundle[];
extern const size_t        ca_bundle_size;
#endif

#define URL_MAX      256
#define MAX_RESPONSE (1024u * 1024u)   /* больше 1 МБ от погодного API не берём */
#define POOL_MAX     8                 /* столько свободных подключений держим про запас */

/* Состояние модуля. static: снаружи его не видно.
   После weather_init() поля только читаются, поэтому потоки могут
   читать их одновременно. */
static int  g_ready;                   /* weather_init() прошёл успешно */
static char g_geo_url[URL_MAX];
static char g_weather_url[URL_MAX];
static char g_language[8];

/* 1 = сервис останавливается, идущие запросы надо прервать.
   atomic: флаг меняет один поток, а читают другие. */
static atomic_int g_cancel;

/* Запас свободных подключений libcurl. Новое шифрованное соединение
   с сайтом — это около четверти секунды, а готовое отвечает в разы быстрее.
   Поэтому после запроса подключение не закрываем, а кладём сюда для
   следующего. Делить одно подключение между потоками нельзя, а передавать
   из потока в поток можно: в каждый момент им пользуется только один. */
static CURL           *g_pool[POOL_MAX];
static int             g_pool_n;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* Берёт свободное подключение из запаса или создаёт новое. */
static CURL *handle_take(void)
{
    CURL *h = NULL;

    pthread_mutex_lock(&g_pool_lock);
    if (g_pool_n > 0) h = g_pool[--g_pool_n];
    pthread_mutex_unlock(&g_pool_lock);
    return h ? h : curl_easy_init();
}

/* Возвращает подключение в запас; если запас полон, закрывает его. */
static void handle_give_back(CURL *h)
{
    pthread_mutex_lock(&g_pool_lock);
    if (g_pool_n < POOL_MAX) {
        g_pool[g_pool_n++] = h;
        h = NULL;
    }
    pthread_mutex_unlock(&g_pool_lock);
    if (h) curl_easy_cleanup(h);
}

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

/* libcurl вызывает её, пока идёт запрос: часто, пока идут данные, и примерно
   раз в секунду, пока сайт молчит. Не ноль — прервать запрос. Так остановка
   сервиса не ждёт медленный сайт (иначе ждала бы до 20 секунд). */
static int on_progress(void *userp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)userp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return atomic_load(&g_cancel) ? 1 : 0;
}

/* GET-запрос. Возвращает тело ответа (потом его нужно free) или NULL. */
static char *http_get(CURL *curl, const char *url)
{
    Buf      b = {NULL, 0};
    long     status = 0;
    CURLcode rc;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);     /* не ждём дольше 10 секунд */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);     /* запросы идут из нескольких потоков */
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);   /* без этого on_progress не вызывается */
#ifdef HAVE_CA_BUNDLE
    {
        struct curl_blob ca = { (void *)ca_bundle, ca_bundle_size, CURL_BLOB_NOCOPY };

        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    }
#endif

    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK || status != 200 || !b.data) {
        if (rc == CURLE_ABORTED_BY_CALLBACK)
            log_msg("INFO", "weather request cancelled: the service is stopping");
        else
            log_msg("ERROR", "weather API failed: %s (HTTP %ld)", curl_easy_strerror(rc), status);
        free(b.data);
        return NULL;
    }
    return b.data;
}

/* Скачивает адрес и разбирает ответ как JSON. NULL, если что-то не так. */
static cJSON *get_json(CURL *curl, const char *url)
{
    char  *text = http_get(curl, url);
    cJSON *json;

    if (!text) return NULL;
    json = cJSON_Parse(text);
    free(text);
    if (!json) log_msg("ERROR", "weather API returned broken JSON");
    return json;
}

/* ---------- Средний уровень: два запроса к Open-Meteo ---------- */

/* Название города -> координаты, название и страна (в out). */
static WeatherStatus geocode(CURL *curl, const char *city, WeatherResult *out,
                             double *lat, double *lon)
{
    char          url[1024];
    char         *escaped = curl_easy_escape(curl, city, 0);     /* "Київ" -> "%D0%9A..." */
    cJSON        *json;
    const cJSON  *first, *la, *lo, *nm, *co;
    WeatherStatus rc = WEATHER_NOT_FOUND;

    if (!escaped) return WEATHER_ERROR;
    snprintf(url, sizeof url, "%s?name=%s&count=1&language=%s", g_geo_url, escaped, g_language);
    curl_free(escaped);

    json = get_json(curl, url);
    if (!json) return WEATHER_ERROR;

    /* Если город не найден, поля "results" в ответе просто нет. */
    first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "results"), 0);
    la = cJSON_GetObjectItemCaseSensitive(first, "latitude");
    lo = cJSON_GetObjectItemCaseSensitive(first, "longitude");
    nm = cJSON_GetObjectItemCaseSensitive(first, "name");
    co = cJSON_GetObjectItemCaseSensitive(first, "country");
    if (cJSON_IsNumber(la) && cJSON_IsNumber(lo) && cJSON_IsString(nm)) {
        *lat = la->valuedouble;
        *lon = lo->valuedouble;
        snprintf(out->place, sizeof out->place, "%s", nm->valuestring);
        /* Страны может и не быть (например, у места в открытом море). */
        snprintf(out->country, sizeof out->country, "%s",
                 cJSON_IsString(co) ? co->valuestring : "");
        rc = WEATHER_OK;
    }
    cJSON_Delete(json);
    return rc;
}

/* Координаты -> текущая температура и ветер. */
static WeatherStatus fetch_current(CURL *curl, double lat, double lon, double *temp, double *wind)
{
    char          url[1024];
    cJSON        *json;
    const cJSON  *cur, *t, *w;
    WeatherStatus rc = WEATHER_ERROR;

    snprintf(url, sizeof url,
             "%s?latitude=%.4f&longitude=%.4f&current=temperature_2m,wind_speed_10m",
             g_weather_url, lat, lon);

    json = get_json(curl, url);
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

int weather_init(const char *geo_url, const char *weather_url, const char *language)
{
    if (snprintf(g_geo_url, URL_MAX, "%s", geo_url) >= URL_MAX ||
        snprintf(g_weather_url, URL_MAX, "%s", weather_url) >= URL_MAX) {
        log_msg("ERROR", "weather: API address is too long");
        return -1;
    }
    if (snprintf(g_language, sizeof g_language, "%s", language) >= (int)sizeof g_language) {
        log_msg("ERROR", "weather: language code is too long");
        return -1;
    }
    /* Вызывается до запуска сервера, пока других потоков ещё нет. */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_msg("ERROR", "weather: cannot initialise libcurl");
        return -1;
    }
    atomic_store(&g_cancel, 0);
    g_ready = 1;
    return 0;
}

WeatherStatus weather_lookup(const char *city, WeatherResult *out)
{
    CURL         *curl;
    double        lat, lon;
    WeatherStatus st;

    if (!g_ready) return WEATHER_ERROR;           /* weather_init() не вызывали */

    /* Запросы приходят из разных потоков одновременно, а одно подключение
       libcurl нельзя делить между потоками. Поэтому каждый запрос берёт
       своё — из запаса или новое. */
    curl = handle_take();
    if (!curl) {
        log_msg("ERROR", "weather: cannot create a libcurl handle");
        return WEATHER_ERROR;
    }
    st = geocode(curl, city, out, &lat, &lon);
    if (st == WEATHER_OK)
        st = fetch_current(curl, lat, lon, &out->temperature_c, &out->wind_kmh);
    handle_give_back(curl);
    return st;
}

void weather_cancel_all(void)
{
    atomic_store(&g_cancel, 1);
}

void weather_cleanup(void)
{
    if (!g_ready) return;
    g_ready = 0;
    pthread_mutex_lock(&g_pool_lock);
    while (g_pool_n > 0) curl_easy_cleanup(g_pool[--g_pool_n]);
    pthread_mutex_unlock(&g_pool_lock);
    curl_global_cleanup();
}
