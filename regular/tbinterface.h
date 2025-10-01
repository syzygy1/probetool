#ifndef TBINTERFACE_H
#define TBINTERFACE_H

#include "tbprobe.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize data structures
void TBitf_init(void);

// Allocate a TB_Position instance
TB_Position *TBitf_alloc_position(void);

// Free a TB_Position instance
void TBitf_free_position(TB_Position *pos);

// Initialize a TB_Position instance from a fen
void TBitf_set_from_fen(TB_Position *pos, const char *fen, int *cnt50);

// Convert a move to algebraic notation
void TBitf_move_to_string(TB_Position *pos, int m, char *str);

// Convert a move to uci notation
void TBitf_move_to_string_uci(TB_Position *pos, int m, char *str);

// Print an ASCII representation of the position to stdout
void TBitf_print_pos(TB_Position *pos);

#ifdef __cplusplus
}
#endif

#endif
