#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_STR_MAX 256

/* Настройки сервиса. Это простые данные без хитрой логики,
   поэтому структура открытая: любой модуль может читать поля. */
typedef struct {
    int  port;
    char db_path[CONFIG_STR_MAX];
    char log_path[CONFIG_STR_MAX];
    char weather_url[CONFIG_STR_MAX];
    char geo_url[CONFIG_STR_MAX];
} Config;

/* Сначала значения по умолчанию, потом файл, потом переменные окружения.
   Возвращает 0, если всё в порядке. */
int config_load(Config *c, const char *path);

#endif
