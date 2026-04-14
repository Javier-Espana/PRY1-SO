#include "session.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/chat_protocol.h"
#include "../common/protocol.h"
#include "net.h"
#include "ui.h"

static int sock_fd = -1;
static char username[MAX_NAME];
static char my_status[16] = STATUS_ACTIVE;
static volatile int running = 1;

/* Imprime la lista de comandos en el log. */
static void show_help(void) {
    ui_add_log("--- AYUDA ------------------------------------------", COL_SERVER);
    ui_add_log("  <mensaje>            -> enviar al chat general", COL_SERVER);
    ui_add_log("  /dm <usuario> <msg>  -> mensaje directo privado", COL_SERVER);
    ui_add_log("  /list                -> listar usuarios conectados", COL_SERVER);
    ui_add_log("  /info <usuario>      -> ver IP y estado de usuario", COL_SERVER);
    ui_add_log("  /status <estado>     -> ACTIVO | OCUPADO | INACTIVO", COL_SERVER);
    ui_add_log("  /help                -> mostrar esta ayuda", COL_SERVER);
    ui_add_log("  /exit                -> salir del chat", COL_SERVER);
    ui_add_log("----------------------------------------------------", COL_SERVER);
    ui_redraw_chat();
}

/*
 * Decodifica un mensaje del servidor y lo refleja en la UI.
 * Tipos esperados (fields[0]) y su layout:
 *   OK|mensaje                       ERROR|codigo|mensaje
 *   SERVER_BROADCAST|remitente|texto SERVER_DIRECT|remitente|texto
 *   USER_LIST|n|user:estado|...      USER_INFO|usuario|ip|estado
 *   STATUS_UPDATE|usuario|estado     FORCED_STATUS|usuario|estado
 *   USER_JOINED|usuario|ip           USER_LEFT|usuario
 */
static void process_server_msg(const char *raw) {
    char buf[BUF_SIZE];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *fields[16];
    int nf = proto_tokenize(buf, fields, 16);
    if (nf == 0) {
        return;
    }

    char line[512];

    if (strcmp(fields[0], T_OK) == 0) {
        /* OK no se muestra: el envio ya se refleja por el broadcast/DM que regresa. */
        return;
    } else if (strcmp(fields[0], T_ERROR) == 0) {
        snprintf(line, sizeof(line), "[ERROR %s] %s", nf > 1 ? fields[1] : "?", nf > 2 ? fields[2] : "");
        ui_add_log(line, COL_ERROR);
    } else if (strcmp(fields[0], T_SRV_BROADCAST) == 0 && nf >= 3) {
        int is_me = strcmp(fields[1], username) == 0;
        snprintf(line, sizeof(line), "[%s] %s", fields[1], fields[2]);
        ui_add_log(line, is_me ? COL_MY_MSG : COL_NORMAL);
    } else if (strcmp(fields[0], T_SRV_DIRECT) == 0 && nf >= 3) {
        snprintf(line, sizeof(line), "[DM de %s] %s", fields[1], fields[2]);
        ui_add_log(line, COL_DM);
    } else if (strcmp(fields[0], T_USER_LIST) == 0) {
        int cnt = nf > 1 ? atoi(fields[1]) : 0;
        snprintf(line, sizeof(line), "--- Usuarios conectados (%d) ---", cnt);
        ui_add_log(line, COL_STATUS);
        for (int i = 2; i < nf; i++) {
            snprintf(line, sizeof(line), "  - %s", fields[i]);
            ui_add_log(line, COL_STATUS);
        }
    } else if (strcmp(fields[0], T_USER_INFO) == 0 && nf >= 4) {
        snprintf(line, sizeof(line), "%s  IP: %s  Estado: %s", fields[1], fields[2], fields[3]);
        ui_add_log(line, COL_STATUS);
    } else if (strcmp(fields[0], T_STATUS_UPDATE) == 0 && nf >= 3) {
        strncpy(my_status, fields[2], sizeof(my_status) - 1);
        my_status[sizeof(my_status) - 1] = '\0';
        snprintf(line, sizeof(line), "Estado cambiado a %s", fields[2]);
        ui_add_log(line, COL_SERVER);
        ui_redraw_status(username, my_status);
    } else if (strcmp(fields[0], T_FORCED_STATUS) == 0 && nf >= 3) {
        strncpy(my_status, fields[2], sizeof(my_status) - 1);
        my_status[sizeof(my_status) - 1] = '\0';
        snprintf(line, sizeof(line), "Estado cambiado a %s por inactividad", fields[2]);
        ui_add_log(line, COL_STATUS);
        ui_redraw_status(username, my_status);
    } else if (strcmp(fields[0], T_USER_JOINED) == 0 && nf >= 2) {
        snprintf(line, sizeof(line), "%s se conecto", fields[1]);
        ui_add_log(line, COL_SERVER);
    } else if (strcmp(fields[0], T_USER_LEFT) == 0 && nf >= 2) {
        snprintf(line, sizeof(line), "%s se desconecto", fields[1]);
        ui_add_log(line, COL_SERVER);
    } else {
        snprintf(line, sizeof(line), "[raw] %s", raw);
        ui_add_log(line, COL_NORMAL);
    }

    ui_redraw_chat();
}

/*
 * Hilo lector: acumula bytes del socket en `partial` y emite un mensaje
 * por cada '\n'. Maneja recvs fragmentados y pegados.
 */
static void *recv_thread(void *arg) {
    (void)arg;
    char buf[BUF_SIZE];
    char partial[BUF_SIZE] = "";

    while (running) {
        ssize_t n = recv(sock_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (running) {
                ui_add_log("Conexion con el servidor perdida.", COL_ERROR);
                ui_redraw_chat();
                running = 0;
            }
            return NULL;
        }
        buf[n] = '\0';

        strncat(partial, buf, sizeof(partial) - strlen(partial) - 1);

        char *nl;
        while ((nl = strchr(partial, '\n')) != NULL) {
            *nl = '\0';
            if (strlen(partial) > 0) {
                process_server_msg(partial);
            }
            memmove(partial, nl + 1, strlen(nl + 1) + 1);
        }
    }
    return NULL;
}

/* Traduce una linea del usuario (comando o texto) a un mensaje del protocolo. */
static void process_input(const char *line) {
    char cmd[BUF_SIZE * 2];
    char log_line[BUF_SIZE * 2];

    if (strcmp(line, "/help") == 0) {
        show_help();
        return;
    }

    if (strcmp(line, "/list") == 0) {
        /* LIST_USERS | remitente */
        snprintf(cmd, sizeof(cmd), "%s|%s", T_LIST, username);
        net_send_line(sock_fd, cmd);
        return;
    }

    if (strncmp(line, "/info ", 6) == 0) {
        const char *target = line + 6;
        /* GET_USER_INFO | remitente | usuario */
        snprintf(cmd, sizeof(cmd), "%s|%s|%s", T_GET_INFO, username, target);
        net_send_line(sock_fd, cmd);
        return;
    }

    if (strncmp(line, "/status ", 8) == 0) {
        const char *s = line + 8;
        /* CHANGE_STATUS | remitente | estado */
        snprintf(cmd, sizeof(cmd), "%s|%s|%s", T_CHANGE_STATUS, username, s);
        net_send_line(sock_fd, cmd);
        return;
    }

    if (strncmp(line, "/dm ", 4) == 0) {
        char tmp[BUF_SIZE];
        strncpy(tmp, line + 4, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *sp = strchr(tmp, ' ');
        if (!sp) {
            ui_add_log("Uso: /dm <usuario> <mensaje>", COL_ERROR);
            ui_redraw_chat();
            return;
        }
        *sp = '\0';
        const char *target = tmp;
        const char *msg = sp + 1;

        /* Sanitiza '|' -> '::' antes de meter el texto en el protocolo. */
        char safe_msg[BUF_SIZE];
        proto_sanitize(msg, safe_msg, sizeof(safe_msg));

        /* MSG_DIRECT | remitente | destino | mensaje */
        snprintf(cmd, sizeof(cmd), "%s|%s|%s|%s", T_DIRECT, username, target, safe_msg);
        net_send_line(sock_fd, cmd);

        snprintf(log_line, sizeof(log_line), "[DM -> %s] %s", target, safe_msg);
        ui_add_log(log_line, COL_DM);
        ui_redraw_chat();
        return;
    }

    if (strcmp(line, "/exit") == 0) {
        /* DISCONNECT | remitente */
        snprintf(cmd, sizeof(cmd), "%s|%s", T_DISCONNECT, username);
        net_send_line(sock_fd, cmd);
        running = 0;
        return;
    }

    /* Texto libre -> broadcast. Sanitiza '|' para no romper el formato. */
    char safe_line[BUF_SIZE];
    proto_sanitize(line, safe_line, sizeof(safe_line));

    /* MSG_BROADCAST | remitente | mensaje */
    snprintf(cmd, sizeof(cmd), "%s|%s|%s", T_BROADCAST, username, safe_line);
    net_send_line(sock_fd, cmd);
}

/*
 * Punto de entrada: conecta, envia REGISTER, levanta UI y recv thread,
 * y se queda en el input loop hasta que el usuario haga /exit o caiga la conexion.
 */
int client_session_run(const char *user, const char *server_ip, int server_port) {
    if (!user || !server_ip || server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "Parametros invalidos\n");
        return 1;
    }

    strncpy(username, user, sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';

    if (strlen(username) == 0 || strchr(username, '|')) {
        fprintf(stderr, "Nombre de usuario invalido\n");
        return 1;
    }

    sock_fd = net_connect(server_ip, server_port);
    if (sock_fd < 0) {
        return 1;
    }

    ui_init();
    ui_redraw_status(username, my_status);
    show_help();

    /* Primer mensaje obligatorio: REGISTER | usuario */
    char reg_msg[BUF_SIZE];
    snprintf(reg_msg, sizeof(reg_msg), "%s|%s", T_REGISTER, username);
    net_send_line(sock_fd, reg_msg);

    pthread_t rtid;
    pthread_create(&rtid, NULL, recv_thread, NULL);

    ui_input_loop(process_input, &running);

    running = 0;
    shutdown(sock_fd, SHUT_RDWR);
    close(sock_fd);
    pthread_join(rtid, NULL);
    ui_teardown();

    printf("Sesion cerrada. Hasta luego.\n");
    return 0;
}
