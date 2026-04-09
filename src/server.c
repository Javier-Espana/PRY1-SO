/*
 * server.c — Servidor de Chat Multithread
 * CC3064 Sistemas Operativos — Proyecto 01
 * Grupo: José Mérida, Javier España, Angel Esquit
 *
 * Compilar: gcc -o chat_server server.c -lpthread -Wall
 * Uso:      ./chat_server <puerto>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

/* ── Constantes ─────────────────────────────────────────────── */
#define MAX_CLIENTS      50
#define BUF_SIZE         4096
#define MAX_NAME         64
#define INACTIVITY_SEC   60     /* segundos antes de INACTIVO */

/* Estados de usuario */
#define STATUS_ACTIVE    "ACTIVO"
#define STATUS_BUSY      "OCUPADO"
#define STATUS_INACTIVE  "INACTIVO"

/* Tipos de mensaje cliente → servidor */
#define T_REGISTER       "REGISTER"
#define T_BROADCAST      "MSG_BROADCAST"
#define T_DIRECT         "MSG_DIRECT"
#define T_LIST           "LIST_USERS"
#define T_GET_INFO       "GET_USER_INFO"
#define T_CHANGE_STATUS  "CHANGE_STATUS"
#define T_DISCONNECT     "DISCONNECT"

/* Tipos de mensaje servidor → cliente */
#define T_OK             "OK"
#define T_ERROR          "ERROR"
#define T_SRV_BROADCAST  "SERVER_BROADCAST"
#define T_SRV_DIRECT     "SERVER_DIRECT"
#define T_USER_LIST      "USER_LIST"
#define T_USER_INFO      "USER_INFO"
#define T_STATUS_UPDATE  "STATUS_UPDATE"
#define T_FORCED_STATUS  "FORCED_STATUS"
#define T_USER_JOINED    "USER_JOINED"
#define T_USER_LEFT      "USER_LEFT"

/* Códigos de error */
#define ERR_NAME_DUP     "101"
#define ERR_IP_DUP       "102"
#define ERR_NAME_INVALID "103"
#define ERR_USER_NF      "201"
#define ERR_STATUS_BAD   "301"
#define ERR_FMT          "401"
#define ERR_INACTIVE     "501"

/* ── Estructura de cliente ───────────────────────────────────── */
typedef struct {
    int      fd;
    char     username[MAX_NAME];
    char     ip[INET_ADDRSTRLEN];
    char     status[16];
    time_t   last_active;
    int      active;          /* 1 = slot ocupado */
    pthread_mutex_t sock_mutex;
} Client;

/* ── Estado global ───────────────────────────────────────────── */
static Client          clients[MAX_CLIENTS];
static int             client_count = 0;
static pthread_rwlock_t list_lock = PTHREAD_RWLOCK_INITIALIZER;
static int             server_fd = -1;

/* ── Utilidades de envío ─────────────────────────────────────── */

/* Envía un mensaje formateado al fd dado (sin lock; llame desde dentro del lock correcto) */
static void send_raw(int fd, const char *msg) {
    send(fd, msg, strlen(msg), 0);
}

/* Envía con lock del socket del cliente */
static void send_to_client(Client *c, const char *msg) {
    pthread_mutex_lock(&c->sock_mutex);
    send_raw(c->fd, msg);
    pthread_mutex_unlock(&c->sock_mutex);
}

/* ── Búsqueda en la lista (debe tenerse rdlock o wrlock) ─────── */
static int find_by_name(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && strcmp(clients[i].username, name) == 0)
            return i;
    return -1;
}

static int find_by_ip(const char *ip) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && strcmp(clients[i].ip, ip) == 0)
            return i;
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].active)
            return i;
    return -1;
}

/* ── Limpieza de sesión (wrlock ya tomado por quien llama) ───── */
static void remove_client_locked(int idx) {
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s|%s\n", T_USER_LEFT, clients[idx].username);

    /* Cerrar socket */
    close(clients[idx].fd);
    clients[idx].active = 0;
    client_count--;
    pthread_mutex_destroy(&clients[idx].sock_mutex);

    /* Notificar a los demás (sin lock, ya tenemos wrlock → nadie más entra) */
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active)
            send_raw(clients[i].fd, buf);
}

/* ── Manejadores de cada tipo de mensaje ────────────────────── */

static void handle_register(int idx_or_neg __attribute__((unused)), int fd, const char *ip,
                             char *fields[], int nf) {
    /* fields[1] = username */
    if (nf < 2 || strlen(fields[1]) == 0) {
        char r[128]; snprintf(r, sizeof(r), "%s|%s|Nombre invalido\n", T_ERROR, ERR_NAME_INVALID);
        send_raw(fd, r); return;
    }
    /* Validar que no contenga '|' */
    if (strchr(fields[1], '|')) {
        char r[128]; snprintf(r, sizeof(r), "%s|%s|Nombre contiene caracter invalido\n", T_ERROR, ERR_NAME_INVALID);
        send_raw(fd, r); return;
    }

    pthread_rwlock_wrlock(&list_lock);

    if (find_by_name(fields[1]) >= 0) {
        pthread_rwlock_unlock(&list_lock);
        char r[128]; snprintf(r, sizeof(r), "%s|%s|Nombre ya registrado\n", T_ERROR, ERR_NAME_DUP);
        send_raw(fd, r); return;
    }
    if (find_by_ip(ip) >= 0) {
        pthread_rwlock_unlock(&list_lock);
        char r[128]; snprintf(r, sizeof(r), "%s|%s|IP ya tiene sesion activa\n", T_ERROR, ERR_IP_DUP);
        send_raw(fd, r); return;
    }
    int slot = find_free_slot();
    if (slot < 0) {
        pthread_rwlock_unlock(&list_lock);
        char r[128]; snprintf(r, sizeof(r), "%s|%s|Servidor lleno\n", T_ERROR, "999");
        send_raw(fd, r); return;
    }

    clients[slot].fd = fd;
    strncpy(clients[slot].username, fields[1], MAX_NAME - 1);
    strncpy(clients[slot].ip, ip, INET_ADDRSTRLEN - 1);
    strncpy(clients[slot].status, STATUS_ACTIVE, sizeof(clients[slot].status) - 1);
    clients[slot].last_active = time(NULL);
    clients[slot].active = 1;
    pthread_mutex_init(&clients[slot].sock_mutex, NULL);
    client_count++;

    /* Notificar a los demás del nuevo usuario */
    char joined[BUF_SIZE];
    snprintf(joined, sizeof(joined), "%s|%s|%s\n", T_USER_JOINED, fields[1], ip);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && i != slot)
            send_raw(clients[i].fd, joined);

    pthread_rwlock_unlock(&list_lock);

    char r[128]; snprintf(r, sizeof(r), "%s|Bienvenido %s\n", T_OK, fields[1]);
    send_raw(fd, r);

    printf("[SERVER] Usuario '%s' conectado desde %s (slot %d)\n", fields[1], ip, slot);
}

static void handle_broadcast(int sender_fd, char *fields[], int nf) {
    /* fields: MSG_BROADCAST|remitente|mensaje */
    if (nf < 3) { send_raw(sender_fd, "ERROR|401|Formato invalido\n"); return; }

    /* Verificar que el remitente no esté INACTIVO */
    pthread_rwlock_rdlock(&list_lock);
    int si = find_by_name(fields[1]);
    if (si >= 0 && strcmp(clients[si].status, STATUS_INACTIVE) == 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|501|Debes cambiar de estado antes de enviar mensajes\n");
        return;
    }
    if (si >= 0) clients[si].last_active = time(NULL);
    pthread_rwlock_unlock(&list_lock);

    /* Broadcast */
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s|%s|%s\n", T_SRV_BROADCAST, fields[1], fields[2]);

    pthread_rwlock_rdlock(&list_lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active)
            send_to_client(&clients[i], buf);
    pthread_rwlock_unlock(&list_lock);

    send_raw(sender_fd, "OK|Mensaje enviado\n");
}

static void handle_direct(int sender_fd, char *fields[], int nf) {
    /* fields: MSG_DIRECT|remitente|destino|mensaje */
    if (nf < 4) { send_raw(sender_fd, "ERROR|401|Formato invalido\n"); return; }

    pthread_rwlock_rdlock(&list_lock);
    int si = find_by_name(fields[1]);
    if (si >= 0 && strcmp(clients[si].status, STATUS_INACTIVE) == 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|501|Debes cambiar de estado antes de enviar mensajes\n");
        return;
    }
    if (si >= 0) clients[si].last_active = time(NULL);

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

static void handle_list(int sender_fd, char *fields[], int nf) {
    (void)fields; (void)nf;
    char buf[BUF_SIZE];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", T_USER_LIST);

    pthread_rwlock_rdlock(&list_lock);
    int cnt = 0;
    /* Contamos primero */
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active) cnt++;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "|%d", cnt);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "|%s:%s",
                            clients[i].username, clients[i].status);
    pthread_rwlock_unlock(&list_lock);

    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    send_raw(sender_fd, buf);
}

static void handle_get_info(int sender_fd, char *fields[], int nf) {
    /* fields: GET_USER_INFO|remitente|usuario */
    if (nf < 3) { send_raw(sender_fd, "ERROR|401|Formato invalido\n"); return; }

    pthread_rwlock_rdlock(&list_lock);
    int idx = find_by_name(fields[2]);
    if (idx < 0) {
        pthread_rwlock_unlock(&list_lock);
        send_raw(sender_fd, "ERROR|201|Usuario no encontrado\n");
        return;
    }
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s|%s|%s|%s\n",
             T_USER_INFO, clients[idx].username, clients[idx].ip, clients[idx].status);
    pthread_rwlock_unlock(&list_lock);

    send_raw(sender_fd, buf);
}

static void handle_change_status(int sender_fd, char *fields[], int nf) {
    /* fields: CHANGE_STATUS|remitente|estado */
    if (nf < 3) { send_raw(sender_fd, "ERROR|401|Formato invalido\n"); return; }

    const char *new_status = fields[2];
    if (strcmp(new_status, STATUS_ACTIVE) != 0 &&
        strcmp(new_status, STATUS_BUSY)   != 0 &&
        strcmp(new_status, STATUS_INACTIVE) != 0) {
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
    clients[idx].last_active = time(NULL);
    pthread_rwlock_unlock(&list_lock);

    char r[BUF_SIZE];
    snprintf(r, sizeof(r), "%s|%s|%s\n", T_STATUS_UPDATE, fields[1], new_status);
    send_raw(sender_fd, r);
}

static void handle_disconnect(int idx, char *fields[], int nf) {
    (void)fields; (void)nf;
    /* El hilo del cliente llama esto y luego termina */
    char r[128]; snprintf(r, sizeof(r), "%s|Hasta luego %s\n", T_OK, clients[idx].username);
    send_raw(clients[idx].fd, r);

    pthread_rwlock_wrlock(&list_lock);
    remove_client_locked(idx);
    pthread_rwlock_unlock(&list_lock);
}

/* ── Hilo de inactividad ─────────────────────────────────────── */
static void *inactivity_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(10); /* revisar cada 10 segundos */
        time_t now = time(NULL);

        pthread_rwlock_wrlock(&list_lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) continue;
            if (strcmp(clients[i].status, STATUS_INACTIVE) == 0) continue;
            if (now - clients[i].last_active >= INACTIVITY_SEC) {
                strncpy(clients[i].status, STATUS_INACTIVE, sizeof(clients[i].status) - 1);
                char buf[BUF_SIZE];
                snprintf(buf, sizeof(buf), "%s|%s|%s\n",
                         T_FORCED_STATUS, clients[i].username, STATUS_INACTIVE);
                send_raw(clients[i].fd, buf);
                printf("[SERVER] Usuario '%s' marcado INACTIVO por timeout\n", clients[i].username);
            }
        }
        pthread_rwlock_unlock(&list_lock);
    }
    return NULL;
}

/* ── Hilo principal de cada cliente ─────────────────────────── */
typedef struct { int fd; char ip[INET_ADDRSTRLEN]; } ClientArg;

static void *client_thread(void *arg) {
    ClientArg ca = *(ClientArg *)arg;
    free(arg);

    int  fd  = ca.fd;
    char ip[INET_ADDRSTRLEN];
    strncpy(ip, ca.ip, sizeof(ip));

    char buf[BUF_SIZE];
    char *fields[16];
    int  my_slot = -1;    /* índice en clients[], -1 = no registrado */
    int  registered = 0;

    while (1) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            /* Desconexión abrupta */
            if (registered && my_slot >= 0) {
                pthread_rwlock_wrlock(&list_lock);
                if (clients[my_slot].active)
                    remove_client_locked(my_slot);
                pthread_rwlock_unlock(&list_lock);
                printf("[SERVER] Usuario '%s' desconectado abruptamente\n",
                       clients[my_slot].username);
            }
            close(fd);
            return NULL;
        }
        buf[n] = '\0';

        /* Quitar '\n' */
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';

        /* Parsear campos separados por '|' */
        int nf = 0;
        char tmp[BUF_SIZE];
        strncpy(tmp, buf, sizeof(tmp));
        char *tok = strtok(tmp, "|");
        while (tok && nf < 16) { fields[nf++] = tok; tok = strtok(NULL, "|"); }

        if (nf == 0) continue;

        /* Si no registrado, solo acepta REGISTER */
        if (!registered) {
            if (strcmp(fields[0], T_REGISTER) == 0) {
                handle_register(my_slot, fd, ip, fields, nf);
                /* Buscar el slot que se asignó */
                pthread_rwlock_rdlock(&list_lock);
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].active && clients[i].fd == fd) {
                        my_slot = i; registered = 1; break;
                    }
                }
                pthread_rwlock_unlock(&list_lock);
            } else {
                send_raw(fd, "ERROR|401|Debes registrarte primero\n");
            }
            continue;
        }

        /* Actualizar last_active si está activo o busy */
        pthread_rwlock_wrlock(&list_lock);
        if (clients[my_slot].active &&
            strcmp(clients[my_slot].status, STATUS_INACTIVE) != 0)
            clients[my_slot].last_active = time(NULL);
        pthread_rwlock_unlock(&list_lock);

        /* Despachar por tipo */
        if      (strcmp(fields[0], T_BROADCAST) == 0)     handle_broadcast(fd, fields, nf);
        else if (strcmp(fields[0], T_DIRECT) == 0)         handle_direct(fd, fields, nf);
        else if (strcmp(fields[0], T_LIST) == 0)           handle_list(fd, fields, nf);
        else if (strcmp(fields[0], T_GET_INFO) == 0)       handle_get_info(fd, fields, nf);
        else if (strcmp(fields[0], T_CHANGE_STATUS) == 0)  handle_change_status(fd, fields, nf);
        else if (strcmp(fields[0], T_DISCONNECT) == 0) {
            handle_disconnect(my_slot, fields, nf);
            return NULL;
        }
        else {
            send_raw(fd, "ERROR|401|Tipo de mensaje desconocido\n");
        }
    }
}

/* ── main ────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc != 2) { fprintf(stderr, "Uso: %s <puerto>\n", argv[0]); return 1; }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "Puerto invalido\n"); return 1; }

    signal(SIGPIPE, SIG_IGN); /* ignorar SIGPIPE para no crashear al escribir a socket cerrado */

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 10) < 0) { perror("listen"); return 1; }

    printf("[SERVER] Escuchando en puerto %d\n", port);
    printf("[SERVER] Timeout de inactividad: %d segundos\n", INACTIVITY_SEC);

    /* Inicializar array de clientes */
    memset(clients, 0, sizeof(clients));

    /* Hilo de inactividad */
    pthread_t inactivity_tid;
    pthread_create(&inactivity_tid, NULL, inactivity_thread, NULL);
    pthread_detach(inactivity_tid);

    /* Loop principal: aceptar conexiones */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int cfd = accept(server_fd, (struct sockaddr *)&client_addr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }

        ClientArg *ca = malloc(sizeof(ClientArg));
        ca->fd = cfd;
        inet_ntop(AF_INET, &client_addr.sin_addr, ca->ip, sizeof(ca->ip));

        printf("[SERVER] Nueva conexion desde %s\n", ca->ip);

        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, ca);
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
