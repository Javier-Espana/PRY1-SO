#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/chat_protocol.h"

int net_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &saddr.sin_addr) <= 0) {
        fprintf(stderr, "IP invalida: %s\n", ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int net_send_line(int fd, const char *msg) {
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    return (int)send(fd, buf, strlen(buf), 0);
}
