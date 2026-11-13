/*
 * Тест программы целиком: запускаем weather_service отдельным процессом,
 * как человек в терминале, и смотрим код выхода и что она пишет на экран.
 * Аргументы (их передаёт CMake): путь к программе и путь к config.env.
 */
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#define OUT_FILE "test_main.out"    /* сюда попадает всё, что программа пишет на экран */

static int         failures;
static const char *g_bin;           /* путь к weather_service */

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void pause_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&t, NULL);
}

/* Открывает слушающий сокет на свободном порту: так порт становится занятым.
   Номер порта — в *port. */
static int listen_any(int *port)
{
    struct sockaddr_in a;
    socklen_t          len = sizeof a;
    int                fd  = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0
        || getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

/* Запускает программу с файлом настроек cfg на порту port. */
static pid_t start(const char *cfg, int port)
{
    pid_t pid;

    fflush(NULL);
    pid = fork();
    if (pid == 0) {                                   /* это уже новый процесс */
        char p[16];
        int  fd = open(OUT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        snprintf(p, sizeof p, "%d", port);
        setenv("PORT", p, 1);
        setenv("LOG_PATH", "test_main.log", 1);
        setenv("DB_PATH", "test_main.db", 1);
        execl(g_bin, g_bin, cfg, (char *)NULL);
        _exit(127);                                   /* сюда попадаем, только если запуск не удался */
    }
    return pid;
}

/* Ждёт конца программы, но не дольше 10 секунд. Возвращает код выхода;
   -1 — процесса нет или его убил сигнал; -2 — не закончилась вовремя
   (тогда её убиваем сами, чтобы тест не завис). */
static int exit_code(pid_t pid)
{
    int status, i;

    if (pid <= 0) return -1;
    for (i = 0; i < 100; i++) {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0) return -1;
        pause_ms(100);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -2;
}

static char *read_out(void)
{
    FILE *f = fopen(OUT_FILE, "rb");
    char *buf;
    size_t got;

    if (!f) return NULL;
    buf = calloc(1, 65536);
    got = buf ? fread(buf, 1, 65535, f) : 0;
    if (buf) buf[got] = '\0';
    fclose(f);
    return buf;
}

static int out_has(const char *needle)
{
    char *text = read_out();
    int   ok   = text && strstr(text, needle);

    free(text);
    return ok;
}

static size_t discard(char *p, size_t size, size_t count, void *userp)
{
    (void)p; (void)userp;
    return size * count;
}

static int health_ok(int port)
{
    char  url[64];
    long  status = 0;
    CURL *c = curl_easy_init();

    if (!c) return 0;
    snprintf(url, sizeof url, "http://127.0.0.1:%d/health", port);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 2L);
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    return status == 200;
}

/* Ждёт, пока программа запустится и начнёт отвечать (до 5 секунд). */
static int wait_started(int port)
{
    int i;

    for (i = 0; i < 50; i++) {
        if (out_has("started on port") && health_ok(port)) return 1;
        pause_ms(100);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *cfg;
    int         port = 0, fd;
    pid_t       pid;

    if (argc != 3) {
        fprintf(stderr, "usage: test_main <weather_service> <config.env>\n");
        return 2;
    }
    g_bin = argv[1];
    cfg   = argv[2];
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* 1) Обычный запуск и остановка сигналом SIGTERM (так останавливают облако и Docker). */
    fd = listen_any(&port);           /* узнаём номер свободного порта... */
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);           /* ...и сразу отдаём его программе */
    pid = start(cfg, port);
    CHECK(pid > 0);
    CHECK(wait_started(port));
    if (pid > 0) kill(pid, SIGTERM);
    CHECK(exit_code(pid) == 0);
    CHECK(out_has("stopped"));

    /* 2) Порт уже занят: программа должна выйти с ошибкой. */
    fd = listen_any(&port);
    CHECK(fd >= 0);
    pid = start(cfg, port);
    CHECK(exit_code(pid) == 1);
    CHECK(out_has("cannot start server"));
    CHECK(out_has("Address already in use"));        /* сказано, почему не запустился */
    if (fd >= 0) close(fd);

    /* 3) Файл настроек указан явно, но его нет (опечатка в имени): ошибка. */
    pid = start("no-such-config.env", port);
    CHECK(exit_code(pid) == 1);
    CHECK(out_has("no-such-config.env"));

    curl_global_cleanup();
    if (failures) {
        char *text = read_out();

        fprintf(stderr, "--- last program output ---\n%s", text ? text : "");
        free(text);
        return 1;
    }
    remove(OUT_FILE);
    remove("test_main.log");
    remove("test_main.db");
    puts("main: ok");
    return 0;
}
