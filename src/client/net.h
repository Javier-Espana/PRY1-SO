#ifndef CLIENT_NET_H
#define CLIENT_NET_H

/* Conecta por TCP. Retorna fd o -1. */
int net_connect(const char *ip, int port);

/* Envia msg + '\n'. Retorna bytes enviados o -1. */
int net_send_line(int fd, const char *msg);

#endif
