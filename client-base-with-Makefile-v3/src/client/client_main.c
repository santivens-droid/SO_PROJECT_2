#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

typedef struct {
    const char *filename;
} auto_move_args;

Board board;
bool stop_execution = false;
int session_tempo = 500; 
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// --- THREAD DE RECEÇÃO ---
static void *receiver_thread(void *arg) {
    (void)arg;
    while (true) {
        Board updated_board = receive_board_update();
        if (!updated_board.data || updated_board.game_over == 1) {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_lock(&mutex);
        session_tempo = updated_board.tempo;
        pthread_mutex_unlock(&mutex);

        draw_board_client(updated_board);
        refresh_screen();
        if (updated_board.data) free(updated_board.data);
    }
    return NULL;
}

// --- THREAD DE MOVIMENTO AUTOMÁTICO ---
void* client_auto_move_thread(void* arg) {
    auto_move_args* a = (auto_move_args*)arg;
    FILE* fp = fopen(a->filename, "r");
    if (!fp) return NULL;

    char line[256];
    while (true) {
        pthread_mutex_lock(&mutex);
        if (stop_execution) { pthread_mutex_unlock(&mutex); break; }
        pthread_mutex_unlock(&mutex);

        if (fgets(line, sizeof(line), fp) == NULL) {
            rewind(fp);
            continue;
        }

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;
        if (strncmp(line, "POS", 3) == 0 || strncmp(line, "PASSO", 5) == 0) continue;

        for (int i = 0; line[i] != '\0'; i++) {
            char cmd = (char)toupper((unsigned char)line[i]);
            if (cmd == 'W' || cmd == 'A' || cmd == 'S' || cmd == 'D') {
                pthread_mutex_lock(&mutex);
                if (stop_execution) { pthread_mutex_unlock(&mutex); goto end; }
                int wait = session_tempo;
                pthread_mutex_unlock(&mutex);

                pacman_play(cmd);
                sleep_ms(wait);
            }
        }
    }
end:
    fclose(fp);
    return NULL;
}

// --- MAIN ---
int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <id> <reg_pipe> [cmd_file]\n", argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    char req_path[MAX_PIPE_PATH_LENGTH], notif_path[MAX_PIPE_PATH_LENGTH];
    snprintf(req_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_request", client_id);
    snprintf(notif_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    if (pacman_connect(req_path, notif_path, register_pipe) != 0) return 1;

    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, receiver_thread, NULL);

    pthread_t auto_tid;
    auto_move_args a_args = {commands_file};
    bool has_auto = (commands_file != NULL);

    if (has_auto) {
        pthread_create(&auto_tid, NULL, client_auto_move_thread, &a_args);
    }

    terminal_init();
    set_timeout(100);

    while (1) {
        pthread_mutex_lock(&mutex);
        if (stop_execution) { pthread_mutex_unlock(&mutex); break; }
        pthread_mutex_unlock(&mutex);

        int ch = get_input();
        if (ch == -1) continue;

        char cmd = (char)toupper(ch);

        // A tecla 'Q' funciona SEMPRE (emergência/saída)
        if (cmd == 'Q') {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        // --- LÓGICA DE BLOQUEIO DO TECLADO ---
        // Se houver um ficheiro (has_auto == true), ignoramos W,A,S,D do teclado
        if (!has_auto) {
            if (cmd == 'W' || cmd == 'A' || cmd == 'S' || cmd == 'D') {
                pacman_play(cmd);
            }
        }
    }

    pacman_disconnect();
    pthread_join(recv_tid, NULL);
    if (has_auto) pthread_join(auto_tid, NULL);

    pthread_mutex_destroy(&mutex);
    terminal_cleanup();
    return 0;
}