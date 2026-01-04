#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>

// Variáveis privadas a este ficheiro

struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    
    // 1. Criar os FIFOs do cliente
    if (mkfifo(req_pipe_path, 0666) < 0 && errno != EEXIST) return 1;
    if (mkfifo(notif_pipe_path, 0666) < 0 && errno != EEXIST) return 1;

    // 2. Preparar a mensagem (OP_CODE + Caminhos)
    char buffer[1 + 2 * MAX_PIPE_PATH_LENGTH];
    memset(buffer, 0, sizeof(buffer));
    
    buffer[0] = (char)OP_CODE_CONNECT;
    strncpy(buffer + 1, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(buffer + 1 + MAX_PIPE_PATH_LENGTH, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    // 3. Abrir o FIFO do servidor e enviar pedido
    int server_fd = open(server_pipe_path, O_WRONLY);
    if (server_fd < 0) return 1;
    
    if (write(server_fd, buffer, sizeof(buffer)) != sizeof(buffer)) {
        close(server_fd);
        return 1;
    }
    close(server_fd);

    // 4. Abrir os pipes locais (Aqui a ordem importa!)
    // O servidor deve abrir os lados opostos
    session.notif_pipe = open(notif_pipe_path, O_RDONLY);
    if (session.notif_pipe < 0) return 1;

    session.req_pipe = open(req_pipe_path, O_WRONLY);
    if (session.req_pipe < 0) {
        close(session.notif_pipe);
        return 1;
    }

    // 5. Validar confirmação do servidor
    char response[2];
    if (read(session.notif_pipe, response, 2) != 2) return 1;

    if (response[0] != (char)OP_CODE_CONNECT || response[1] != 0) {
        close(session.notif_pipe);
        close(session.req_pipe);
        return 1;
    }

    // Guardar estado na variável static
    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.id = 1; 

    return 0; // Sucesso
}

void pacman_play(char command) {
    if (session.id == -1 || session.req_pipe < 0) return;

    char op_code = OP_CODE_PLAY;

    // Envia OP_CODE
    if (write(session.req_pipe, &op_code, sizeof(char)) < 0) {
        perror("Erro ao enviar op_code");
        return;
    }

    // Envia o Comando
    if (write(session.req_pipe, &command, sizeof(char)) < 0) {
        perror("Erro ao enviar comando");
        return;
    }
}

int pacman_disconnect() {
    int error = 0;

    // Se a sessão já não existe, consideramos sucesso (objetivo atingido)
    if (session.id == -1) return 0;

    // 1. Avisar o servidor que vamos sair
    char op_code = OP_CODE_DISCONNECT;
    if (session.req_pipe >= 0) {
        if (write(session.req_pipe, &op_code, sizeof(char)) == -1) {
            // Se falhar o aviso, registamos o erro mas continuamos a limpar
            error = 1; 
        }
    }

    // 2. Fechar os descritores de ficheiro
    if (session.req_pipe >= 0) {
        if (close(session.req_pipe) == -1) error = 1;
        session.req_pipe = -1;
    }
    if (session.notif_pipe >= 0) {
        if (close(session.notif_pipe) == -1) error = 1;
        session.notif_pipe = -1;
    }

    // 3. Apagar os ficheiros FIFO do disco
    if (session.req_pipe_path[0] != '\0') {
        if (unlink(session.req_pipe_path) == -1) {
            if (errno != ENOENT) error = 1; // Só é erro se não for "ficheiro não encontrado"
        }
        session.req_pipe_path[0] = '\0';
    }
    if (session.notif_pipe_path[0] != '\0') {
        if (unlink(session.notif_pipe_path) == -1) {
            if (errno != ENOENT) error = 1;
        }
        session.notif_pipe_path[0] = '\0';
    }

    // 4. Marcar sessão como encerrada
    session.id = -1;

    debug("Resultado da desconexão: %s\n", error == 0 ? "Sucesso" : "Falha parcial");
    
    return error; 
}

Board receive_board_update(void) {
    Board board;
    board.data = NULL; // Inicializar para evitar problemas em caso de erro

    // 1. Verificar se a sessão está ativa
    if (session.id == -1 || session.notif_pipe < 0) {
        return board;
    }

    // 2. Ler o OP_CODE para confirmar se é uma atualização de tabuleiro
    char op_code;
    if (read(session.notif_pipe, &op_code, sizeof(char)) <= 0) {
        return board;
    }

    if (op_code != (char)OP_CODE_BOARD) { // OP_CODE_BOARD é 4
        return board;
    }

    // 3. Ler os metadados (6 inteiros: width, height, tempo, victory, game_over, points)
    // De acordo com o formato: (int) width | (int) height | (int) tempo | (int) victory | (int) game_over | (int) points 
    if (read(session.notif_pipe, &board.width, sizeof(int)) <= 0) return board;
    if (read(session.notif_pipe, &board.height, sizeof(int)) <= 0) return board;
    if (read(session.notif_pipe, &board.tempo, sizeof(int)) <= 0) return board;
    if (read(session.notif_pipe, &board.victory, sizeof(int)) <= 0) return board;
    if (read(session.notif_pipe, &board.game_over, sizeof(int)) <= 0) return board;
    if (read(session.notif_pipe, &board.accumulated_points, sizeof(int)) <= 0) return board;

    // 4. Alocar memória para os dados do tabuleiro (width * height)
    int board_size = board.width * board.height;
    board.data = malloc(board_size * sizeof(char));
    if (board.data == NULL) return board;

    // 5. Ler os dados do tabuleiro propriamente ditos
    if (read(session.notif_pipe, board.data, board_size) != board_size) {
        free(board.data);
        board.data = NULL;
        return board;
    }

    return board;
}