#ifndef WEATHER_H
#define WEATHER_H

typedef struct {
    char   place[128];      /* название города, как его вернул геокодер */
    double temperature_c;
    double wind_kmh;
} WeatherResult;

typedef enum { WEATHER_OK, WEATHER_NOT_FOUND, WEATHER_ERROR } WeatherStatus;

/* Адреса API приходят параметрами: так модуль не зависит от config.h. */
int           weather_init(const char *geo_url, const char *weather_url);
WeatherStatus weather_lookup(const char *city, WeatherResult *out);
void          weather_cleanup(void);

#endif
