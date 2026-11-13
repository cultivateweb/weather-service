#ifndef SERVER_H
#define SERVER_H

/* "Закрытый" тип: что у него внутри, знает только server.c.
   Поэтому остальные файлы не подключают microhttpd.h. */
typedef struct Server Server;

Server *server_start(int port);   /* NULL, если запустить не удалось */
void    server_stop(Server *s);

#endif
