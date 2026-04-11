#include <stdio.h>
#include <stdlib.h>

#include "client_app.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <usuario> <IP_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    int server_port = atoi(argv[3]);
    return client_run(argv[1], argv[2], server_port);
}
