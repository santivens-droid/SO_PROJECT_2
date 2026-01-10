#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include "session.h"
#include "protocol.h"
#include "debug.h"
#include "board.h" // Necessário para aceder à struct board_t para os scores

// --- Estruturas e Constantes ---

#define MAX_BUFFER_SIZE 10 // Tamanho do buffer de pedidos pendentes

typedef struct {
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} connection_request_t;

// --- Variáveis Globais ---

// Controlo do Servidor
volatile sig_atomic_t server_shutdown = 0;
volatile sig_atomic_t sigusr1_pending = 0;  // flag para o Top 5
char* global_fifo_registo = NULL;
char* global_levels_dir = NULL;
int global_max_games = 0;

// Buffer Produtor-Consumidor
connection_request_t request_buffer[MAX_BUFFER_SIZE];
int buf_in = 0;
int buf_out = 0;
int buf_count = 0;

pthread_mutex_t mutex_buffer = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_empty;
sem_t sem_full;

// Gestão de Sessões Ativas (para o SIGUSR1 - Top 5)
// Guardamos ponteiros para os boards ativos para ler os scores
board_t** active_games; 
pthread_mutex_t mutex_sessions = PTHREAD_MUTEX_INITIALIZER;

// --- Funções Auxiliares ---

// Comparador para o qsort (ordem decrescente de pontos)
int compare_scores(const void* a, const void* b) {
    int score_a = (*(board_t**)a)->pacmans[0].points;
    int score_b = (*(board_t**)b)->pacmans[0].points;
    return score_b - score_a; 
}

void handle_sigusr1(int sig) {
    (void)sig;
    sigusr1_pending = 1; // Única operação segura permitida
}

void executar_log_top_5() {
    pthread_mutex_lock(&mutex_sessions); // Seguro aqui, fora do handler

    FILE* log = fopen("server_top_scores.log", "w");
    if (!log) {
        pthread_mutex_unlock(&mutex_sessions);
        return;
    }

    fprintf(log, "=== TOP 5 JOGOS ATIVOS ===\n");
    
    int count = 0;
    board_t* temp_list[global_max_games];
    
    for (int i = 0; i < global_max_games; i++) {
        if (active_games[i] != NULL) {
            temp_list[count++] = active_games[i];
        }
    }

    if (count == 0) {
        fprintf(log, "Nenhum jogo ativo no momento.\n");
    } else {
        qsort(temp_list, count, sizeof(board_t*), compare_scores);
        int limit = (count < 5) ? count : 5;
        for (int i = 0; i < limit; i++) {
            fprintf(log, "Rank #%d - Jogador: %s - Pontos: %d\n", 
            i + 1, 
            temp_list[i]->player_id,  // Agora usamos o ID guardado
            temp_list[i]->pacmans[0].points);
        }
    }

    fclose(log);
    pthread_mutex_unlock(&mutex_sessions);
    debug("Log de pontuações gerado com segurança.\n");
}

void handle_server_shutdown(int sig) {
    (void)sig;
    server_shutdown = 1;
    if (global_fifo_registo) unlink(global_fifo_registo);
    debug("\nSinal de paragem recebido. A encerrar...\n");
    exit(0);
}

// --- Worker Thread (Consumidor) ---
void* worker_thread(void* arg) {
    int thread_id = *(int*)arg;
    free(arg);

    // 1. Bloquear SIGUSR1
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        debug("Erro ao bloquear SIGUSR1 na thread %d\n", thread_id);
    }

    debug("Worker %d iniciado.\n", thread_id);

    while (!server_shutdown) {
        // 2. Consumir pedido
        sem_wait(&sem_full);
        
        pthread_mutex_lock(&mutex_buffer);
        connection_request_t req = request_buffer[buf_out];
        buf_out = (buf_out + 1) % MAX_BUFFER_SIZE;
        // buf_count--; // (Opcional, se não usares noutro lado)
        pthread_mutex_unlock(&mutex_buffer);
        
        sem_post(&sem_empty);

        debug("Worker %d atendeu: %s\n", thread_id, req.req_pipe_path);

        // 3. Limpar slot antes de começar (Segurança)
        pthread_mutex_lock(&mutex_sessions);
        active_games[thread_id] = NULL;
        pthread_mutex_unlock(&mutex_sessions);

        // 4. Iniciar Sessão
        start_session(global_levels_dir, req.req_pipe_path, req.notif_pipe_path, &active_games[thread_id]);

        // 5. Limpar slot após fim (Segurança extra)
        pthread_mutex_lock(&mutex_sessions);
        active_games[thread_id] = NULL;
        pthread_mutex_unlock(&mutex_sessions);

        debug("Worker %d terminou sessão.\n", thread_id);
    }
    return NULL;
}

// --- Main (Produtor / Tarefa Anfitriã) ---

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <levels_dir> <max_games> <fifo_registo>\n", argv[0]);
        return 1;
    }

    global_levels_dir = argv[1];
    global_max_games = atoi(argv[2]);
    global_fifo_registo = argv[3];

    if (global_max_games <= 0) {
        fprintf(stderr, "max_games deve ser > 0\n");
        return 1;
    }

    // Inicialização de Logs e Sinais
    open_debug_file("server-debug.log");
    
    // Tratamento de SIGINT/SIGTERM
    struct sigaction sa_term;
    sa_term.sa_handler = handle_server_shutdown;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    // Tratamento de SIGUSR1 (Apenas na Main, Workers bloqueiam)
    struct sigaction sa_usr;
    sa_usr.sa_handler = handle_sigusr1;
    sigemptyset(&sa_usr.sa_mask);
    sa_usr.sa_flags = 0;
    sigaction(SIGUSR1, &sa_usr, NULL);

    // Ignorar SIGPIPE (Evita crash se cliente desconectar abruptamente)
    signal(SIGPIPE, SIG_IGN);

    // Inicialização de Estruturas de Dados
    active_games = calloc(global_max_games, sizeof(board_t*));
    
    sem_init(&sem_empty, 0, MAX_BUFFER_SIZE);
    sem_init(&sem_full, 0, 0);

    // Criar FIFO de Registo
    unlink(global_fifo_registo);
    if (mkfifo(global_fifo_registo, 0666) < 0) {
        perror("Erro ao criar FIFO de registo");
        return 1;
    }

    // Criar Thread Pool
    pthread_t* workers = malloc(sizeof(pthread_t) * global_max_games);
    for (int i = 0; i < global_max_games; i++) {
        int* id = malloc(sizeof(int));
        *id = i;
        if (pthread_create(&workers[i], NULL, worker_thread, id) != 0) {
            perror("Falha ao criar worker thread");
            exit(1);
        }
    }

    debug("Servidor iniciado. Pool de %d threads. Escutando: %s\n", global_max_games, global_fifo_registo);

    // Loop Principal (Produtor)
    while (!server_shutdown) {
        
        int fd = open(global_fifo_registo, O_RDONLY);
            
            if (fd < 0) {
                if (errno == EINTR) {
                    if (sigusr1_pending) {
                        executar_log_top_5(); // Executa a lógica pesada aqui
                        sigusr1_pending = 0;
                    }
                    continue; 
                }
                perror("Erro ao abrir FIFO de registo");
                break; 
            }

            char buffer[1 + 2 * MAX_PIPE_PATH_LENGTH];
            ssize_t n = read(fd, buffer, sizeof(buffer));
            
            if (n < 0) {
                if (errno == EINTR) {
                    if (sigusr1_pending) {
                        executar_log_top_5(); // Também verifica após o read
                        sigusr1_pending = 0;
                    }
                    close(fd);
                    continue; 
                }
            }
        
        if (n > 0 && buffer[0] == (char)OP_CODE_CONNECT) {
            connection_request_t req;
            strncpy(req.req_pipe_path, buffer + 1, MAX_PIPE_PATH_LENGTH);
            strncpy(req.notif_pipe_path, buffer + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
            
            debug("Main: Recebido pedido de conexão. A colocar no buffer...\n");

            // Produtor: Coloca no buffer
            // Se o buffer estiver cheio, sem_wait bloqueia (Backpressure natural)
            if (sem_wait(&sem_empty) == -1 && errno == EINTR) {
                // Se foi interrompido por sinal, tenta de novo ou gere conforme necessário
                close(fd);
                continue; 
            }

            pthread_mutex_lock(&mutex_buffer);
            request_buffer[buf_in] = req;
            buf_in = (buf_in + 1) % MAX_BUFFER_SIZE;
            buf_count++;
            pthread_mutex_unlock(&mutex_buffer);

            sem_post(&sem_full);
        }
        
        close(fd);
    }

    // Limpeza
    unlink(global_fifo_registo);
    free(workers);
    free(active_games);
    close_debug_file();
    return 0;
}
