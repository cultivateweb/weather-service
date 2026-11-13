/* Тест модуля config. Сеть, база и сервер для него не нужны. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

/* Свой CHECK вместо assert: assert выключается в Release-сборке. */
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (!f) return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

int main(void)
{
    Config c;

    /* Тест не должен зависеть от переменных окружения машины. */
    unsetenv("PORT");
    unsetenv("DB_PATH");
    unsetenv("LOG_PATH");
    unsetenv("WEATHER_URL");
    unsetenv("GEO_URL");
    unsetenv("GEO_LANGUAGE");

    /* Файла нет: остаются значения по умолчанию. */
    CHECK(config_load(&c, "no-such-file.env") == 0);
    CHECK(c.port == 8080);
    CHECK(strcmp(c.geo_language, "uk") == 0);

    /* Файл: комментарии пропускаются, пробелы вокруг убираются. */
    CHECK(write_file("test.env", "# comment\n  PORT = 9090 \nDB_PATH=x.db\n") == 0);
    CHECK(config_load(&c, "test.env") == 0);
    CHECK(c.port == 9090);
    CHECK(strcmp(c.db_path, "x.db") == 0);

    /* Переменная окружения важнее файла. */
    setenv("PORT", "7000", 1);
    CHECK(config_load(&c, "test.env") == 0);
    CHECK(c.port == 7000);
    unsetenv("PORT");

    /* Плохое значение: ошибка. */
    CHECK(write_file("test.env", "PORT=abc\n") == 0);
    CHECK(config_load(&c, "test.env") != 0);

    /* Язык поиска: только две строчные латинские буквы. */
    CHECK(write_file("test.env", "GEO_LANGUAGE=ru\n") == 0);
    CHECK(config_load(&c, "test.env") == 0);
    CHECK(strcmp(c.geo_language, "ru") == 0);
    CHECK(write_file("test.env", "GEO_LANGUAGE=russian\n") == 0);
    CHECK(config_load(&c, "test.env") != 0);
    CHECK(write_file("test.env", "GEO_LANGUAGE=RU\n") == 0);
    CHECK(config_load(&c, "test.env") != 0);
    CHECK(write_file("test.env", "GEO_LANGUAGE=u&\n") == 0);
    CHECK(config_load(&c, "test.env") != 0);

    remove("test.env");
    puts("config: ok");
    return 0;
}
