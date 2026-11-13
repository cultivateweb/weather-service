#ifndef SERVER_H
#define SERVER_H

/* "Закрытый" тип: что у него внутри, знает только server.c.
   Поэтому остальные файлы не подключают microhttpd.h. */
typedef struct Server Server;

Server *server_start(int port);   /* NULL, если запустить не удалось; 0 = любой свободный порт */
int     server_port(const Server *s);   /* на каком порту слушает (нужно тестам при порте 0) */
void    server_stop(Server *s);

#endif
