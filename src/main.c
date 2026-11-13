/*
 * main.c только собирает модули вместе: запускает их по порядку
 * и останавливает в обратном порядке. Никакой логики сервиса здесь нет.
 */
#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "logger.h"
#include "server.h"
#include "weather.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    Config      cfg;
    Server     *server = NULL;
    int         rc = 1;
    const char *cfg_path = argc > 1 ? argv[1] : "config.env";

    if (config_load(&cfg, cfg_path) != 0) return 1;
    if (log_open(cfg.log_path, cfg.db_path) != 0) return 1;

    if (weather_init(cfg.geo_url, cfg.weather_url, cfg.geo_language) != 0) goto done;
    server = server_start(cfg.port);
    if (!server) goto done;
    log_msg("INFO", "started on port %d", cfg.port);

    signal(SIGINT, on_signal);         /* Ctrl+C */
    signal(SIGTERM, on_signal);        /* "вежливая" остановка из облака или Docker */
    while (!g_stop) sleep(1);
    rc = 0;

done:                                  /* единое место очистки при любом выходе */
    weather_cancel_all();              /* иначе остановка ждала бы медленный погодный сайт */
    server_stop(server);
    log_msg("INFO", "stopped");
    weather_cleanup();
    log_close();
    return rc;
}
