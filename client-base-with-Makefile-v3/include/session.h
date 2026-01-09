#ifndef SESSION_H
#define SESSION_H

#include "board.h"

// active_game_slot: ponteiro para o slot no array global do main.c
void start_session(char* levels_dir, char* req_path, char* notif_path, board_t** active_game_slot);

#endif