#include "client_app.h"

#include <arpa/inet.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/chat_protocol.h"

#define MAX_LOG 500
#define INPUT_MAX 512

#define COL_NORMAL 1
#define COL_MY_MSG 2
#define COL_SERVER 3
#define COL_ERROR 4
#define COL_STATUS 5
#define COL_DM 6
#define COL_HEADER 7

static int sock_fd = -1;
static char username[MAX_NAME];
static char my_status[16] = STATUS_ACTIVE;
static int running = 1;

typedef struct {
    char text[512];
    int color;
} LogLine;

static LogLine chat_log[MAX_LOG];
static int log_count = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static WINDOW *win_chat = NULL;
static WINDOW *win_input = NULL;
static WINDOW *win_status = NULL;
static int rows;
static int cols;
static pthread_mutex_t screen_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_log(const char *text, int color) {
    pthread_mutex_lock(&log_mutex);
    if (log_count < MAX_LOG) {
        strncpy(chat_log[log_count].text, text, sizeof(chat_log[log_count].text) - 1);
        chat_log[log_count].text[sizeof(chat_log[log_count].text) - 1] = '\0';
        chat_log[log_count].color = color;
        log_count++;
    } else {
        memmove(chat_log, chat_log + 1, sizeof(LogLine) * (MAX_LOG - 1));
        strncpy(chat_log[MAX_LOG - 1].text, text, sizeof(chat_log[MAX_LOG - 1].text) - 1);
        chat_log[MAX_LOG - 1].text[sizeof(chat_log[MAX_LOG - 1].text) - 1] = '\0';
        chat_log[MAX_LOG - 1].color = color;
    }
    pthread_mutex_unlock(&log_mutex);
}

static void redraw_chat(void) {
    pthread_mutex_lock(&screen_mutex);
    werase(win_chat);
    box(win_chat, 0, 0);
    mvwprintw(win_chat, 0, 2, " Chat ");

    int visible = rows - 6;
    if (visible < 1) {
        pthread_mutex_unlock(&screen_mutex);
        return;
    }

    pthread_mutex_lock(&log_mutex);
    int start = log_count > visible ? log_count - visible : 0;
    for (int i = start; i < log_count; i++) {
        wattron(win_chat, COLOR_PAIR(chat_log[i].color));
        mvwprintw(win_chat, 1 + (i - start), 2, "%.*s", cols - 4, chat_log[i].text);
        wattroff(win_chat, COLOR_PAIR(chat_log[i].color));
    }
    pthread_mutex_unlock(&log_mutex);

    wrefresh(win_chat);
    pthread_mutex_unlock(&screen_mutex);
}

static void redraw_status(void) {
    pthread_mutex_lock(&screen_mutex);
    werase(win_status);
    wattron(win_status, COLOR_PAIR(COL_HEADER));
    mvwprintw(win_status, 0, 1, " Usuario: %-20s Estado: %-10s  [/help para comandos]", username, my_status);
    wattroff(win_status, COLOR_PAIR(COL_HEADER));
    wrefresh(win_status);
    pthread_mutex_unlock(&screen_mutex);
}

static void init_ui(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    getmaxyx(stdscr, rows, cols);

    start_color();
    use_default_colors();
    init_pair(COL_NORMAL, COLOR_WHITE, -1);
    init_pair(COL_MY_MSG, COLOR_GREEN, -1);
    init_pair(COL_SERVER, COLOR_CYAN, -1);
    init_pair(COL_ERROR, COLOR_RED, -1);
    init_pair(COL_STATUS, COLOR_YELLOW, -1);
    init_pair(COL_DM, COLOR_MAGENTA, -1);
    init_pair(COL_HEADER, COLOR_BLACK, COLOR_CYAN);

    win_chat = newwin(rows - 4, cols, 1, 0);
    win_input = newwin(3, cols, rows - 3, 0);
    win_status = newwin(1, cols, 0, 0);

    scrollok(win_chat, TRUE);

    box(win_chat, 0, 0);
    box(win_input, 0, 0);
    mvwprintw(win_input, 0, 2, " Mensaje ");

    refresh();
    wrefresh(win_chat);
    wrefresh(win_input);
}

static void teardown_ui(void) {
    if (win_chat) {
        delwin(win_chat);
        win_chat = NULL;
    }
    if (win_input) {
        delwin(win_input);
        win_input = NULL;
    }
    if (win_status) {
        delwin(win_status);
        win_status = NULL;
    }
    endwin();
}

static void server_send(const char *msg) {
    char buf[BUF_SIZE];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    send(sock_fd, buf, strlen(buf), 0);
}

static void show_help(void) {
    add_log("--- AYUDA ------------------------------------------", COL_SERVER);
    add_log("  <mensaje>            -> enviar al chat general", COL_SERVER);
    add_log("  /dm <usuario> <msg>  -> mensaje directo privado", COL_SERVER);
    add_log("  /list                -> listar usuarios conectados", COL_SERVER);
    add_log("  /info <usuario>      -> ver IP y estado de usuario", COL_SERVER);
    add_log("  /status <estado>     -> ACTIVO | OCUPADO | INACTIVO", COL_SERVER);
    add_log("  /help                -> mostrar esta ayuda", COL_SERVER);
    add_log("  /exit                -> salir del chat", COL_SERVER);
    add_log("----------------------------------------------------", COL_SERVER);
    redraw_chat();
}

static void process_server_msg(const char *raw) {
    char buf[BUF_SIZE];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *fields[16];
    int nf = 0;
    char *tok = strtok(buf, "|");
    while (tok && nf < 16) {
        fields[nf++] = tok;
        tok = strtok(NULL, "|");
    }
    if (nf == 0) {
        return;
    }

    char line[512];

    if (strcmp(fields[0], T_OK) == 0) {
        snprintf(line, sizeof(line), "[OK] %s", nf > 1 ? fields[1] : "OK");
        add_log(line, COL_SERVER);
    } else if (strcmp(fields[0], T_ERROR) == 0) {
        snprintf(line, sizeof(line), "[ERROR %s] %s", nf > 1 ? fields[1] : "?", nf > 2 ? fields[2] : "");
        add_log(line, COL_ERROR);
    } else if (strcmp(fields[0], T_SRV_BROADCAST) == 0 && nf >= 3) {
        int is_me = strcmp(fields[1], username) == 0;
        snprintf(line, sizeof(line), "[%s] %s", fields[1], fields[2]);
        add_log(line, is_me ? COL_MY_MSG : COL_NORMAL);
    } else if (strcmp(fields[0], T_SRV_DIRECT) == 0 && nf >= 3) {
        snprintf(line, sizeof(line), "[DM de %s] %s", fields[1], fields[2]);
        add_log(line, COL_DM);
    } else if (strcmp(fields[0], T_USER_LIST) == 0) {
        int cnt = nf > 1 ? atoi(fields[1]) : 0;
        snprintf(line, sizeof(line), "--- Usuarios conectados (%d) ---", cnt);
        add_log(line, COL_STATUS);
        for (int i = 2; i < nf; i++) {
            snprintf(line, sizeof(line), "  - %s", fields[i]);
            add_log(line, COL_STATUS);
        }
    } else if (strcmp(fields[0], T_USER_INFO) == 0 && nf >= 4) {
        snprintf(line, sizeof(line), "%s  IP: %s  Estado: %s", fields[1], fields[2], fields[3]);
        add_log(line, COL_STATUS);
    } else if (strcmp(fields[0], T_STATUS_UPDATE) == 0 && nf >= 3) {
        strncpy(my_status, fields[2], sizeof(my_status) - 1);
        my_status[sizeof(my_status) - 1] = '\0';
        snprintf(line, sizeof(line), "Estado cambiado a %s", fields[2]);
        add_log(line, COL_SERVER);
        redraw_status();
    } else if (strcmp(fields[0], T_FORCED_STATUS) == 0 && nf >= 3) {
        strncpy(my_status, fields[2], sizeof(my_status) - 1);
        my_status[sizeof(my_status) - 1] = '\0';
        snprintf(line, sizeof(line), "Estado cambiado a %s por inactividad", fields[2]);
        add_log(line, COL_STATUS);
        redraw_status();
    } else if (strcmp(fields[0], T_USER_JOINED) == 0 && nf >= 2) {
        snprintf(line, sizeof(line), "%s se conecto", fields[1]);
        add_log(line, COL_SERVER);
    } else if (strcmp(fields[0], T_USER_LEFT) == 0 && nf >= 2) {
        snprintf(line, sizeof(line), "%s se desconecto", fields[1]);
        add_log(line, COL_SERVER);
    } else {
        snprintf(line, sizeof(line), "[raw] %s", raw);
        add_log(line, COL_NORMAL);
    }

    redraw_chat();
}

static void *recv_thread(void *arg) {
    (void)arg;
    char buf[BUF_SIZE];
    char partial[BUF_SIZE] = "";

    while (running) {
        ssize_t n = recv(sock_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (running) {
                add_log("Conexion con el servidor perdida.", COL_ERROR);
                redraw_chat();
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

static void process_input(const char *line) {
    char cmd[BUF_SIZE * 2];
    char log_line[BUF_SIZE * 2];

    if (strcmp(line, "/help") == 0) {
        show_help();
        return;
    }

    if (strcmp(line, "/list") == 0) {
        snprintf(cmd, sizeof(cmd), "%s|%s", T_LIST, username);
        server_send(cmd);
        return;
    }

    if (strncmp(line, "/info ", 6) == 0) {
        const char *target = line + 6;
        snprintf(cmd, sizeof(cmd), "%s|%s|%s", T_GET_INFO, username, target);
        server_send(cmd);
        return;
    }

    if (strncmp(line, "/status ", 8) == 0) {
        const char *s = line + 8;
        snprintf(cmd, sizeof(cmd), "%s|%s|%s", T_CHANGE_STATUS, username, s);
        server_send(cmd);
        return;
    }

    if (strncmp(line, "/dm ", 4) == 0) {
        char tmp[BUF_SIZE];
        strncpy(tmp, line + 4, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *sp = strchr(tmp, ' ');
        if (!sp) {
            add_log("Uso: /dm <usuario> <mensaje>", COL_ERROR);
            redraw_chat();
            return;
        }
        *sp = '\0';
        const char *target = tmp;
        const char *msg = sp + 1;

        snprintf(cmd, sizeof(cmd), "%s|%s|%s|%s", T_DIRECT, username, target, msg);
        server_send(cmd);

        snprintf(log_line, sizeof(log_line), "[DM -> %s] %s", target, msg);
        add_log(log_line, COL_DM);
        redraw_chat();
        return;
    }

    if (strcmp(line, "/exit") == 0) {
        snprintf(cmd, sizeof(cmd), "%s|%s", T_DISCONNECT, username);
        server_send(cmd);
        running = 0;
        return;
    }

    snprintf(cmd, sizeof(cmd), "%s|%s|%s", T_BROADCAST, username, line);
    server_send(cmd);
}

static void input_loop(void) {
    char input[INPUT_MAX + 1];
    int pos = 0;

    pthread_mutex_lock(&screen_mutex);
    wmove(win_input, 1, 2);
    wrefresh(win_input);
    pthread_mutex_unlock(&screen_mutex);

    while (running) {
        int ch = wgetch(win_input);
        if (ch == ERR) {
            usleep(10000);
            continue;
        }

        if (ch == '\n' || ch == KEY_ENTER) {
            if (pos > 0) {
                input[pos] = '\0';
                process_input(input);
                pos = 0;

                pthread_mutex_lock(&screen_mutex);
                wmove(win_input, 1, 2);
                wclrtoeol(win_input);
                box(win_input, 0, 0);
                mvwprintw(win_input, 0, 2, " Mensaje ");
                wrefresh(win_input);
                pthread_mutex_unlock(&screen_mutex);
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                pthread_mutex_lock(&screen_mutex);
                int cy;
                int cx;
                getyx(win_input, cy, cx);
                (void)cy;
                if (cx > 2) {
                    wmove(win_input, 1, cx - 1);
                    wdelch(win_input);
                }
                wrefresh(win_input);
                pthread_mutex_unlock(&screen_mutex);
            }
        } else if (ch >= 32 && ch < 256 && pos < INPUT_MAX) {
            input[pos++] = (char)ch;
            pthread_mutex_lock(&screen_mutex);
            waddch(win_input, (chtype)ch);
            wrefresh(win_input);
            pthread_mutex_unlock(&screen_mutex);
        }
    }
}

int client_run(const char *user, const char *server_ip, int server_port) {
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

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &saddr.sin_addr) <= 0) {
        fprintf(stderr, "IP invalida: %s\n", server_ip);
        close(sock_fd);
        return 1;
    }

    if (connect(sock_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    init_ui();
    redraw_status();
    show_help();

    char reg_msg[BUF_SIZE];
    snprintf(reg_msg, sizeof(reg_msg), "%s|%s\n", T_REGISTER, username);
    send(sock_fd, reg_msg, strlen(reg_msg), 0);

    pthread_t rtid;
    pthread_create(&rtid, NULL, recv_thread, NULL);

    nodelay(win_input, TRUE);
    input_loop();

    running = 0;
    shutdown(sock_fd, SHUT_RDWR);
    close(sock_fd);
    pthread_join(rtid, NULL);
    teardown_ui();

    printf("Sesion cerrada. Hasta luego.\n");
    return 0;
}
