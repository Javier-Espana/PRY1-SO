#include "session.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/chat_protocol.h"
#include "../common/protocol.h"

#define MAX_CLIENTS 50

typedef struct {
    int fd;
    char username[MAX_NAME];
    char ip[INET_ADDRSTRLEN];
    char status[16];
    time_t last_active;
    int active;
    pthread_mutex_t sock_mutex;
} Client;

/*
 * Tabla de clientes compartida por todos los hilos.
 * list_lock: rwlock porque la mayoria de operaciones (broadcast, DM, list, info)
 * solo leen. Solo register/disconnect/change_status toman write-lock.
 * Cada Client tiene ademas un sock_mutex propio para que dos hilos no se
 * pisen al escribir al mismo socket (ver send_to_client).
 */
static Client clients[MAX_CLIENTS];
static int client_count = 0;
static pthread_rwlock_t list_lock = PTHREAD_RWLOCK_INITIALIZER;

void session_init(void) {
    memset(clients, 0, sizeof(clients));
    client_count = 0;
}

/* Envia sin sincronizar. Usar solo al socket propio (el que lee este hilo). */
static void send_raw(int fd, const char *msg) {
    send(fd, msg, strlen(msg), 0);
}

/* Envia al socket de otro cliente. Toma el mutex del socket para no mezclar bytes. */
static void send_to_client(Client *c, const char *msg) {
    pthread_mutex_lock(&c->sock_mutex);
    send_raw(c->fd, msg);
    pthread_mutex_unlock(&c->sock_mutex);
}

/* Busca por nombre. Caller debe tener list_lock (read o write). */
static int find_by_name(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].username, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Busca por IP. Caller debe tener list_lock. */
static int find_by_ip(const char *ip) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].ip, ip) == 0) {
            return i;
        }
    }
    return -1;
}

/* Primer slot libre en la tabla o -1. Caller debe tener write-lock. */
static int find_free_slot(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            return i;
        }
    }
    return -1;
}

/*
 * Cierra fd, libera el slot y notifica USER_LEFT a todos.
 * Caller DEBE tener write-lock (de ahi el sufijo _locked).
 */
static void remove_client_locked(int idx) {
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s|%s\n", T_USER_LEFT, clients[idx].username);

    close(clients[idx].fd);
    clients[idx].active = 0;
    client_count--;
    pthread_mutex_destroy(&clients[idx].sock_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            send_raw(clients[i].fd, buf);
        }
    }
}

/*
 * REGISTER | usuario
 *   fields[0] = "REGISTER", fields[1] = nombre pedido.
 * Valida unicidad de nombre e IP, inserta en la tabla, USER_JOINED a los demas.
 */
static void handle_register(int fd, const char *ip, char *fields[], int nf) {
    if (nf < 2 || strlen(fields[1]) == 0) {
        char r[128];
        snprintf(r, sizeof(r), "%s|%s|Nombre invalido\n", T_ERROR, ERR_NAME_INVALID);
        send_raw(fd, r);
        return;
    }

    if (strchr(fields[1], '|')) {
        char r[128];
        snprintf(r, sizeof(r), "%s|%s|Nombre contiene caracter invalido\n", T_ERROR, ERR_NAME_INVALID);
        send_raw(fd, r);
        return;
    }

    pthread_rwlock_wrlock(&list_lock);

    if (find_by_name(fields[1]) >= 0) {
        pthread_rwlock_unlock(&list_lock);
        char r[128];
        snprintf(r, sizeof(r), "%s|%s|Nombre ya registrado\n", T_ERROR, ERR_NAME_DUP);
        send_raw(fd, r);
        return;
    }

    if (find_by_ip(ip) >= 0) {
        pthread_rwlock_unlock(&list_lock);
        char r[128];
        snprintf(r, sizeof(r), "%s|%s|IP ya tiene sesion activa\n", T_ERROR, ERR_IP_DUP);
        send_raw(fd, r);
        return;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        pthread_rwlock_unlock(&list_lock);
        char r[128];
        snprintf(r, sizeof(r), "%s|999|Servidor lleno\n", T_ERROR);
        send_raw(fd, r);
        return;
    }

    clients[slot].fd = fd;
    strncpy(clients[slot].username, fields[1], MAX_NAME - 1);
    clients[slot].username[MAX_NAME - 1] = '\0';
    strncpy(clients[slot].ip, ip, INET_ADDRSTRLEN - 1);
    clients[slot].ip[INET_ADDRSTRLEN - 1] = '\0';
    strncpy(clients[slot].status, STATUS_ACTIVE, sizeof(clients[slot].status) - 1);
    clients[slot].status[sizeof(clients[slot].status) - 1] = '\0';
    clients[slot].last_active = time(NULL);
    clients[slot].active = 1;
    pthread_mutex_init(&clients[slot].sock_mutex, NULL);
    client_count++;

    char joined[BUF_SIZE];
    snprintf(joined, sizeof(joined), "%s|%s|%s\n", T_USER_JOINED, fields[1], ip);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && i != slot) {
            send_raw(clients[i].fd, joined);
        }
    }

    pthread_rwlock_unlock(&list_lock);

    char r[128];
    snprintf(r, sizeof(r), "%s|Bienvenido %s\n", T_OK, fields[1]);
    send_raw(fd, r);

    printf("[SERVER] Usuario '%s' conectado desde %s (slot %d)\n", fields[1], ip, slot);
}

/*
 * MSG_BROADCAST | remitente | mensaje
 *   fields[1] = remitente, fields[2] = texto.
 * Reenvia como SERVER_BROADCAST|remitente|mensaje a todos los conectados.
 */
static void handle_broadcast(int sender_fd, char *fields[], int nf) {
    if (nf < 3) {
        send_raw(sender_fd, "ERROR|401|Formato invalido\n");
        return;
    }

    pthread_rwlock_rdlock(&list_lock);
    int si = find_by_name(fields[1]);
    if (si >= 0 && strcmp(clients[si].status, STATUS_INACTIVE) == 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|501|Debes cambiar de estado antes de enviar mensajes\n");
        return;
    }
    pthread_rwlock_unlock(&list_lock);

    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s|%s|%s\n", T_SRV_BROADCAST, fields[1], fields[2]);

    pthread_rwlock_rdlock(&list_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            send_to_client(&clients[i], buf);
        }
    }
    pthread_rwlock_unlock(&list_lock);

    send_raw(sender_fd, "OK|Mensaje enviado\n");
}

/*
 * MSG_DIRECT | remitente | destino | mensaje
 *   fields[1]=remitente, fields[2]=destino, fields[3]=texto.
 * Envia SERVER_DIRECT|remitente|mensaje al destino, o ERROR 201 si no existe.
 */
static void handle_direct(int sender_fd, char *fields[], int nf) {
    if (nf < 4) {
        send_raw(sender_fd, "ERROR|401|Formato invalido\n");
        return;
    }

    pthread_rwlock_rdlock(&list_lock);
    int si = find_by_name(fields[1]);
    if (si >= 0 && strcmp(clients[si].status, STATUS_INACTIVE) == 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|501|Debes cambiar de estado antes de enviar mensajes\n");
        return;
    }

    int di = find_by_name(fields[2]);
    if (di < 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|201|Usuario destino no existe o no esta conectado\n");
        return;
    }

    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s|%s|%s\n", T_SRV_DIRECT, fields[1], fields[3]);
    send_to_client(&clients[di], buf);
    pthread_rwlock_unlock(&list_lock);

    char r[BUF_SIZE];
    snprintf(r, sizeof(r), "%s|Mensaje enviado a %s\n", T_OK, fields[2]);
    send_raw(sender_fd, r);
}

/*
 * LIST_USERS | remitente  ->  USER_LIST | n | user:estado | user:estado | ...
 * n es el conteo de conectados.
 */
static void handle_list(int sender_fd) {
    char buf[BUF_SIZE];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", T_USER_LIST);

    pthread_rwlock_rdlock(&list_lock);
    int cnt = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            cnt++;
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "|%d", cnt);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "|%s:%s", clients[i].username, clients[i].status);
        }
    }
    pthread_rwlock_unlock(&list_lock);

    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    send_raw(sender_fd, buf);
}

/*
 * GET_USER_INFO | remitente | usuario  ->  USER_INFO | usuario | ip | estado
 *   fields[2] = usuario consultado. ERROR 201 si no existe.
 */
static void handle_get_info(int sender_fd, char *fields[], int nf) {
    if (nf < 3) {
        send_raw(sender_fd, "ERROR|401|Formato invalido\n");
        return;
    }

    pthread_rwlock_rdlock(&list_lock);
    int idx = find_by_name(fields[2]);
    if (idx < 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|201|Usuario no encontrado\n");
        return;
    }

    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s|%s|%s|%s\n", T_USER_INFO, clients[idx].username, clients[idx].ip, clients[idx].status);
    pthread_rwlock_unlock(&list_lock);

    send_raw(sender_fd, buf);
}

/*
 * CHANGE_STATUS | remitente | estado  ->  STATUS_UPDATE | usuario | estado
 *   fields[2] debe ser ACTIVO, OCUPADO o INACTIVO. ERROR 301 si no.
 */
static void handle_change_status(int sender_fd, char *fields[], int nf) {
    if (nf < 3) {
        send_raw(sender_fd, "ERROR|401|Formato invalido\n");
        return;
    }

    const char *new_status = fields[2];
    if (strcmp(new_status, STATUS_ACTIVE) != 0 && strcmp(new_status, STATUS_BUSY) != 0 && strcmp(new_status, STATUS_INACTIVE) != 0) {
        send_raw(sender_fd, "ERROR|301|Estado no valido\n");
        return;
    }

    pthread_rwlock_wrlock(&list_lock);
    int idx = find_by_name(fields[1]);
    if (idx < 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|201|Usuario no encontrado\n");
        return;
    }

    strncpy(clients[idx].status, new_status, sizeof(clients[idx].status) - 1);
    clients[idx].status[sizeof(clients[idx].status) - 1] = '\0';
    clients[idx].last_active = time(NULL);
    pthread_rwlock_unlock(&list_lock);

    char r[BUF_SIZE];
    snprintf(r, sizeof(r), "%s|%s|%s\n", T_STATUS_UPDATE, fields[1], new_status);
    send_raw(sender_fd, r);
}

/*
 * DISCONNECT | remitente
 * Responde OK al cliente, lo saca de la tabla y emite USER_LEFT|usuario a los demas.
 */
static void handle_disconnect(int idx) {
    char r[128];
    snprintf(r, sizeof(r), "%s|Hasta luego %s\n", T_OK, clients[idx].username);
    send_raw(clients[idx].fd, r);

    pthread_rwlock_wrlock(&list_lock);
    remove_client_locked(idx);
    pthread_rwlock_unlock(&list_lock);
}

void *session_inactivity_thread(void *arg) {
    (void)arg;
    /* Revisa cada 10s; no es preciso al segundo, alcanza para un chat. */
    while (1) {
        sleep(10);
        time_t now = time(NULL);

        pthread_rwlock_wrlock(&list_lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                continue;
            }
            if (strcmp(clients[i].status, STATUS_INACTIVE) == 0) {
                continue;
            }
            if (now - clients[i].last_active >= INACTIVITY_SEC) {
                strncpy(clients[i].status, STATUS_INACTIVE, sizeof(clients[i].status) - 1);
                clients[i].status[sizeof(clients[i].status) - 1] = '\0';
                char buf[BUF_SIZE];
                snprintf(buf, sizeof(buf), "%s|%s|%s\n", T_FORCED_STATUS, clients[i].username, STATUS_INACTIVE);
                send_raw(clients[i].fd, buf);
                printf("[SERVER] Usuario '%s' marcado INACTIVO por timeout\n", clients[i].username);
            }
        }
        pthread_rwlock_unlock(&list_lock);
    }
    return NULL;
}

/*
 * Loop por conexion: recibe, parsea, despacha. El primer mensaje debe ser
 * REGISTER; despues se aceptan el resto. Termina con DISCONNECT o recv<=0.
 */
void *session_client_thread(void *arg) {
    ClientArg ca = *(ClientArg *)arg;
    free(arg);

    int fd = ca.fd;
    char ip[INET_ADDRSTRLEN];
    strncpy(ip, ca.ip, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';

    char buf[BUF_SIZE];
    char *fields[16];
    int my_slot = -1;
    int registered = 0;

    while (1) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (registered && my_slot >= 0) {
                char username_copy[MAX_NAME];
                pthread_rwlock_wrlock(&list_lock);
                if (clients[my_slot].active) {
                    strncpy(username_copy, clients[my_slot].username, sizeof(username_copy) - 1);
                    username_copy[sizeof(username_copy) - 1] = '\0';
                    remove_client_locked(my_slot);
                } else {
                    username_copy[0] = '\0';
                }
                pthread_rwlock_unlock(&list_lock);

                if (username_copy[0] != '\0') {
                    printf("[SERVER] Usuario '%s' desconectado abruptamente\n", username_copy);
                }
            } else {
                close(fd);
            }
            return NULL;
        }
        buf[n] = '\0';

        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = '\0';
        }

        char tmp[BUF_SIZE];
        strncpy(tmp, buf, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        int nf = proto_tokenize(tmp, fields, 16);

        if (nf == 0) {
            continue;
        }

        if (!registered) {
            if (strcmp(fields[0], T_REGISTER) == 0) {
                handle_register(fd, ip, fields, nf);

                pthread_rwlock_rdlock(&list_lock);
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].active && clients[i].fd == fd) {
                        my_slot = i;
                        registered = 1;
                        break;
                    }
                }
                pthread_rwlock_unlock(&list_lock);
            } else {
                send_raw(fd, "ERROR|401|Debes registrarte primero\n");
            }
            continue;
        }

        pthread_rwlock_wrlock(&list_lock);
        if (clients[my_slot].active && strcmp(clients[my_slot].status, STATUS_INACTIVE) != 0) {
            clients[my_slot].last_active = time(NULL);
        }
        pthread_rwlock_unlock(&list_lock);

        if (strcmp(fields[0], T_BROADCAST) == 0) {
            handle_broadcast(fd, fields, nf);
        } else if (strcmp(fields[0], T_DIRECT) == 0) {
            handle_direct(fd, fields, nf);
        } else if (strcmp(fields[0], T_LIST) == 0) {
            handle_list(fd);
        } else if (strcmp(fields[0], T_GET_INFO) == 0) {
            handle_get_info(fd, fields, nf);
        } else if (strcmp(fields[0], T_CHANGE_STATUS) == 0) {
            handle_change_status(fd, fields, nf);
        } else if (strcmp(fields[0], T_DISCONNECT) == 0) {
            handle_disconnect(my_slot);
            return NULL;
        } else {
            send_raw(fd, "ERROR|401|Tipo de mensaje desconocido\n");
        }
    }
}
