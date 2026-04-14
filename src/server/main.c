#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "session.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto invalido\n");
        return 1;
    }

    /* Ignorar SIGPIPE: si un cliente cierra feo, send() devuelve EPIPE y seguimos. */
    signal(SIGPIPE, SIG_IGN);

    int server_fd = net_listen(port);
    if (server_fd < 0) {
        return 1;
    }

    printf("[SERVER] Escuchando en puerto %d\n", port);
    printf("[SERVER] Timeout de inactividad: %d segundos\n", INACTIVITY_SEC);

    session_init();

    pthread_t inactivity_tid;
    pthread_create(&inactivity_tid, NULL, session_inactivity_thread, NULL);
    pthread_detach(inactivity_tid);

    /* Loop principal: solo acepta. Toda la logica vive en el hilo por cliente. */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int cfd = accept(server_fd, (struct sockaddr *)&client_addr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        ClientArg *ca = malloc(sizeof(ClientArg));
        if (!ca) {
            close(cfd);
            continue;
        }
        ca->fd = cfd;
        inet_ntop(AF_INET, &client_addr.sin_addr, ca->ip, sizeof(ca->ip));

        printf("[SERVER] Nueva conexion desde %s\n", ca->ip);

        pthread_t tid;
        pthread_create(&tid, NULL, session_client_thread, ca);
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
