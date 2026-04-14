#ifndef CLIENT_SESSION_H
#define CLIENT_SESSION_H

/* Conecta, registra usuario, corre UI y recv thread hasta /exit o desconexion. */
int client_session_run(const char *user, const char *server_ip, int server_port);

#endif
