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
    volatile int* level_finished;
} pacman_task_args;

typedef struct {
    board_t* board;
    int ghost_index;
    volatile int* session_running;
    volatile int* level_finished;
} ghost_task_args;

void* server_pacman_task(void* arg) {
    pacman_task_args* a = (pacman_task_args*)arg;
    char op_code;
    char move_dir;

    debug("Thread de escuta de comandos iniciada.\n");

    while (*a->session_running&& !(*a->level_finished)) {
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
                                
                int res=move_pacman(a->board, 0, &cmd);

                // 3. Guardar posição depois
                int x_depois = a->board->pacmans[0].pos_x;
                int y_depois = a->board->pacmans[0].pos_y;

                // 4. Imprimir o resultado no log
                debug("LOG MOVIMENTO: Tecla %c | Posição: (%d,%d) -> (%d,%d)\n", 
                    move_dir, x_antes, y_antes, x_depois, y_depois);

                pthread_rwlock_unlock(&a->board->state_lock);
                if (res==REACHED_PORTAL){
                    debug("Portal atingido! A mudar de nível...\n");
                    *a->level_finished = 1;
                }
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
// --- CÓDIGO A ADICIONAR NO SESSION.C ---



void* server_ghost_task(void* arg) {
    ghost_task_args* a = (ghost_task_args*)arg;
    ghost_t* ghost = &a->board->ghosts[a->ghost_index];

    debug("Thread do Fantasma %d iniciada.\n", a->ghost_index);

    while (*a->session_running && !(*a->level_finished)) {
        // 1. Verificar se o fantasma tem movimentos ou é aleatório
        command_t* cmd = NULL;
        
        // Se houver movimentos pré-definidos no ficheiro .m
        if (ghost->n_moves > 0) {
            cmd = &ghost->moves[ghost->current_move % ghost->n_moves];
        } else {
            // Se não houver, cria um comando aleatório (R = Random)
            static command_t random_cmd = {'R', 0, 0}; 
            cmd = &random_cmd;
        }

        // 2. Bloquear o estado global para garantir consistência (igual ao Pacman)
        pthread_rwlock_wrlock(&a->board->state_lock);
        
        // Executa o movimento (a lógica de colisão está dentro desta função)
        move_ghost(a->board, a->ghost_index, cmd);
        
        pthread_rwlock_unlock(&a->board->state_lock);

        // 3. Respeitar o tempo do jogo
        sleep_ms(a->board->tempo);
    }
    
    debug("Thread do Fantasma %d a encerrar.\n", a->ghost_index);
    free(a);
    return NULL;
}

// ... (structs pacman_task_args e ghost_task_args mantêm-se iguais) ...
// ... (funções server_pacman_task e server_ghost_task mantêm-se iguais) ...

// Função start_session atualizada para integração com SIGUSR1
void start_session(char* levels_dir, char* req_path, char* notif_path, board_t** active_game_slot) {
    board_t board;
    struct dirent **namelist;
    volatile int session_running = 1;
    int score_acumulado = 0;

    // --- LIGAÇÃO AO SIGUSR1 ---
    // Apontamos o slot global para a nossa variável local 'board'
    // Assim o main.c consegue ler os pontos em tempo real
    if (active_game_slot != NULL) {
        *active_game_slot = &board;
    }
    // --------------------------

    // 1. Procurar ficheiros .lvl
    int n_levels = scandir(levels_dir, &namelist, filter_levels, alphasort);
    if (n_levels <= 0) {
        // Se falhar, limpamos o slot para evitar leituras de lixo
        if (active_game_slot) *active_game_slot = NULL;
        if (n_levels == 0) fprintf(stderr, "Nenhum nível em: %s\n", levels_dir);
        else perror("Erro scandir");
        return;
    }

    // 2. Abrir pipes e Handshake
    int fd_notif = open(notif_path, O_WRONLY);
    int fd_req = open(req_path, O_RDONLY);

    if (fd_notif < 0 || fd_req < 0) {
        if (active_game_slot) *active_game_slot = NULL;
        for (int i = 0; i < n_levels; i++) free(namelist[i]);
        free(namelist);
        return;
    }

    char ack[2] = {(char)OP_CODE_CONNECT, 0};
    write(fd_notif, ack, 2);

    // --- 3. LOOP DE NÍVEIS ---
    for (int i = 0; i < n_levels && session_running; i++) {
        
        debug("Nível %d: %s\n", i + 1, namelist[i]->d_name);

        if (load_level(&board, namelist[i]->d_name, levels_dir, score_acumulado) < 0) {
            break;
        }

        volatile int level_finished = 0;

        // Lançar Threads
        pthread_t pacman_tid;
        pacman_task_args p_args = {fd_req, &board, &session_running, &level_finished};
        pthread_create(&pacman_tid, NULL, server_pacman_task, &p_args);

        pthread_t ghost_tids[MAX_GHOSTS];
        for (int g = 0; g < board.n_ghosts; g++) {
            ghost_task_args* g_args = malloc(sizeof(ghost_task_args));
            g_args->board = &board;
            g_args->ghost_index = g;
            g_args->session_running = &session_running;
            g_args->level_finished = &level_finished;
            pthread_create(&ghost_tids[g], NULL, server_ghost_task, g_args);
        }

        // Game Loop
        while (session_running && !level_finished) {
            char op = (char)OP_CODE_BOARD;
            write(fd_notif, &op, 1);
            write(fd_notif, &board.width, sizeof(int));
            write(fd_notif, &board.height, sizeof(int));
            write(fd_notif, &board.tempo, sizeof(int));
            
            int victory = 0; 
            int game_over = !board.pacmans[0].alive;
            
            write(fd_notif, &victory, sizeof(int));
            write(fd_notif, &game_over, sizeof(int));
            write(fd_notif, &board.pacmans[0].points, sizeof(int));

            char* board_str = get_board_displayed(&board);
            write(fd_notif, board_str, board.width * board.height);
            free(board_str);

            if (game_over) session_running = 0;

            sleep_ms(board.tempo);
        }

        // Limpeza de Threads
        // Forçar saída do Pacman do read() fechando o pipe (opcional mas robusto) ou apenas join
        // Como usas pipes bloqueantes, o ideal seria enviar sinal ou fechar descritor, 
        // mas o pthread_cancel resolve para sair do read.
        pthread_cancel(pacman_tid); 
        pthread_join(pacman_tid, NULL);

        for (int g = 0; g < board.n_ghosts; g++) {
            pthread_join(ghost_tids[g], NULL);
        }

        if (session_running) score_acumulado = board.pacmans[0].points;
        unload_level(&board);
    } 

    // Limpeza Final
    if (active_game_slot) *active_game_slot = NULL; // Remover da lista pública
    
    for (int i = 0; i < n_levels; i++) free(namelist[i]);
    free(namelist);
    close(fd_notif);
    close(fd_req);
}