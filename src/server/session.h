#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include <arpa/inet.h>

#define INACTIVITY_SEC 60

/* Lo que main le pasa a cada hilo de cliente. */
typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
} ClientArg;

/* Pone la tabla de clientes en ceros. Llamar antes de aceptar conexiones. */
void session_init(void);

/* Hilo global: cada 10s marca INACTIVO a los clientes sin actividad reciente. */
void *session_inactivity_thread(void *arg);

/* Hilo por cliente: lee mensajes del socket y despacha a los handlers. */
void *session_client_thread(void *arg);

#endif
