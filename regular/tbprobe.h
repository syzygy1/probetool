/*
  Copyright (c) 2025 Ronald de Man
  This file may be redistributed and/or modified without restrictions.
*/

#ifndef TBPROBE_H
#define TBPROBE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TB_WDL, TB_DTM, TB_DTZ };

// Number of detected WDL, DTM and DTZ tables
extern int TB_NumTables[3];

// Counts of actual probes, i.e. accesses of compressed tablebase data.
// (Not intended to be reported to the user by an engine.)
extern uint64_t TB_ProbeCount[3];

// No need to try to probe if the number of pieces > MaxCardinality
extern int TB_MaxCardinality[3];

// Forward declaration of the TB_Position struct. This struct is not
// declared here but by the code interfacing with the probing code.
struct TB_Position;
typedef struct TB_Position TB_Position;

// Functions implemented by the probing code:

// TB_init(path) is called to initialize the probing code. It will look
// in the directories listed in 'path' for tablebases files. TB_init() may
// be called multiple times to "reload" the tables from a different path.
void TB_init(const char *path);

// TB_release() will release any mmap()ed tablebase files, reducing memory
// usage. This could be called between games, for example.
void TB_release(void);

// Clean up everything.
void TB_free(void);

// Probe the WDL table for a particular position.
//
// The caller should verify that the probe was successfull by checking
// the value *succes.
//
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
int TB_probe_wdl(TB_Position *pos, bool *success);

// Probe the DTZ table for a particular position.
// If *success true, the probe was successful.
// The return value is from the point of view of the side to move:
//         n < -100 : loss, but draw under 50-move rule
// -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
//         0        : draw
//     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
//   100 < n        : win, but draw under 50-move rule
//
// If the position mate, -1 is returned instead of 0.
//
// The return value n can be off by 1: a return value -n can mean a loss
// in n+1 ply and a return value +n can mean a win in n+1 ply. This
// cannot happen for tables with positions exactly on the "edge" of
// the 50-move rule.
//
// This means that if dtz > 0 is returned, the position is certainly
// a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
// picks moves that preserve dtz + 50-move-counter <= 99.
//
// If n = 100 immediately after a capture or pawn move, then the position
// is also certainly a win, and during the whole phase until the next
// capture or pawn move, the inequality to be preserved is
// dtz + 50-movecounter <= 100.
//
// In short, if a move is available resulting in dtz + 50-move-counter <= 99,
// then do not accept moves leading to dtz + 50-move-counter == 100.
int TB_probe_dtz(TB_Position *pos, bool *success);

// Probe the DTM table for a non-drawn position.
// 'won' must be true if the position is a win or cursed win and
// false if the position is a loss or blessed loss.
// The value returned is 10000 - (#ply to mate) if the position is
// winning and -10000 + (#ply to mate) if the position is losing.
//
// NOTE: Syzygy DTM tables have not yet been released. So this function
// can be ignored for now.
int TB_probe_dtm(TB_Position *pos, bool winning, bool *success);

// Test whether the current position is a DTM-optimal successor of the
// parent position. The (signed) dtm value passed must be the expected
// DTM value of a DTM-optimal succesor. If the parent position has DTM
// value d, then pass (d > 0) - d.
bool TB_probe_dtm_test(TB_Position *pos, int dtm, bool *success);


/********************* Functions required by tbprobe.c *********************/
//
// The code interfacing with the tablebase probing code must provide a
// definition of the TB_Position struct and implement the following functions.
// Example implementations can be found in tbinterface_bb/_0x88.c.

// Return a 64-bit key identifying the combination of material on the board.
uint64_t TB_material_key(TB_Position *pos);

// Return the same key but generated from a list of piece counts.
// PAWN = 1, KNIGHT = 2, ..., KING = 6, as usual.
uint64_t TB_material_key_from_counts(int whiteCounts[8], int blackCounts[8]);

// Produce a text string of the form KQPvKRP, where "KQP" represents the
// white pieces and "KRP" represents the black pieces.
void TB_material_string(TB_Position *pos, char str[16]);

// List the squares (A1=1, H1=8, H8=63) of the pieces on the board in the
// output array p[] in the order indicated by the input array of piece
// types pt[] (WPAWN = 1, BKING = 14). Identical piece types are always
// listed in pt[] consecutively.
// If 'flip' is true, then the function must flip both the color of the
// piece types read from pt[] (^0x08) and the squares stored in p[] (^0x38).
void TB_list_squares(TB_Position *pos, const uint8_t *pt, bool flip, int *p);

// Generate all (pseudo-)legal captures, including promotions and
// underpromotions, and store them inside or in association with the
// TB_Position data structure as a list or array of moves numbered 0..M-1.
// Return the number M of generated moves.
int TB_generate_captures(TB_Position *pos);

// Same for quiet moves, including non-capture promotions and underpromotions.
// The value of start will be either 0, in which case the number of generated
// quiet moves is to be returned, or the number of already generated captures,
// in which case the quiet moves will be appended to the list of captures and
// the total number of moves will be returned.
int TB_generate_quiets(TB_Position *pos, int start);

// Test whether the mth generated move (0 <= m < M) is legal.
bool TB_move_is_legal(TB_Position *pos, int m);

// Test whether the mth move is legal and do the move if it is.
bool TB_do_move(TB_Position *pos, int m);

// Undo the mth move (only if it was legal).
void TB_undo_move(TB_Position *pos, int m);

// Test whether it is white's turn to make a move.
bool TB_white_to_move(TB_Position *pos);

// Test whether only the two kings are left on the board.
bool TB_bare_kings(TB_Position *pos);

// Test whether the side to move is in check.
bool TB_in_check(TB_Position *pos);

// Test whether the position has an en passant capture. It is OK to
// return true as false positive (e.g. after a double pawn push).
bool TB_has_en_passant(TB_Position *pos);

// Test whether the mth move is an en passant capture (it need not be
// legal).
bool TB_move_is_ep(TB_Position *pos, int m);

// Test whether the mth move is a pawn move.
bool TB_move_is_pawn_move(TB_Position *pos, int m);

// Test whether the position has no legal moves.
bool TB_no_legal_moves(TB_Position *pos);

#ifdef __cplusplus
}
#endif

#endif
