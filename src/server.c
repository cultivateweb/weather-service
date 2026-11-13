#include "server.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <microhttpd.h>

#include "logger.h"
#include "weather.h"

#define MAX_BODY        2048   /* макс. размер входящего JSON, байт */
#define MAX_CONNECTIONS 64u    /* больше одновременных подключений не принимаем */

/* Здесь живёт всё, что связано с microhttpd. Снаружи этого не видно. */
struct Server {
    struct MHD_Daemon *daemon;
    atomic_int         stopping;   /* 1 = идёт остановка (флаг читают другие потоки) */
};

/* ---------- Ответы ---------- */

/* Отправляет JSON-ответ и освобождает obj. */
static enum MHD_Result reply(struct MHD_Connection *conn, unsigned int status, cJSON *obj)
{
    char                *text = cJSON_PrintUnformatted(obj);
    struct MHD_Response *resp;
    enum MHD_Result      ret = MHD_NO;

    if (text) {
        resp = MHD_create_response_from_buffer(strlen(text), text, MHD_RESPMEM_MUST_COPY);
        if (resp) {
            MHD_add_response_header(resp, "Content-Type", "application/json");
            ret = MHD_queue_response(conn, status, resp);
            MHD_destroy_response(resp);
        }
        free(text);
    }
    cJSON_Delete(obj);
    return ret;
}

static enum MHD_Result reply_error(struct MHD_Connection *conn, unsigned int status, const char *msg)
{
    cJSON *obj = cJSON_CreateObject();

    cJSON_AddStringToObject(obj, "error", msg);
    return reply(conn, status, obj);
}

/* ---------- Обработка запросов ---------- */

/* Достаёт "city" из входящего JSON. Возвращает 1, если город корректный. */
static int read_city(const char *body, char *city, size_t cap)
{
    cJSON       *in = cJSON_Parse(body);
    const cJSON *c  = cJSON_GetObjectItemCaseSensitive(in, "city");
    int          ok = cJSON_IsString(c) && c->valuestring[0] != '\0'
                      && strlen(c->valuestring) < cap;

    if (ok) snprintf(city, cap, "%s", c->valuestring);
    cJSON_Delete(in);
    return ok;
}

static enum MHD_Result handle_weather(struct MHD_Connection *conn, const char *body)
{
    char          city[101];
    WeatherResult w;
    cJSON        *out;

    if (!read_city(body, city, sizeof city))
        return reply_error(conn, 400, "send JSON like {\"city\": \"Kyiv\"}");

    log_msg("INFO", "request: city=%s", city);

    switch (weather_lookup(city, &w)) {
    case WEATHER_OK:
        break;
    case WEATHER_NOT_FOUND:
        log_msg("WARN", "city not found: %s", city);
        return reply_error(conn, 404, "city not found");
    default:
        return reply_error(conn, 502, "weather service is unavailable");
    }

    log_msg("INFO", "answer: %s (%s), %.1f C, wind %.1f km/h",
            w.place, w.country, w.temperature_c, w.wind_kmh);

    out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "city", w.place);
    cJSON_AddStringToObject(out, "country", w.country);
    cJSON_AddNumberToObject(out, "temperature_c", w.temperature_c);
    cJSON_AddNumberToObject(out, "wind_kmh", w.wind_kmh);
    return reply(conn, 200, out);
}

/* Тело запроса приходит по кусочкам, поэтому для каждого запроса есть своя "коробка". */
typedef struct {
    char   data[MAX_BODY + 1];
    size_t len;
    int    too_big;
} Body;

/* Эту функцию microhttpd вызывает для каждого запроса (и не один раз). */
static enum MHD_Result handle(void *cls, struct MHD_Connection *conn,
                              const char *url, const char *method,
                              const char *version, const char *chunk,
                              size_t *chunk_size, void **req)
{
    Body *body = *req;

    (void)cls;
    (void)version;

    if (!body) {                       /* первый вызов: пришли только заголовки */
        body = calloc(1, sizeof *body);
        if (!body) return MHD_NO;
        *req = body;
        return MHD_YES;
    }
    if (*chunk_size > 0) {             /* пришёл кусок тела: копим */
        if (body->len + *chunk_size <= MAX_BODY) {
            memcpy(body->data + body->len, chunk, *chunk_size);
            body->len += *chunk_size;
        } else {
            body->too_big = 1;
        }
        *chunk_size = 0;
        return MHD_YES;
    }

    /* Запрос получен целиком, можно отвечать. */
    if (strcmp(url, "/health") == 0 && strcmp(method, "GET") == 0) {
        cJSON *ok = cJSON_CreateObject();
        cJSON_AddStringToObject(ok, "status", "ok");
        return reply(conn, 200, ok);
    }
    if (strcmp(url, "/weather") == 0) {
        if (strcmp(method, "POST") != 0) return reply_error(conn, 405, "use POST");
        if (body->too_big)               return reply_error(conn, 413, "request is too big");
        return handle_weather(conn, body->data);
    }
    return reply_error(conn, 404, "not found");
}

/* Вызывается, когда запрос закончен: убираем за собой. */
static void request_done(void *cls, struct MHD_Connection *conn, void **req,
                         enum MHD_RequestTerminationCode why)
{
    (void)cls; (void)conn; (void)why;
    free(*req);
    *req = NULL;
}

/* Свои сообщения об ошибках (например, «порт занят») microhttpd передаёт сюда,
   а мы кладём их в общий журнал. Без этого причина ошибки терялась. */
static void on_mhd_log(void *cls, const char *fmt, va_list ap)
{
    Server *s = cls;
    char    msg[256];
    size_t  n;

    /* При остановке библиотека сообщает об оборванных подключениях:
       это ожидаемо, журнал этим не засоряем. */
    if (atomic_load(&s->stopping)) return;
    vsnprintf(msg, sizeof msg, fmt, ap);
    n = strlen(msg);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == ' ')) msg[--n] = '\0';
    log_msg("WARN", "http: %s", msg);
}

/* ---------- Открытый интерфейс (то, что объявлено в server.h) ---------- */

Server *server_start(int port)
{
    Server *s = calloc(1, sizeof *s);

    if (!s) return NULL;
    atomic_init(&s->stopping, 0);
    /* Каждое подключение обслуживается в своём потоке: пока один запрос ждёт
       погодный сайт (до 20 секунд), остальные получают ответ сразу. */
    s->daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_AUTO_INTERNAL_THREAD
                                 | MHD_USE_ERROR_LOG,
                                 (uint16_t)port,
                                 NULL, NULL, &handle, NULL,
                                 MHD_OPTION_EXTERNAL_LOGGER, &on_mhd_log, s,      /* первым, по правилам */
                                 MHD_OPTION_NOTIFY_COMPLETED, &request_done, NULL,
                                 MHD_OPTION_CONNECTION_TIMEOUT, 30u,
                                 MHD_OPTION_CONNECTION_LIMIT, MAX_CONNECTIONS,
                                 MHD_OPTION_END);
    if (!s->daemon) {
        log_msg("ERROR", "cannot start server on port %d", port);
        free(s);
        return NULL;
    }
    return s;
}

int server_port(const Server *s)
{
    const union MHD_DaemonInfo *info;

    if (!s) return 0;
    info = MHD_get_daemon_info(s->daemon, MHD_DAEMON_INFO_BIND_PORT);
    return info ? info->port : 0;
}

void server_stop(Server *s)
{
    if (!s) return;
    atomic_store(&s->stopping, 1);
    MHD_stop_daemon(s->daemon);
    free(s);
}
