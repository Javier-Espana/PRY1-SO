CC            = gcc
CFLAGS        = -Wall -Wextra -g -pthread
SRC_DIR       = src
BIN_DIR       = bin
SERVER_SRC    = $(SRC_DIR)/server.c
CLIENT_SRC    = $(SRC_DIR)/client.c
SERVER_BIN    = $(BIN_DIR)/chat_server
CLIENT_BIN    = $(BIN_DIR)/chat_client
LDLIBS_SERVER = -lpthread
LDLIBS_CLIENT = -lpthread -lncurses

.PHONY: all clean

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_SERVER)

$(CLIENT_BIN): $(CLIENT_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_CLIENT)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) chat_server chat_client
