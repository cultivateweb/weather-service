/*
 * Сквозной тест: настоящий сервер сервиса принимает настоящие HTTP-запросы.
 * Интернет не нужен: вместо Open-Meteo отвечает подставной сайт,
 * который тест поднимает у себя же (на той же библиотеке microhttpd).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <microhttpd.h>
#include <sqlite3.h>

#include "logger.h"
#include "server.h"
#include "weather.h"

#define LOG_FILE     "test_service.log"
#define DB_FILE      "test_service.db"
#define SLOW_SECONDS 3      /* столько «думает» подставной сайт над городом Slow */

static int failures;
static int g_port;          /* порт сервиса, который проверяем */

/* В отличие от test_config, не останавливаемся на первой ошибке: так видно все сразу. */
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---------- Подставной погодный сайт ---------- */

static enum MHD_Result fake_reply(struct MHD_Connection *conn, unsigned int status, const char *text)
{
    struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(text), (void *)text,
                                                                MHD_RESPMEM_MUST_COPY);
    enum MHD_Result      ret;

    if (!resp) return MHD_NO;
    ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static int is_ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

/* Ведёт себя как Open-Meteo:
   /search   — ищет город. Как и настоящий сайт, кириллицу находит только с language=uk.
               Особые названия: Nowhere — такого нет, Broken — битый ответ,
               Slow — ответ через SLOW_SECONDS секунд.
   /forecast — текущая погода (всегда одна и та же). */
static enum MHD_Result fake_handler(void *cls, struct MHD_Connection *conn,
                                    const char *url, const char *method,
                                    const char *version, const char *upload,
                                    size_t *upload_size, void **req)
{
    const char *name, *lang;
    int         uk;
    cJSON      *root, *item;
    char       *text;
    enum MHD_Result ret;

    (void)cls; (void)method; (void)version; (void)upload; (void)upload_size; (void)req;

    if (strcmp(url, "/forecast") == 0)
        return fake_reply(conn, 200,
                          "{\"current\":{\"temperature_2m\":3.4,\"wind_speed_10m\":12.3}}");
    if (strcmp(url, "/search") != 0)
        return fake_reply(conn, 404, "{}");

    name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
    lang = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "language");
    uk   = lang && strcmp(lang, "uk") == 0;
    if (!name) name = "";

    if (strcmp(name, "Slow") == 0) sleep(SLOW_SECONDS);
    if (strcmp(name, "Broken") == 0) return fake_reply(conn, 200, "{oops");
    if (strcmp(name, "Nowhere") == 0 || (!is_ascii(name) && !uk))
        return fake_reply(conn, 200, "{\"generationtime_ms\":0.1}");   /* нет поля results */

    /* Ответ собираем через cJSON: он сам экранирует кавычки и переводы строк. */
    root = cJSON_CreateObject();
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddStringToObject(item, "country", uk ? "Україна" : "Ukraine");
    cJSON_AddNumberToObject(item, "latitude", 50.45);
    cJSON_AddNumberToObject(item, "longitude", 30.52);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "results"), item);
    text = cJSON_PrintUnformatted(root);
    ret  = fake_reply(conn, 200, text ? text : "{}");
    free(text);
    cJSON_Delete(root);
    return ret;
}

/* Каждое подключение в своём потоке: медленный ответ не держит остальные. */
static struct MHD_Daemon *fake_start(unsigned int *port)
{
    struct MHD_Daemon          *d;
    const union MHD_DaemonInfo *info;

    d = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_AUTO_INTERNAL_THREAD,
                         0, NULL, NULL, &fake_handler, NULL, MHD_OPTION_END);
    if (!d) return NULL;
    info  = MHD_get_daemon_info(d, MHD_DAEMON_INFO_BIND_PORT);
    *port = info ? info->port : 0;
    return d;
}

/* ---------- Клиент: шлёт запросы сервису ---------- */

typedef struct {
    long   status;          /* код ответа; 0 — ответа не было */
    char   body[4096];
    size_t len;
    char   allow[64];       /* заголовок Allow, если он был */
} Reply;

static size_t on_body(char *chunk, size_t size, size_t count, void *userp)
{
    Reply *r   = userp;
    size_t add = size * count;

    if (add > sizeof r->body - 1 - r->len) add = sizeof r->body - 1 - r->len;
    memcpy(r->body + r->len, chunk, add);
    r->len += add;
    r->body[r->len] = '\0';
    return size * count;
}

static size_t on_header(char *line, size_t size, size_t count, void *userp)
{
    Reply *r   = userp;
    size_t len = size * count;

    if (len > 6 && strncasecmp(line, "Allow:", 6) == 0) {
        size_t from = 6, to = len;

        while (from < to && line[from] == ' ') from++;
        while (to > from && (line[to - 1] == '\r' || line[to - 1] == '\n')) to--;
        snprintf(r->allow, sizeof r->allow, "%.*s", (int)(to - from), line + from);
    }
    return len;
}

/* body == NULL — запрос без тела. */
static Reply http(const char *method, const char *path, const char *body)
{
    Reply r;
    char  url[256];
    CURL *c = curl_easy_init();

    memset(&r, 0, sizeof r);
    if (!c) return r;
    snprintf(url, sizeof url, "http://127.0.0.1:%d%s", g_port, path);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (strcmp(method, "HEAD") == 0) curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_cleanup(c);
    return r;
}

static int json_str_is(const char *json, const char *key, const char *want)
{
    cJSON       *j  = cJSON_Parse(json);
    const cJSON *v  = cJSON_GetObjectItemCaseSensitive(j, key);
    int          ok = cJSON_IsString(v) && strcmp(v->valuestring, want) == 0;

    cJSON_Delete(j);
    return ok;
}

static int json_num_is(const char *json, const char *key, double want)
{
    cJSON       *j  = cJSON_Parse(json);
    const cJSON *v  = cJSON_GetObjectItemCaseSensitive(j, key);
    int          ok = cJSON_IsNumber(v) && v->valuedouble > want - 1e-9 && v->valuedouble < want + 1e-9;

    cJSON_Delete(j);
    return ok;
}

/* Запрос медленного города в отдельном потоке: пока он ждёт, тест шлёт другие. */
static void *slow_request(void *arg)
{
    *(Reply *)arg = http("POST", "/weather", "{\"city\":\"Slow\"}");
    return NULL;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void pause_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&t, NULL);
}

/* Строка body вида {"city":"aaa...a"}: название из n букв a. */
static char *city_body(size_t n)
{
    char *s = malloc(n + 16);

    if (!s) return NULL;
    memcpy(s, "{\"city\":\"", 9);
    memset(s + 9, 'a', n);
    memcpy(s + 9 + n, "\"}", 3);
    return s;
}

/* ---------- Что записалось в журнал ---------- */

static char *read_file(const char *path)
{
    FILE  *f = fopen(path, "rb");
    char  *buf = NULL;
    long   size;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0
        && (buf = malloc((size_t)size + 1)) != NULL) {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static int file_has(const char *path, const char *needle)
{
    char *text = read_file(path);
    int   ok   = text && strstr(text, needle);

    free(text);
    return ok;
}

/* Есть ли в файле строка, которая начинается с prefix. */
static int file_has_line_starting(const char *path, const char *prefix)
{
    char       *text = read_file(path);
    const char *line = text;
    int         found = 0;

    while (line && *line && !found) {
        found = strncmp(line, prefix, strlen(prefix)) == 0;
        line  = strchr(line, '\n');
        if (line) line++;
    }
    free(text);
    return found;
}

/* Одно число из базы журнала; -1, если запрос не удался (например, нет таблицы). */
static int db_count(const char *sql)
{
    sqlite3      *db = NULL;
    sqlite3_stmt *st = NULL;
    int           n  = -1;

    if (sqlite3_open(DB_FILE, &db) == SQLITE_OK
        && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/* ---------- Сам тест ---------- */

int main(void)
{
    struct MHD_Daemon *fake;
    unsigned int       fake_port = 0;
    char               geo[128], forecast[128];
    char              *body;
    Server            *srv;
    Reply              r;

    remove(LOG_FILE);
    remove(DB_FILE);
    if (log_open(LOG_FILE, DB_FILE) != 0) return 1;

    fake = fake_start(&fake_port);
    if (!fake) { fprintf(stderr, "fake weather API did not start\n"); return 1; }
    snprintf(geo, sizeof geo, "http://127.0.0.1:%u/search", fake_port);
    snprintf(forecast, sizeof forecast, "http://127.0.0.1:%u/forecast", fake_port);
    if (weather_init(geo, forecast, "uk") != 0) return 1;

    srv = server_start(0);
    if (!srv) return 1;
    g_port = server_port(srv);
    CHECK(g_port > 0);

    /* Служебные адреса */
    r = http("GET", "/health", NULL);
    CHECK(r.status == 200);
    CHECK(json_str_is(r.body, "status", "ok"));
    CHECK(http("GET", "/nope", NULL).status == 404);
    CHECK(http("GET", "/weather", NULL).status == 405);

    /* Плохие запросы */
    r = http("POST", "/weather", NULL);
    CHECK(r.status == 400);
    CHECK(json_str_is(r.body, "error", "send JSON like {\"city\": \"Kyiv\"}"));
    CHECK(http("POST", "/weather", "{oops").status == 400);
    CHECK(http("POST", "/weather", "{\"city\":\"\"}").status == 400);
    CHECK(http("POST", "/weather", "{\"city\":5}").status == 400);

    /* Хороший запрос */
    r = http("POST", "/weather", "{\"city\":\"Kyiv\"}");
    CHECK(r.status == 200);
    CHECK(json_str_is(r.body, "city", "Kyiv"));
    CHECK(json_num_is(r.body, "temperature_c", 3.4));
    CHECK(json_num_is(r.body, "wind_kmh", 12.3));

    /* Кириллица: сайт находит её, только если передан язык (у нас uk) */
    r = http("POST", "/weather", "{\"city\":\"Київ\"}");
    CHECK(r.status == 200);
    CHECK(json_str_is(r.body, "city", "Київ"));

    /* Ошибки погодного сайта */
    CHECK(http("POST", "/weather", "{\"city\":\"Nowhere\"}").status == 404);
    CHECK(http("POST", "/weather", "{\"city\":\"Broken\"}").status == 502);

    /* Границы размера: название до 100 байт, тело запроса до 2048 байт */
    body = city_body(100);
    CHECK(body && http("POST", "/weather", body).status == 200);
    free(body);
    body = city_body(101);
    CHECK(body && http("POST", "/weather", body).status == 400);
    free(body);
    body = city_body(3000);
    CHECK(body && http("POST", "/weather", body).status == 413);
    free(body);

    /* Опасные названия: перевод строки и команда для базы */
    CHECK(http("POST", "/weather", "{\"city\":\"Evil\\nINJECTED [ERROR] fake\"}").status == 200);
    CHECK(http("POST", "/weather", "{\"city\":\"x'); DROP TABLE logs;--\"}").status == 200);

    /* Пока один запрос ждёт медленный погодный сайт, остальные обслуживаются сразу. */
    {
        pthread_t t;
        Reply     slow;
        double    t0;
        int       started = pthread_create(&t, NULL, slow_request, &slow) == 0;

        CHECK(started);
        if (started) {
            pause_ms(300);                              /* медленный запрос уже ждёт */
            t0 = now();
            CHECK(http("GET", "/health", NULL).status == 200);
            CHECK(now() - t0 < 1.0);
            CHECK(http("POST", "/weather", "{\"city\":\"Kyiv\"}").status == 200);
            CHECK(now() - t0 < 1.0);
            pthread_join(t, NULL);
            CHECK(slow.status == 200);
        }
    }

    /* Остановка не ждёт медленный погодный сайт. */
    {
        pthread_t t;
        Reply     slow;
        double    t0;
        int       started = pthread_create(&t, NULL, slow_request, &slow) == 0;

        CHECK(started);
        pause_ms(300);                                  /* медленный запрос уже ждёт */
        t0 = now();
        weather_cancel_all();                           /* так делает main.c при остановке */
        server_stop(srv);
        CHECK(now() - t0 < 2.0);
        if (started) pthread_join(t, NULL);
    }
    weather_cleanup();
    MHD_stop_daemon(fake);
    log_close();

    /* Журнал: файл и база */
    CHECK(file_has(LOG_FILE, "request: city=Kyiv"));
    CHECK(file_has(LOG_FILE, "answer: Kyiv"));
    CHECK(!file_has_line_starting(LOG_FILE, "INJECTED"));          /* запись не подделали */
    CHECK(db_count("SELECT count(*) FROM logs WHERE message LIKE '%DROP TABLE%'") == 2);

    if (failures) return 1;
    remove(LOG_FILE);
    remove(DB_FILE);
    puts("service: ok");
    return 0;
}
