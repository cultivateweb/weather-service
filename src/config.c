#include "config.h"          /* свой заголовок подключаем первым: так видно, что он самодостаточен */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const CONFIG_KEYS[] = {
    "PORT", "DB_PATH", "LOG_PATH", "WEATHER_URL", "GEO_URL", "GEO_LANGUAGE"
};

/* Убирает пробелы по краям строки (меняет строку на месте). */
static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

/* Записывает одну настройку. Возвращает 0, если всё в порядке. */
static int config_set(Config *c, const char *key, const char *val)
{
    char *dst;

    if (strcmp(key, "PORT") == 0) {
        char *end;
        long  port;

        errno = 0;
        port  = strtol(val, &end, 10);
        /* end показывает, где число кончилось: после него ничего быть не должно,
           иначе «8080abc» молча превратилось бы в 8080. */
        if (end == val || *end != '\0' || errno == ERANGE || port < 1 || port > 65535)
            return -1;
        c->port = (int)port;
        return 0;
    }
    if (strcmp(key, "GEO_LANGUAGE") == 0) {
        /* Ровно две строчные латинские буквы: uk, ru, en... Значение идёт
           прямо в адрес запроса, поэтому лишнего в нём быть не должно. */
        if (strlen(val) != 2 || val[0] < 'a' || val[0] > 'z' || val[1] < 'a' || val[1] > 'z')
            return -1;
        memcpy(c->geo_language, val, 3);
        return 0;
    }
    if      (strcmp(key, "DB_PATH") == 0)     dst = c->db_path;
    else if (strcmp(key, "LOG_PATH") == 0)    dst = c->log_path;
    else if (strcmp(key, "WEATHER_URL") == 0) dst = c->weather_url;
    else if (strcmp(key, "GEO_URL") == 0)     dst = c->geo_url;
    else return -1;                           /* незнакомый ключ */

    return snprintf(dst, CONFIG_STR_MAX, "%s", val) < CONFIG_STR_MAX ? 0 : -1;
}

static int is_known_key(const char *key)
{
    size_t i;

    for (i = 0; i < sizeof CONFIG_KEYS / sizeof *CONFIG_KEYS; i++)
        if (strcmp(key, CONFIG_KEYS[i]) == 0) return 1;
    return 0;
}

/* Читает строки вида КЛЮЧ=значение. Пустые строки и строки с # пропускает.
   Опечатку не пропускает молча: иначе PROT=9000 тихо оставил бы порт 8080. */
static int read_file(Config *c, FILE *f, const char *path)
{
    char line[512];
    int  n = 0;                                  /* номер строки: для сообщения об ошибке */

    while (fgets(line, sizeof line, f)) {
        char *key, *eq;

        n++;
        /* Строка не влезла в буфер: её хвост прочитался бы как отдельная строка. */
        if (!strchr(line, '\n') && !feof(f)) {
            fprintf(stderr, "config: %s line %d: the line is too long\n", path, n);
            return -1;
        }
        key = trim(line);
        if (*key == '\0' || *key == '#') continue;

        eq = strchr(key, '=');
        if (!eq) {
            fprintf(stderr, "config: %s line %d: expected KEY=value, got \"%s\"\n", path, n, key);
            return -1;
        }
        *eq = '\0';
        key = trim(key);
        if (!is_known_key(key)) {
            fprintf(stderr, "config: %s line %d: unknown setting %s\n", path, n, key);
            return -1;
        }
        if (config_set(c, key, trim(eq + 1)) != 0) {
            fprintf(stderr, "config: %s line %d: bad value for %s\n", path, n, key);
            return -1;
        }
    }
    return 0;
}

int config_load(Config *c, const char *path)
{
    FILE  *f;
    size_t i;

    /* Значения по умолчанию. */
    c->port = 8080;
    snprintf(c->db_path, CONFIG_STR_MAX, "service.db");
    snprintf(c->log_path, CONFIG_STR_MAX, "service.log");
    snprintf(c->weather_url, CONFIG_STR_MAX, "https://api.open-meteo.com/v1/forecast");
    snprintf(c->geo_url, CONFIG_STR_MAX, "https://geocoding-api.open-meteo.com/v1/search");
    snprintf(c->geo_language, sizeof c->geo_language, "uk");

    /* 1) Файл. Если его нет, работаем на значениях по умолчанию. */
    f = fopen(path, "r");
    if (f) {
        int rc = read_file(c, f, path);

        fclose(f);
        if (rc != 0) return -1;
    }

    /* 2) Переменные окружения важнее файла (удобно для облака). */
    for (i = 0; i < sizeof CONFIG_KEYS / sizeof *CONFIG_KEYS; i++) {
        const char *v = getenv(CONFIG_KEYS[i]);

        if (v && config_set(c, CONFIG_KEYS[i], v) != 0) {
            fprintf(stderr, "config: bad value in variable %s\n", CONFIG_KEYS[i]);
            return -1;
        }
    }
    return 0;
}
