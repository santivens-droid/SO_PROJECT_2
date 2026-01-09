#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "protocol.h"
#include "debug.h"

// Variável global para sinalizar a paragem do servidor (externa)
volatile sig_atomic_t server_shutdown = 0;
char* global_fifo_registo = NULL;

// Handler para SIGINT (Ctrl+C) ou SIGTERM
void handle_server_shutdown(int sig) {
    (void)sig;
    server_shutdown = 1;
    if (global_fifo_registo) {
        unlink(global_fifo_registo); // Limpeza física do pipe de registo
    }
    debug("Sinal de paragem recebido. Servidor a encerrar...\n");
    exit(0); 
}

void start_session(char* levels_dir, char* req_path, char* notif_path);

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <levels_dir> <max_games> <fifo_registo>\n", argv[0]);
        return 1;
    }

    char* levels_dir = argv[1];
    global_fifo_registo = argv[3];

    // Registar o tratamento de sinais conforme a nota de terminação externa
    signal(SIGINT, handle_server_shutdown);
    signal(SIGTERM, handle_server_shutdown);

    open_debug_file("server-debug.log");
    unlink(global_fifo_registo);
    if (mkfifo(global_fifo_registo, 0666) < 0) {
        perror("Erro ao criar FIFO de registo");
        return 1;
    }

    debug("Servidor iniciado indefinidamente. Escutando: %s\n", global_fifo_registo);

    // Loop infinito de aceitação (sem temporizadores de inatividade)
    while (!server_shutdown) {
        int fd = open(global_fifo_registo, O_RDONLY);
        if (fd < 0) break; // Erro fatal na leitura encerra o servidor

        char buffer[1 + 2 * MAX_PIPE_PATH_LENGTH];
        if (read(fd, buffer, sizeof(buffer)) > 0 && buffer[0] == (char)OP_CODE_CONNECT) {
            char req_path[MAX_PIPE_PATH_LENGTH + 1] = {0};
            char notif_path[MAX_PIPE_PATH_LENGTH + 1] = {0};
            
            strncpy(req_path, buffer + 1, MAX_PIPE_PATH_LENGTH);
            strncpy(notif_path, buffer + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);

            // Sessão única: Processa um cliente e espera que ele termine
            start_session(levels_dir, req_path, notif_path);
        }
        close(fd);
    }

    unlink(global_fifo_registo);
    close_debug_file();
    return 0;
}