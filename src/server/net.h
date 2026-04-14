#ifndef SERVER_NET_H
#define SERVER_NET_H

/* Crea socket TCP, bind + listen en port. Retorna fd o -1. */
int net_listen(int port);

#endif
