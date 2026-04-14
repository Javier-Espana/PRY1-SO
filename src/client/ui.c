#include "ui.h"

#include <ncurses.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define MAX_LOG 500
#define INPUT_MAX 512

typedef struct {
    char text[512];
    int color;
} LogLine;

static LogLine chat_log[MAX_LOG];
static int log_count = 0;
/* log_mutex: protege chat_log (productor = recv_thread, consumidor = redraw). */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static WINDOW *win_chat = NULL;
static WINDOW *win_input = NULL;
static WINDOW *win_status = NULL;
static int rows;
static int cols;
/* screen_mutex: ncurses no es thread-safe; serializa accesos a las windows. */
static pthread_mutex_t screen_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Agrega una linea al buffer. Si esta lleno, descarta la mas vieja (FIFO). */
void ui_add_log(const char *text, int color) {
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

/* Redibuja chat mostrando las ultimas `visible` lineas del log. */
void ui_redraw_chat(void) {
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
    /* Devolver cursor a la caja de input (ultimo wrefresh gana). */
    if (win_input) wrefresh(win_input);
    pthread_mutex_unlock(&screen_mutex);
}

/* Redibuja la barra superior con usuario y estado actual. */
void ui_redraw_status(const char *username, const char *status) {
    pthread_mutex_lock(&screen_mutex);
    werase(win_status);
    wattron(win_status, COLOR_PAIR(COL_HEADER));
    mvwprintw(win_status, 0, 1, " Usuario: %-20s Estado: %-10s  [/help para comandos]", username, status);
    wattroff(win_status, COLOR_PAIR(COL_HEADER));
    wrefresh(win_status);
    if (win_input) wrefresh(win_input);
    pthread_mutex_unlock(&screen_mutex);
}

/* Arranca ncurses, define pares de color y crea las 3 ventanas. */
void ui_init(void) {
    initscr();
    cbreak();
    noecho();
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

    /* keypad en win_input para que wgetch traduzca KEY_BACKSPACE/KEY_ENTER. */
    keypad(win_input, TRUE);
    scrollok(win_chat, TRUE);

    box(win_chat, 0, 0);
    box(win_input, 0, 0);
    mvwprintw(win_input, 0, 2, " Mensaje ");

    refresh();
    wrefresh(win_chat);
    wrefresh(win_input);
}

/* Libera ventanas y restaura la terminal. */
void ui_teardown(void) {
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

/*
 * Lee teclas y arma una linea hasta Enter; entrega al callback on_line.
 * nodelay + usleep para no bloquear (asi el flag `running` se revisa seguido).
 */
void ui_input_loop(void (*on_line)(const char *line), volatile int *running) {
    char input[INPUT_MAX + 1];
    int pos = 0;

    nodelay(win_input, TRUE);

    pthread_mutex_lock(&screen_mutex);
    wmove(win_input, 1, 2);
    wrefresh(win_input);
    pthread_mutex_unlock(&screen_mutex);

    while (*running) {
        int ch = wgetch(win_input);
        if (ch == ERR) {
            usleep(10000);
            continue;
        }

        if (ch == '\n' || ch == KEY_ENTER) {
            if (pos > 0) {
                input[pos] = '\0';
                on_line(input);
                pos = 0;

                pthread_mutex_lock(&screen_mutex);
                wmove(win_input, 1, 2);
                wclrtoeol(win_input);
                box(win_input, 0, 0);
                mvwprintw(win_input, 0, 2, " Mensaje ");
                /* Reposicionar DESPUES del label, para que el cursor fisico
                 * termine dentro de la caja, no en la linea del borde. */
                wmove(win_input, 1, 2);
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
