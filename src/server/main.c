#include <stdio.h>
#include <stdlib.h>

#include "server_app.h"

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

    return server_run(port);
}
