#ifndef CLIENT_UI_H
#define CLIENT_UI_H

#define COL_NORMAL 1
#define COL_MY_MSG 2
#define COL_SERVER 3
#define COL_ERROR 4
#define COL_STATUS 5
#define COL_DM 6
#define COL_HEADER 7

/* Inicializa ncurses y las ventanas (status, chat, input). */
void ui_init(void);

/* Cierra ventanas y sale de ncurses. */
void ui_teardown(void);

/* Agrega una linea al log (buffer circular). No redibuja. */
void ui_add_log(const char *text, int color);

/* Redibuja la ventana de chat con las ultimas lineas del log. */
void ui_redraw_chat(void);

/* Redibuja la barra superior con usuario y estado. */
void ui_redraw_status(const char *username, const char *status);

/* Loop de teclado. Llama on_line(linea) cada Enter. Corre hasta *running = 0. */
void ui_input_loop(void (*on_line)(const char *line), volatile int *running);

#endif
