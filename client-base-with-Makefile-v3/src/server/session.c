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

    while (*a->session_running) {
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

void start_session(char* levels_dir, char* req_path, char* notif_path) {
    board_t board;
    struct dirent **namelist;
    volatile int session_running = 1;
    int score_acumulado=0;
    // 1. Procurar ficheiros .lvl na diretoria fornecida
    int n_levels = scandir(levels_dir, &namelist, filter_levels, alphasort);
    
    if (n_levels < 0) {
        perror("Erro ao ler diretoria de níveis");
        return;
    }
    if (n_levels == 0) {
        fprintf(stderr, "Nenhum nível (.lvl) encontrado em: %s\n", levels_dir);
        return;
    }

    // 2. Abrir pipes e fazer Handshake (Apenas UMA vez por sessão)
    int fd_notif = open(notif_path, O_WRONLY);
    int fd_req = open(req_path, O_RDONLY);

    if (fd_notif < 0 || fd_req < 0) {
        // Limpar namelist se falhar a abertura
        for (int i = 0; i < n_levels; i++) free(namelist[i]);
        free(namelist);
        return;
    }

    // Handshake: confirmar conexão ao cliente
    char ack[2] = {(char)OP_CODE_CONNECT, 0};
    write(fd_notif, ack, 2);

    // --- 3. LOOP PRINCIPAL DE NÍVEIS ---
    for (int i = 0; i < n_levels && session_running; i++) {
        
        debug("A iniciar nível %d/%d: %s\n", i + 1, n_levels, namelist[i]->d_name);

        // Carregar o nível atual
        if (load_level(&board, namelist[i]->d_name, levels_dir, score_acumulado) < 0) {
            debug("Erro ao carregar nível %s. A abortar sessão.\n", namelist[i]->d_name);
            break;
        }

        // Flag para controlar o fim deste nível específico (Portal)
        volatile int level_finished = 0;

        // Preparar Threads
        pthread_t pacman_tid;
        // NOTA: Certifica-te que atualizaste a struct pacman_task_args para ter o campo level_finished
        pacman_task_args p_args = {fd_req, &board, &session_running, &level_finished};

        pthread_create(&pacman_tid, NULL, server_pacman_task, &p_args);

        // Lançar Monstros
        pthread_t ghost_tids[MAX_GHOSTS];
        for (int g = 0; g < board.n_ghosts; g++) {
            ghost_task_args* g_args = malloc(sizeof(ghost_task_args));
            g_args->board = &board;
            g_args->ghost_index = g;
            g_args->session_running = &session_running;
            
            if (pthread_create(&ghost_tids[g], NULL, server_ghost_task, g_args) != 0) {
                perror("Erro ao criar thread do monstro");
                free(g_args);
            }
        }

        // --- GAME LOOP (Nível Atual) ---
        while (session_running && !level_finished) {
            // Envio periódico do tabuleiro
            char op = (char)OP_CODE_BOARD;
            write(fd_notif, &op, 1);
            write(fd_notif, &board.width, sizeof(int));
            write(fd_notif, &board.height, sizeof(int));
            write(fd_notif, &board.tempo, sizeof(int));
            
            // Verificar Vitória (apenas se for o último nível e acabou)
            int victory = 0; 
            
            int game_over = !board.pacmans[0].alive;
            
            write(fd_notif, &victory, sizeof(int));
            write(fd_notif, &game_over, sizeof(int));
            write(fd_notif, &board.pacmans[0].points, sizeof(int));

            char* board_str = get_board_displayed(&board);
            write(fd_notif, board_str, board.width * board.height);
            free(board_str);

            if (game_over) {
                debug("Game Over detetado.\n");
                session_running = 0; // Isto vai quebrar o loop while e o loop for
            }

            sleep_ms(board.tempo);
        }

        // --- FIM DO NÍVEL: Limpeza de Threads ---
        // Esperar pelo Pacman (ele sai do loop quando level_finished=1 ou session_running=0)
        pthread_join(pacman_tid, NULL);

        // Forçar paragem dos monstros e esperar por eles
        // Como os monstros podem estar num sleep(), o cancel é mais seguro na transição
        for (int g = 0; g < board.n_ghosts; g++) {
            pthread_cancel(ghost_tids[g]); 
            pthread_join(ghost_tids[g], NULL);
        }

        // Se completámos o último nível com sucesso
        if (level_finished && i == n_levels - 1) {
            debug("Vitória! Todos os níveis concluídos.\n");
            // Aqui poderias enviar uma última frame com victory=1
            session_running = 0;
        }
        if (session_running) {
            score_acumulado = board.pacmans[0].points;
        }
        unload_level(&board); // Limpar memória do nível antes de carregar o próximo
        }
    
    // 4. Limpeza Final da Sessão
    debug("Sessão terminada. A limpar recursos...\n");
    
    // Só agora libertamos a lista de ficheiros
    for (int i = 0; i < n_levels; i++) free(namelist[i]);
    free(namelist);
    
    close(fd_notif);
    close(fd_req);
}