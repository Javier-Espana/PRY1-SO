CC            = gcc
CFLAGS        = -Wall -Wextra -g -pthread
SRC_DIR       = src
BIN_DIR       = bin
COMMON_SRC    = $(SRC_DIR)/common/protocol.c
SERVER_SRC    = $(SRC_DIR)/server/main.c $(SRC_DIR)/server/net.c $(SRC_DIR)/server/session.c $(COMMON_SRC)
CLIENT_SRC    = $(SRC_DIR)/client/main.c $(SRC_DIR)/client/net.c $(SRC_DIR)/client/ui.c $(SRC_DIR)/client/session.c $(COMMON_SRC)
SERVER_BIN    = $(BIN_DIR)/chat_server
CLIENT_BIN    = $(BIN_DIR)/chat_client
LDLIBS_SERVER = -lpthread
LDLIBS_CLIENT = -lpthread -lncurses

.PHONY: all clean

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) $(LDLIBS_SERVER)

$(CLIENT_BIN): $(CLIENT_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRC) $(LDLIBS_CLIENT)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN) chat_server chat_client
