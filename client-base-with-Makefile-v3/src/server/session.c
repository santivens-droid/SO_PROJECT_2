#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "board.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"
#include "parser.h"

// Estrutura para sincronizar a paragem das threads da sessão
typedef struct {
    int fd_req;
    board_t* board;
    volatile int* session_running;
} pacman_task_args;

void* server_pacman_task(void* arg) {
    pacman_task_args* a = (pacman_task_args*)arg;
    char op_code;
    char move_dir;

    debug("Thread de escuta de comandos iniciada.\n");

    while (*a->session_running) {
        // 1. Ler apenas 1 byte para saber qual é a operação
        ssize_t n = read(a->fd_req, &op_code, 1);
        
        if (n <= 0) { 
            debug("Cliente desconectado (Pipe fechado).\n");
            *a->session_running = 0;
            break;
        }

        // 2. Tratar cada tipo de operação
        if (op_code == (char)OP_CODE_PLAY) {
            // Se é um movimento, agora lemos o segundo byte (a direção)
            if (read(a->fd_req, &move_dir, 1) > 0) {
                
                debug("Servidor: Recebido comando de movimento '%c'\n", move_dir);

                command_t cmd = {move_dir, 1, 1};
                
                pthread_rwlock_wrlock(&a->board->state_lock);
                // Move o pacman 0 (podes precisar de passar o ID real do cliente aqui)
                

                // 1. Guardar posição antes
                int x_antes = a->board->pacmans[0].pos_x;
                int y_antes = a->board->pacmans[0].pos_y;

                a->board->pacmans[0].alive = 1; 
                a->board->pacmans[0].passo = 0;   // Remove o cooldown para o teclado responder logo
                a->board->pacmans[0].waiting = 0; // Garante que não está em espera
                                
                move_pacman(a->board, 0, &cmd);

                // 3. Guardar posição depois
                int x_depois = a->board->pacmans[0].pos_x;
                int y_depois = a->board->pacmans[0].pos_y;

                // 4. Imprimir o resultado no log
                debug("LOG MOVIMENTO: Tecla %c | Posição: (%d,%d) -> (%d,%d)\n", 
                    move_dir, x_antes, y_antes, x_depois, y_depois);

                pthread_rwlock_unlock(&a->board->state_lock);
            }
        } 
        else if (op_code == (char)OP_CODE_DISCONNECT) {
            debug("Servidor: Cliente enviou pedido de desconexão voluntária.\n");
            *a->session_running = 0;
            break;
        }
    }
    
    debug("Encerrando thread de escuta de comandos.\n");
    return NULL;
}

void start_session(char* levels_dir, char* req_path, char* notif_path) {
    board_t board;
    struct dirent **namelist;
    
    // 1. Procurar ficheiros .lvl na diretoria fornecida
    int n = scandir(levels_dir, &namelist, filter_levels, alphasort);
    
    if (n < 0) {
        perror("Erro ao ler diretoria de níveis");
        return;
    }
    if (n == 0) {
        fprintf(stderr, "Nenhum nível (.lvl) encontrado em: %s\n", levels_dir);
        return;
    }

    // 2. Carregar o primeiro nível da lista (namelist[0])
    // Usamos namelist[0]->d_name em vez de "level1.lvl"
    if (load_level(&board, namelist[0]->d_name, levels_dir, 0) < 0) {
        // Limpeza de memória antes de sair em caso de erro
        for (int i = 0; i < n; i++) free(namelist[i]);
        free(namelist);
        return;
    }

    // 3. Libertar a memória alocada pelo scandir (já não precisamos da lista)
    for (int i = 0; i < n; i++) free(namelist[i]);
    free(namelist);
    int fd_notif = open(notif_path, O_WRONLY);
    int fd_req = open(req_path, O_RDONLY);

    if (fd_notif < 0 || fd_req < 0) {
        unload_level(&board);
        return;
    }

    // Handshake: confirmar conexão ao cliente [cite: 81-82]
    char ack[2] = {(char)OP_CODE_CONNECT, 0};
    write(fd_notif, ack, 2);

    volatile int session_running = 1;
    pthread_t pacman_tid;
    pacman_task_args p_args = {fd_req, &board, &session_running};

    pthread_create(&pacman_tid, NULL, server_pacman_task, &p_args);
    // Aqui deveriam ser lançadas também as ghost_threads da Parte 1

    while (session_running) {
        // Envio periódico do tabuleiro (Tarefa Gestora) [cite: 122]
        char op = (char)OP_CODE_BOARD;
        write(fd_notif, &op, 1);
        write(fd_notif, &board.width, sizeof(int));
        write(fd_notif, &board.height, sizeof(int));
        write(fd_notif, &board.tempo, sizeof(int));
        
        int victory = 0; // Implementar lógica de vitória conforme board.c
        int game_over = !board.pacmans[0].alive;
        write(fd_notif, &victory, sizeof(int));
        write(fd_notif, &game_over, sizeof(int));
        write(fd_notif, &board.pacmans[0].points, sizeof(int));

        char* board_str = get_board_displayed(&board);
        write(fd_notif, board_str, board.width * board.height);
        free(board_str);

        if (game_over) {
            debug("Fim de jogo detetado no servidor.\n");
            session_running = 0;
        }

        sleep_ms(board.tempo);
    }

    // Lógica de Terminação de Sessão: Limpeza obrigatória 
    debug("A limpar recursos da sessão...\n");
    pthread_join(pacman_tid, NULL); 
    // pthread_join(ghost_threads...)
    
    unload_level(&board); // Libertar memória do tabuleiro
    close(fd_notif);
    close(fd_req);
}