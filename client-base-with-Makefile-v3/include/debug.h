#ifndef DEBUG_H
#define DEBUG_H
#include <stdio.h>
extern FILE *debugfile;
// DEBUG FILE

void open_debug_file(char *filename);

void close_debug_file();

void debug(const char * format, ...);

void sleep_ms(int milliseconds);

#endif