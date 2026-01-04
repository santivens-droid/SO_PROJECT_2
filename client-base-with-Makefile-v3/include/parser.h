#ifndef PARSER_H
#define PARSER_H

#include "dirent.h"
#include "board.h"
#define MAX_COMMAND_LENGTH 256

int read_line(int fd, char* buffer);
int read_level(board_t* board, char* filename, char* dirname);
int read_pacman(board_t* board, int points);
int read_ghosts(board_t* board);
int filter_levels(const struct dirent *entry);

#endif
