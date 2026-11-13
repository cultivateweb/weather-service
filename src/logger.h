#ifndef LOGGER_H
#define LOGGER_H

/*
 * Журнал: каждая запись попадает в файл, в таблицу logs базы SQLite
 * и на экран. Всё внутреннее состояние спрятано в logger.c.
 */

/* Просит компилятор проверять формат, как у printf (работает в gcc и clang). */
#if defined(__GNUC__)
#define LOG_PRINTF(fmt_arg, first_arg) __attribute__((format(printf, fmt_arg, first_arg)))
#else
#define LOG_PRINTF(fmt_arg, first_arg)
#endif

int  log_open(const char *file_path, const char *db_path);   /* 0 = успех */
void log_msg(const char *level, const char *fmt, ...) LOG_PRINTF(2, 3);
void log_close(void);

#endif
