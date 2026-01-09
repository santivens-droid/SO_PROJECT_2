#ifndef SESSION_H
#define SESSION_H

#include "board.h" // Necessário para conhecer o tipo board_t

// A nova assinatura deve receber o board_t*
void start_session(char* levels_dir, char* req_path, char* notif_path, board_t* board_buffer);

#endif