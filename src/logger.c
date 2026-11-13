#include "logger.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <libpq-fe.h>

/* static: эти переменные видны только в этом файле. */
static FILE   *g_file;
static PGconn *g_db;
static time_t  g_db_retry_at;   /* 0 — связь с базой в порядке; иначе — когда пробовать снова */

/* Запросы обслуживаются в нескольких потоках, и писать в журнал они могут
   одновременно. Замок пускает к записи по одному: строки не перемешаются,
   а порядок записей в файле и в базе совпадёт. Подключением к базе тоже
   пользуются только под замком: делить его между потоками libpq не умеет. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Заменяет на «?» байты, которые не складываются в буквы UTF-8. Такие байты
   приходят с запросами или остаются, когда длинная строка обрезана посреди
   буквы. PostgreSQL строку с ними не примет, и запись пропала бы из базы. */
static void fix_utf8(char *text)
{
    unsigned char *s = (unsigned char *)text;

    while (*s) {
        unsigned char lo = 0x80, hi = 0xBF;     /* каким может быть второй байт */
        int           n, i;

        if (*s < 0x80)                  { s++; continue; }          /* обычный символ ASCII */
        if (*s >= 0xC2 && *s <= 0xDF)   n = 1;
        else if (*s >= 0xE0 && *s <= 0xEF) {
            n = 2;
            if (*s == 0xE0) lo = 0xA0;          /* иначе та же буква записана длиннее, чем нужно */
            if (*s == 0xED) hi = 0x9F;          /* половинки UTF-16: буквами не считаются */
        } else if (*s >= 0xF0 && *s <= 0xF4) {
            n = 3;
            if (*s == 0xF0) lo = 0x90;
            if (*s == 0xF4) hi = 0x8F;          /* дальше U+10FFFF букв нет */
        } else {
            *s++ = '?';
            continue;
        }
        for (i = 1; i <= n; i++) {
            unsigned char b = s[i];

            if (i == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xBF)) break;
        }
        if (i <= n) *s++ = '?';                 /* буква оборвана: заменяем её первый байт */
        else s += n + 1;
    }
}

/* Пишет строку на экран и в файл, но не в базу. Вызывать под замком. */
static void write_local(const char *ts, const char *level, const char *msg)
{
    fprintf(stderr, "%s [%s] %s\n", ts, level, msg);
    if (g_file) {
        fprintf(g_file, "%s [%s] %s\n", ts, level, msg);
        fflush(g_file);
    }
}

/* Последняя ошибка libpq одной строкой (libpq заканчивает её переводом строки). */
static const char *db_error(char *buf, size_t cap)
{
    char *p;

    snprintf(buf, cap, "%s", PQerrorMessage(g_db));
    for (p = buf; *p; p++)
        if ((unsigned char)*p < 0x20) *p = ' ';
    while (p > buf && p[-1] == ' ') *--p = '\0';
    return buf;
}

/* Одна запись в базу. Значения передаются отдельно от текста запроса
   ($1, $2, $3), поэтому их содержимое не может стать командой SQL. */
static int db_insert(const char *ts, const char *level, const char *msg)
{
    const char *values[3] = { ts, level, msg };
    PGresult   *res = PQexecParams(g_db,
                                   "INSERT INTO logs (ts, level, message) VALUES ($1, $2, $3)",
                                   3, NULL, values, NULL, NULL, 0);
    int         ok  = PQresultStatus(res) == PGRES_COMMAND_OK;

    PQclear(res);
    return ok;
}

/* Строка, которую журнал пишет о себе сам: на экран, в файл и в базу. */
static void db_note(const char *ts, const char *msg)
{
    write_local(ts, "INFO", msg);
    db_insert(ts, "INFO", msg);
}

/* Запись в базу. Вызывать под замком. Если связь оборвалась (например, базу
   перезапустили), журнал переподключается. Пока базы нет, строки идут только
   на экран и в файл, а переподключиться он пробует раз в LOG_DB_RETRY_SECONDS:
   иначе каждая строка ждала бы недоступную базу. */
static void db_write(const char *ts, const char *level, const char *msg)
{
    char   err[256], line[320];
    time_t now = time(NULL);

    if (!g_db) return;
    if (g_db_retry_at) {                          /* базы не было */
        if (now < g_db_retry_at) return;          /* пробовать ещё рано */
        PQreset(g_db);
        if (PQstatus(g_db) != CONNECTION_OK) {
            g_db_retry_at = now + LOG_DB_RETRY_SECONDS;
            return;
        }
        g_db_retry_at = 0;
        db_note(ts, "database: connection restored, lines from the outage are not in it");
    }
    if (db_insert(ts, level, msg)) return;

    if (PQstatus(g_db) == CONNECTION_BAD) {       /* связь оборвалась: переподключаемся сразу */
        PQreset(g_db);
        if (PQstatus(g_db) == CONNECTION_OK) {
            db_note(ts, "database: reconnected");
            if (db_insert(ts, level, msg)) return;
        } else {
            g_db_retry_at = now + LOG_DB_RETRY_SECONDS;
            snprintf(line, sizeof line, "database is unavailable, the journal goes on without it: %s",
                     db_error(err, sizeof err));
            write_local(ts, "ERROR", line);
            return;
        }
    }
    snprintf(line, sizeof line, "database: cannot write: %s", db_error(err, sizeof err));
    write_local(ts, "ERROR", line);
}

/* Сообщения сервера вроде «таблица уже есть, пропускаю» libpq выводит на экран.
   Они ни о чём плохом не говорят, поэтому молчим. */
static void ignore_notice(void *arg, const char *message)
{
    (void)arg;
    (void)message;
}

int log_open(const char *file_path, const char *db_url)
{
    if (file_path[0]) {
        g_file = fopen(file_path, "a");
        if (!g_file) {
            perror(file_path);
            return -1;
        }
    }
    if (db_url[0]) {
        /* Ключи перед dbname — значения по умолчанию: если в адресе DB_URL
           они указаны, побеждает адрес. connect_timeout — сколько секунд ждать
           базу при подключении (без него libpq могла бы ждать бесконечно). */
        const char *keys[] = { "connect_timeout", "fallback_application_name", "dbname", NULL };
        const char *vals[] = { "5", "weather-service", db_url, NULL };
        char        err[256];
        PGresult   *res;

        g_db = PQconnectdbParams(keys, vals, 1);
        if (PQstatus(g_db) != CONNECTION_OK) {
            fprintf(stderr, "database: cannot connect: %s\n", db_error(err, sizeof err));
            log_close();
            return -1;
        }
        PQsetNoticeProcessor(g_db, ignore_notice, NULL);
        res = PQexec(g_db,
                     "CREATE TABLE IF NOT EXISTS logs ("
                     "  id      BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
                     "  ts      TIMESTAMPTZ NOT NULL,"
                     "  level   TEXT NOT NULL,"
                     "  message TEXT NOT NULL)");
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            fprintf(stderr, "database: cannot create table logs: %s\n", db_error(err, sizeof err));
            PQclear(res);
            log_close();
            return -1;
        }
        PQclear(res);
        log_msg("INFO", "database: the journal goes to %s on %s:%s",
                PQdb(g_db), PQhost(g_db), PQport(g_db));
    } else {
        log_msg("INFO", "database: DB_URL is empty, the journal goes only to the screen%s",
                g_file ? " and the file" : "");
    }
    return 0;
}

void log_msg(const char *level, const char *fmt, ...)
{
    char      msg[512], ts[32], *p;
    time_t    now = time(NULL);
    struct tm tm;
    va_list   ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* Перевод строки в сообщении мог бы подделать запись в журнале. */
    for (p = msg; *p; p++)
        if ((unsigned char)*p < 0x20) *p = ' ';
    fix_utf8(msg);

    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);

    pthread_mutex_lock(&g_lock);
    write_local(ts, level, msg);                /* на экран и в файл */
    db_write(ts, level, msg);                   /* в базу */
    pthread_mutex_unlock(&g_lock);
}

void log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_db)   { PQfinish(g_db); g_db = NULL; }
    if (g_file) { fclose(g_file); g_file = NULL; }
    g_db_retry_at = 0;
    pthread_mutex_unlock(&g_lock);
}
