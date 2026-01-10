# Variáveis de Compilação
CC = gcc
CFLAGS = -g -Wall -Wextra -std=c17 -D_POSIX_C_SOURCE=200809L -pthread
LDFLAGS = -lncurses
INCLUDE_DIR = include

# Diretórios
OBJ_DIR = obj
BIN_DIR = bin
SRC_DIR = src

# Executáveis
SERVER = PacmanIST
CLIENT = client

# Objetos Comuns (Ficam em src/common/)
OBJS_COMMON = board.o parser.o debug.o

# Objetos do Servidor (Ficam em src/server/)
OBJS_SERVER = main.o session.o $(OBJS_COMMON)

# Objetos do Cliente (Ficam em src/client/)
OBJS_CLIENT = client_main.o api.o display.o $(OBJS_COMMON)

# O "GPS" do Make: onde procurar os ficheiros .c
vpath %.c $(SRC_DIR)/client $(SRC_DIR)/server $(SRC_DIR)/common

# --- Regras Principais ---

all: folders $(BIN_DIR)/$(SERVER) $(BIN_DIR)/$(CLIENT)

# Compilação do Servidor (Nota: sem ncurses)
$(BIN_DIR)/$(SERVER): $(addprefix $(OBJ_DIR)/, $(OBJS_SERVER))
	$(CC) $(CFLAGS) $^ -o $@

# Compilação do Cliente (Nota: com ncurses)
$(BIN_DIR)/$(CLIENT): $(addprefix $(OBJ_DIR)/, $(OBJS_CLIENT))
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Regra genérica para criar qualquer .o na pasta obj/
$(OBJ_DIR)/%.o: %.c | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -c $< -o $@

# Criar pastas bin e obj se não existirem
folders:
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(BIN_DIR)


# Limpeza total
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
	rm -f server-debug.log client-debug.log

.PHONY: all clean folders