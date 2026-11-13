#ifndef LOGGER_H
#define LOGGER_H

/*
 * Журнал: каждая запись попадает на экран, в файл и в таблицу logs базы
 * PostgreSQL. Файл и база — по желанию. Всё внутреннее состояние спрятано в logger.c.
 */

/* Если база перестала отвечать, журнал пробует переподключиться не чаще,
   чем раз в столько секунд. До тех пор строки идут только на экран и в файл. */
#define LOG_DB_RETRY_SECONDS 5

/* Просит компилятор проверять формат, как у printf (работает в gcc и clang). */
#if defined(__GNUC__)
#define LOG_PRINTF(fmt_arg, first_arg) __attribute__((format(printf, fmt_arg, first_arg)))
#else
#define LOG_PRINTF(fmt_arg, first_arg)
#endif

/* file_path — файл журнала, db_url — адрес базы PostgreSQL.
   Пустая строка — туда не писать. Возвращает 0, если всё открылось. */
int  log_open(const char *file_path, const char *db_url);
void log_msg(const char *level, const char *fmt, ...) LOG_PRINTF(2, 3);
void log_close(void);

#endif
