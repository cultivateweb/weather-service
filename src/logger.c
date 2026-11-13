#include "logger.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <sqlite3.h>

/* static: эти переменные видны только в этом файле. */
static FILE    *g_file;
static sqlite3 *g_db;

/* Запросы обслуживаются в нескольких потоках, и писать в журнал они могут
   одновременно. Замок пускает к записи по одному: строки не перемешаются,
   а порядок записей в файле и в базе совпадёт. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int log_open(const char *file_path, const char *db_path)
{
    char *err = NULL;

    g_file = fopen(file_path, "a");
    if (!g_file) {
        perror(file_path);
        return -1;
    }
    if (sqlite3_open(db_path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "db: %s\n", sqlite3_errmsg(g_db));
        log_close();
        return -1;
    }
    if (sqlite3_exec(g_db,
            "CREATE TABLE IF NOT EXISTS logs ("
            "  id      INTEGER PRIMARY KEY,"
            "  ts      TEXT NOT NULL,"
            "  level   TEXT NOT NULL,"
            "  message TEXT NOT NULL)",
            NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db: %s\n", err);
        sqlite3_free(err);
        log_close();
        return -1;
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

    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);

    pthread_mutex_lock(&g_lock);

    fprintf(stderr, "%s [%s] %s\n", ts, level, msg);            /* на экран */

    if (g_file) {                                               /* в файл */
        fprintf(g_file, "%s [%s] %s\n", ts, level, msg);
        fflush(g_file);
    }

    if (g_db) {                                                 /* в базу */
        sqlite3_stmt *st = NULL;

        /* Значения подставляются через ?, а не клеятся в запрос:
           так их содержимое не может превратиться в команду SQL. */
        if (sqlite3_prepare_v2(g_db,
                "INSERT INTO logs (ts, level, message) VALUES (?, ?, ?)",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, ts, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, level, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, msg, -1, SQLITE_STATIC);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }

    pthread_mutex_unlock(&g_lock);
}

void log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_db)   { sqlite3_close(g_db); g_db = NULL; }
    if (g_file) { fclose(g_file);      g_file = NULL; }
    pthread_mutex_unlock(&g_lock);
}
