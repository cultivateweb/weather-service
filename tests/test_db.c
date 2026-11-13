/*
 * Журнал в PostgreSQL: строки доходят до базы ровно такими, какими были,
 * а журнал переживает перезапуск базы и время, пока её нет.
 *
 * Нужна настоящая база. Её адрес — в переменной TEST_DB_URL, например
 *   TEST_DB_URL=postgresql://weather@127.0.0.1:5432/weather
 * (пароль, если нужен, — в переменной PGPASSWORD). Без TEST_DB_URL тест
 * пропускается: возвращает 77, и ctest показывает его как Skipped.
 *
 * Тест заводит себе отдельную базу wtest_<номер процесса> и в конце удаляет её:
 * чужих данных он не трогает. Поэтому пользователю из TEST_DB_URL нужно право
 * создавать базы (у пользователя из compose.yaml оно есть).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libpq-fe.h>

#include "logger.h"

#define LOG_FILE "test_db.log"

static int     failures;
static PGconn *g_admin;      /* подключение к TEST_DB_URL: завести и удалить базу теста */
static PGconn *g_check;      /* подключение к базе теста: смотреть, что записал журнал */
static char    g_name[32];   /* имя базы теста */

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Выполняет команду SQL. 1 — получилось. */
static int sql(PGconn *c, const char *text)
{
    PGresult *r  = PQexec(c, text);
    int       ok = PQresultStatus(r) == PGRES_COMMAND_OK || PQresultStatus(r) == PGRES_TUPLES_OK;

    if (!ok) fprintf(stderr, "SQL failed: %s\n  %s", text, PQerrorMessage(c));
    PQclear(r);
    return ok;
}

/* То же, но имя базы теста подставляется вместо %s. */
static int sql_db(const char *fmt)
{
    char text[256];

    snprintf(text, sizeof text, fmt, g_name);
    return sql(g_admin, text);
}

/* Одно число из ответа базы; -1 — запрос не удался. */
static long number(PGconn *c, const char *text, int n, const char *const *params)
{
    PGresult *r   = PQexecParams(c, text, n, NULL, params, NULL, NULL, 0);
    long      val = -1;

    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
        val = atol(PQgetvalue(r, 0, 0));
    else
        fprintf(stderr, "SQL failed: %s\n  %s", text, PQerrorMessage(c));
    PQclear(r);
    return val;
}

/* Сколько в журнале строк с таким уровнем и таким текстом. */
static long rows(const char *level, const char *message)
{
    const char *p[2] = { level, message };

    return number(g_check, "SELECT count(*) FROM logs WHERE level = $1 AND message = $2", 2, p);
}

/* Сколько в журнале строк, которые начинаются с prefix. */
static long rows_starting(const char *prefix)
{
    const char *p[1] = { prefix };

    return number(g_check, "SELECT count(*) FROM logs WHERE starts_with(message, $1)", 1, p);
}

/* Обрывает подключение журнала к базе — как будто базу перезапустили.
   Возвращает, сколько подключений оборвано (ждём ровно одно). */
static long cut_logger_connection(void)
{
    char        pid[16];
    const char *p[2] = { g_name, pid };

    snprintf(pid, sizeof pid, "%d", PQbackendPID(g_check));    /* своё подключение не трогаем */
    return number(g_admin,
                  "SELECT count(*) FILTER (WHERE pg_terminate_backend(pid, 5000)) "
                  "FROM pg_stat_activity WHERE datname = $1 AND pid <> $2::int", 2, p);
}

/* Сколько раз текст встречается в файле журнала. */
static int file_count(const char *needle)
{
    static char buf[1 << 16];
    FILE       *f = fopen(LOG_FILE, "rb");
    size_t      got;
    int         n = 0;
    const char *p;

    if (!f) return -1;
    got = fread(buf, 1, sizeof buf - 1, f);
    buf[got] = '\0';
    fclose(f);
    for (p = strstr(buf, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

/* Адрес базы теста: всё как в TEST_DB_URL, только имя базы другое. */
static int make_url(const char *base, char *out, size_t cap)
{
    PQconninfoOption *opts = PQconninfoParse(base, NULL), *o;
    size_t            len  = 0;

    if (!opts) return -1;
    for (o = opts; o->keyword; o++) {
        const char *v = strcmp(o->keyword, "dbname") == 0 ? g_name : o->val;

        if (!v) continue;
        len += (size_t)snprintf(out + len, len < cap ? cap - len : 0, "%s='", o->keyword);
        for (; *v; v++) {                        /* ' и \ внутри значения экранируются */
            if (*v == '\'' || *v == '\\') { if (len < cap) out[len] = '\\'; len++; }
            if (len < cap) out[len] = *v;
            len++;
        }
        len += (size_t)snprintf(out + len, len < cap ? cap - len : 0, "' ");
    }
    PQconninfoFree(opts);
    return len < cap ? 0 : -1;
}

int main(void)
{
    const char *base = getenv("TEST_DB_URL");
    const char *keys[] = { "dbname", "application_name", NULL };
    const char *vals[] = { NULL, "wtest-check", NULL };
    char        url[1024], longtext[601];
    size_t      i;

    /* Строки, которые в неаккуратной программе стали бы командами SQL
       или испортили бы запись. В базе они должны лежать как есть. */
    static const char *const tricky[] = {
        "x'); DROP TABLE logs; --",
        "Robert'); DELETE FROM logs; --",
        "quotes ' \" and backslash \\",
        "placeholders $1 $2 %s",
        "Київ, Одеса, Львів",
    };

    if (!base || !*base) {
        puts("db: skipped, set TEST_DB_URL to run it");
        return 77;
    }
    g_admin = PQconnectdb(base);
    if (PQstatus(g_admin) != CONNECTION_OK) {
        fprintf(stderr, "cannot connect to TEST_DB_URL: %s", PQerrorMessage(g_admin));
        return 1;
    }
    snprintf(g_name, sizeof g_name, "wtest_%ld", (long)getpid());
    sql_db("DROP DATABASE IF EXISTS %s");       /* вдруг осталась от упавшего запуска */
    if (!sql_db("CREATE DATABASE %s") || make_url(base, url, sizeof url) != 0) {
        PQfinish(g_admin);
        return 1;
    }
    remove(LOG_FILE);

    /* 1) Первый запуск: таблица logs появляется сама. */
    CHECK(log_open(LOG_FILE, url) == 0);
    vals[0] = url;
    g_check = PQconnectdbParams(keys, vals, 1);
    CHECK(PQstatus(g_check) == CONNECTION_OK);

    /* 2) Хитрые строки лежат в базе как есть, а таблица цела. */
    for (i = 0; i < sizeof tricky / sizeof *tricky; i++)
        log_msg("INFO", "%s", tricky[i]);
    for (i = 0; i < sizeof tricky / sizeof *tricky; i++)
        CHECK(rows("INFO", tricky[i]) == 1);

    /* 3) Байты, которые не складываются в буквы UTF-8, база не принимает.
          Журнал заменяет их на «?», и запись не пропадает. */
    log_msg("WARN", "bad bytes: %s end", "\xff\xfe");
    CHECK(rows("WARN", "bad bytes: ?? end") == 1);
    /* Длинная строка обрезается до 511 байт — посреди двухбайтовой буквы «ж». */
    for (i = 0; i < 300; i++) memcpy(longtext + 2 * i, "ж", 2);
    longtext[600] = '\0';
    log_msg("INFO", "%s", longtext);
    longtext[510] = '?';
    longtext[511] = '\0';
    CHECK(rows("INFO", longtext) == 1);

    /* 4) Базу перезапустили, подключение оборвалось: журнал переподключается сам. */
    CHECK(cut_logger_connection() == 1);
    log_msg("INFO", "after reconnect");
    CHECK(rows("INFO", "after reconnect") == 1);
    CHECK(rows("INFO", "database: reconnected") == 1);

    /* 5) Базы нет совсем: строки идут только в файл. Когда база вернулась,
          журнал снова пишет в неё (пробует раз в LOG_DB_RETRY_SECONDS). */
    CHECK(sql_db("ALTER DATABASE %s ALLOW_CONNECTIONS false"));   /* новых подключений не будет */
    CHECK(cut_logger_connection() == 1);
    log_msg("INFO", "during outage 1");       /* переподключиться не вышло */
    log_msg("INFO", "during outage 2");       /* рано пробовать снова: только файл */
    CHECK(sql_db("ALTER DATABASE %s ALLOW_CONNECTIONS true"));
    sleep(LOG_DB_RETRY_SECONDS + 1);
    log_msg("INFO", "after outage");
    CHECK(rows("INFO", "during outage 1") == 0);
    CHECK(rows("INFO", "during outage 2") == 0);
    CHECK(rows("INFO", "after outage") == 1);
    CHECK(rows_starting("database: connection restored") == 1);
    CHECK(file_count("during outage 2") == 1);
    CHECK(file_count("database is unavailable") == 1);   /* об ошибке сказано один раз */

    /* 6) Связь есть, а запись не прошла (таблицу удалили): ошибка видна в файле. */
    CHECK(sql(g_check, "DROP TABLE logs"));
    log_msg("INFO", "no table");
    CHECK(file_count("database: cannot write") == 1);
    log_close();

    /* 7) Повторный запуск, когда таблица уже есть: не ошибка, записи на месте. */
    CHECK(log_open("", url) == 0);                 /* таблица снова создаётся */
    log_msg("INFO", "first run");
    log_close();
    CHECK(log_open("", url) == 0);                 /* а теперь она уже есть */
    log_close();
    CHECK(rows("INFO", "first run") == 1);

    PQfinish(g_check);
    CHECK(sql_db("DROP DATABASE %s WITH (FORCE)"));
    PQfinish(g_admin);
    if (failures) return 1;
    remove(LOG_FILE);
    puts("db: ok");
    return 0;
}
