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
    unsetenv("DB_URL");
    unsetenv("LOG_PATH");
    unsetenv("WEATHER_URL");
    unsetenv("GEO_URL");
    unsetenv("GEO_LANGUAGE");

    /* Файла нет, и он не обязателен: остаются значения по умолчанию. */
    CHECK(config_load(&c, "no-such-file.env", 0) == 0);
    CHECK(c.port == 8080);
    CHECK(strcmp(c.geo_language, "uk") == 0);
    CHECK(c.db_url[0] == '\0');                                    /* без базы */
    CHECK(strcmp(c.log_path, "service.log") == 0);

    /* Файла нет, а его указали явно (опечатка в имени): ошибка. */
    CHECK(config_load(&c, "no-such-file.env", 1) != 0);

    /* Файл: комментарии пропускаются, пробелы вокруг убираются. */
    CHECK(write_file("test.env", "# comment\n  PORT = 9090 \nDB_URL=postgresql://w@db/w\n") == 0);
    CHECK(config_load(&c, "test.env", 1) == 0);
    CHECK(c.port == 9090);
    CHECK(strcmp(c.db_url, "postgresql://w@db/w") == 0);

    /* Переменная окружения важнее файла. */
    setenv("PORT", "7000", 1);
    CHECK(config_load(&c, "test.env", 1) == 0);
    CHECK(c.port == 7000);
    unsetenv("PORT");

    /* Плохое значение: ошибка. */
    CHECK(write_file("test.env", "PORT=abc\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);

    /* Порт: всё значение целиком — число от 1 до 65535. */
    CHECK(write_file("test.env", "PORT=8080abc\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "PORT=80.5\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "PORT=0\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "PORT=65536\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "PORT=99999999999999999999\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "PORT=65535\n") == 0);
    CHECK(config_load(&c, "test.env", 1) == 0);
    CHECK(c.port == 65535);

    /* Опечатка в файле — ошибка, а не тихий пропуск. */
    CHECK(write_file("test.env", "PROT=9000\n") == 0);                /* ключ с опечаткой */
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "PORT 9000\n") == 0);                /* забыли = */
    CHECK(config_load(&c, "test.env", 1) != 0);
    {
        char longline[700];                                          /* не влезает в буфер */

        memset(longline, 'x', sizeof longline);
        memcpy(longline, "# ", 2);
        longline[sizeof longline - 2] = '\n';
        longline[sizeof longline - 1] = '\0';
        CHECK(write_file("test.env", longline) == 0);
        CHECK(config_load(&c, "test.env", 1) != 0);
    }
    /* Пустые строки, пробелы и комментарии — можно. */
    CHECK(write_file("test.env", "\n# comment\n   \n\tPORT=9000\n") == 0);
    CHECK(config_load(&c, "test.env", 1) == 0);
    CHECK(c.port == 9000);

    /* Язык поиска: только две строчные латинские буквы. */
    CHECK(write_file("test.env", "GEO_LANGUAGE=ru\n") == 0);
    CHECK(config_load(&c, "test.env", 1) == 0);
    CHECK(strcmp(c.geo_language, "ru") == 0);
    CHECK(write_file("test.env", "GEO_LANGUAGE=russian\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "GEO_LANGUAGE=RU\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);
    CHECK(write_file("test.env", "GEO_LANGUAGE=u&\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);

    /* Старая настройка базы SQLite больше не существует: её не пропускаем молча. */
    CHECK(write_file("test.env", "DB_PATH=service.db\n") == 0);
    CHECK(config_load(&c, "test.env", 1) != 0);

    /* Пустое значение выключает файл журнала (так делают в Docker). */
    setenv("LOG_PATH", "", 1);
    CHECK(config_load(&c, "no-such-file.env", 0) == 0);
    CHECK(c.log_path[0] == '\0');
    unsetenv("LOG_PATH");

    remove("test.env");
    puts("config: ok");
    return 0;
}
