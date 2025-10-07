/*
  Copyright (c) 2025 Ronald de Man
  This file may be redistributed and/or modified without restrictions.
*/

// This code implements simple 0x88-based move generation routines
// to interface with the probing code.
//
// For best performance, engine authors may consider re-implementing the
// TB_Position struct and the TB_...() functions required by tbprobe.c
// using their own board representation and move generation routines.
// Alternatively, they could implement a function to convert their engine's
// board represenation to the TB_Position struct defined below.
//
// Programs such as GUIs can probably use this code directly and use
// TBitf_set_from_fen() to initialize a TB_Position struct from a FEN
// string.
//
// See also tbinterface_bb.c, which uses bitboard-based move generation
// and a somewhat smaller TB_Position struct.

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jtbprobe.h"
#include "jtbinterface.h"

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5)
#define assume(x) do { if (!(x)) __builtin_unreachable(); } while (0)
#else
#define assume(x) do { } while (0)
#endif

#define INLINE static inline __attribute__((always_inline))

enum {
  WHITE = 0,
  BLACK = 1
};

enum {
  PAWN = 1,
  KNIGHT = 2,
  BISHOP = 3,
  ROOK = 4,
  QUEEN = 5,
  KING = 6
};

/* 0x88 attack generation */

static signed char delta[16][16] = {
  { 0 },
  { 15, 17 },
  { -33, -31, -18, -14, 14, 18, 31, 33, 0 },
  { -34, -30, 30, 34, 0 },
  { -16, -1, 1, 16, 0 },
  { -17, -15, 15, 17, 0 },
  { -17, -16, -15, -1, 1, 15, 16, 17, 0 },
  { 0 },
  { 0 },
  { -17, -15 },
  { -33, -31, -18, -14, 14, 18, 31, 33, 0 },
  { -34, -30, 30, 34, 0 },
  { -16, -1, 1, 16, 0 },
  { -17, -15, 15, 17, 0 },
  { -17, -16, -15, -1, 1, 15, 16, 17, 0 },
  { 0 }
};

static bool sliding[16] = {
  false, false, false, false, true, false, false, false,
  false, false, false, false, true, false, false, false
};

#define WPAWN_MASK  (1<<0)
#define BPAWN_MASK  (1<<1)
#define KNIGHT_MASK (1<<2)
#define BISHOP_MASK (1<<3)
#define ROOK_MASK   (1<<4)
#define QUEEN_MASK  (1<<5)
#define KING_MASK   (1<<6)

static uint8_t mask[16] = {
  0, WPAWN_MASK, KNIGHT_MASK, BISHOP_MASK, ROOK_MASK, QUEEN_MASK, KING_MASK, 0,
  0, BPAWN_MASK, KNIGHT_MASK, BISHOP_MASK, ROOK_MASK, QUEEN_MASK, KING_MASK, 0
};

static uint8_t attacks[240];
static int8_t attackDelta[240];

INLINE int from0x88(int sq88)
{
  return (sq88 + (sq88 & 7)) >> 1;
}

void TBitf_init(void)
{
  for (int i = 0; i < 16; i++)
    for (int k = 0; delta[i][k]; k++) {
      int d = delta[i][k];
      if (!sliding[i])
        attacks[120 + d] |= mask[i];
      else
        for (unsigned k = 120 + d, c = 1; k < 240 && c < 8; k += d, c++) {
          attacks[k] |= mask[i];
          attackDelta[k] = d;
        }
    }
}

/* Board and move representation */

struct State {
  int firstMove;
  uint8_t fromSquare;
  int8_t epPiece;
};

typedef uint16_t Move;

// Board representation for up to 7 pieces.
// The number N of pieces is stored in num.
// The squares of the pieces in 0x88 encoding (A1=0, H1=7, A2=16, H8=119)
// are listed in sq[0..N-1].
// The piece types (WPAWN=1, BKING=14) are listed in pt[0..N-1].
// It is mandatory that pt[0]=6 (= WKING) and pt[1]=14 (= BKING).
// The side to move (WHITE=0, BLACK=1) is stored in stm.
// The elements of the board[0..119] array contains the index (0..N-1)
// of the piece occupying the square or -1 if empty.
//
// state[0].epPiece contains the index of the pawn that can be captured
// en passant in the initial position (as set up by the engine or GUI).
// If there is no such pawn, its value is -1.
//
// Note that there is no provision for castling. The tablebase files do
// not store information for positions with castling rights.
//
// Note that the TB_Position struct does not keep track of the 50-move
// clock. It is up to the application to combine DTZ50 information with the
// 50-move clock.
struct TB_Position {
  int8_t bd[120];
  uint8_t sq[8];
  uint8_t pt[8];
  int num;
  int cnt[2];
  int stm;
  int idx;
  int maxIdx;
  int maxMoves;
  struct State *state;
  Move *move;
};

typedef struct TB_Position TB_Position;

#define CAPT_FLAG 0x08
#define PROM_FLAG 0x20

// A capture move encodes the indices of the capturing and captured pieces.
INLINE Move make_capture(int i, int j)
{
  return i | CAPT_FLAG | (j << 8);
}

INLINE Move make_prom_capture(int i, int j)
{
  return i | CAPT_FLAG | PROM_FLAG | (j << 8);
}

// A non-capture move encodes the index of the moving piece and the
// destination square.
INLINE Move make_quiet(int i, int sq)
{
  return i | (sq << 8);
}

INLINE Move make_prom(int i, int sq)
{
  return i | PROM_FLAG | (sq << 8);
}

INLINE bool piece_attacks(TB_Position *pos, int i, int to)
{
  int pt = pos->pt[i];
  int from = pos->sq[i];
  if (!(attacks[120 + to - from] & mask[pt]))
    return false;
  if (!sliding[pt])
    return true;
  int d = attackDelta[120 + to - from];
  do {
    from += d;
    if (from == to)
      return true;
  } while (pos->bd[from] < 0);
  return false;
}

// Allocate a TB_Position struct. Each search thread should have one.
TB_Position *TBitf_alloc_position(void)
{
  TB_Position *pos = malloc(sizeof *pos);
  pos->maxIdx = 7;
  pos->maxMoves = 500;
  pos->state = malloc(pos->maxIdx * sizeof(struct State));
  pos->move = malloc(pos->maxMoves * sizeof(Move));

  return pos;
}

// Free an allocated TB_Position struct
void TBitf_free_position(TB_Position *pos)
{
  free(pos->state);
  free(pos->move);
  free(pos);
}

static const char pieceChar[] = " PNBRQK  pnbrqk";

static const uint64_t matKey[] = {
  0, 0xd8c54b6242cc4658, 0xb84cd5fd6adf1a60, 0x0c8fa4e03da65e01,
  0xa16591be7916b4ac, 0x6e4682f9525cc4c4, 0, 0,
  0, 0x60adc383afac9d1b, 0x97bbdd24afa0b2d1, 0x298cefb2f9bfac89,
  0x6b16a6bc4040b7c2, 0xf7153c5390f198ac, 0, 0
};

uint64_t TB_material_key(TB_Position *pos)
{
  uint64_t key = 0;

  // We skip the two kings
  for (int i = 2; i < pos->num; i++)
    key += matKey[pos->pt[i]];

  return key;
}

uint64_t TB_material_key_from_counts(int whiteCounts[8], int blackCounts[8])
{
  uint64_t key = 0;

  for (int i = PAWN; i <= QUEEN ; i++)
    key +=  whiteCounts[i] * matKey[i]
          + blackCounts[i] * matKey[i + 8];

  return key;
}

void TB_material_string(TB_Position *pos, char str[16])
{
  int cnt[16];

  for (int i = 0; i < 16; i++)
    cnt[i] = 0;
  for (int i = 0; i < pos->num; i++)
    cnt[pos->pt[i]]++;

  int j = 0;
  for (int i = KING; i >= PAWN; i--)
    while (cnt[i]--)
      str[j++] = pieceChar[i];
  str[j++] = 'v';
  for (int i = KING; i >= PAWN; i--)
    while (cnt[8 + i]--)
      str[j++] = pieceChar[i];
  str[j] = 0;
}

void TB_list_squares(TB_Position *pos, const uint8_t *pt, bool flip, int *p)
{
  for (int i = 0; i < pos->num; ) {
    int t = pt[i] ^ (flip << 3);
    for (int j = 0; j < pos->num; j++)
      if (pos->pt[j] == t)
        p[i++] = from0x88(pos->sq[j]) ^ (flip ? 0x38 : 0x00);
  }
}

bool TB_white_to_move(TB_Position *pos)
{
  return pos->stm == WHITE;
}

bool TB_bare_king(TB_Position *pos, int *v)
{
  if (pos->cnt[pos->stm])
    return false;

  // Still a draw if the bare king can capture the opponent's last piece.
  *v =    pos->num == 3
      &&  piece_attacks(pos, pos->stm, pos->sq[2])
      && !piece_attacks(pos, pos->stm ^ 1, pos->sq[2]) ? 0 : -2;

  return true;
}

bool TB_move_is_legal(TB_Position *pos, int m)
{
  if (!TB_do_move(pos, m))
    return false;
  TB_undo_move(pos, m);
  return true;
}

bool TB_no_legal_moves(TB_Position *pos)
{
  int num = TB_generate_captures(pos);
  num = TB_generate_quiets(pos, num);
  for (int m = 0; m < num; m++)
    if (TB_move_is_legal(pos, m))
      return false;
  return true;
}

bool TB_move_is_pawn_move(TB_Position *pos, int m)
{
  Move move = pos->move[pos->state[pos->idx].firstMove + m];
  return (pos->pt[move & 7] & 7) == PAWN;
}

INLINE bool rank18(int sq)
{
  return (unsigned)(sq - 0x10) >= 0x60;
}

int TB_generate_captures(TB_Position *pos)
{
  int m = pos->state[pos->idx].firstMove;

  if (m + 150 >= pos->maxMoves) {
    pos->maxMoves += 500;
    pos->move = realloc(pos->move, pos->maxMoves * sizeof(Move));
  }

  // generate regular captures and captures with promotion
  for (int i = 0; i < pos->num; i++) {
    int pt = pos->pt[i];
    if ((pt >> 3) != pos->stm)
      continue;
    if ((pt & 7) == PAWN) {
      for (int k = 0; k < 2; k++) {
        int sq = pos->sq[i] + delta[pt][k];
        if (sq & 0x88) continue;
        int j = pos->bd[sq];
        if (j < 0 || !((pt ^ pos->pt[j]) & 8))
          continue;
        if (!rank18(sq))
          pos->move[m++] = make_capture(i, j);
        else
          pos->move[m++] = make_prom_capture(i, j);
      }
    }
    else
      for (int j = 0; j < pos->num; j++)
        if (((pt ^ pos->pt[j]) & 8) && piece_attacks(pos, i, pos->sq[j]))
          pos->move[m++] = make_capture(i, j);
  }

  pos->state[pos->idx + 1].firstMove = m;
  return m - pos->state[pos->idx].firstMove;
}

int TB_generate_quiets(TB_Position *pos, int start)
{
  int m = pos->state[pos->idx].firstMove + start;

  if (m + 150 >= pos->maxMoves) {
    pos->maxMoves += 500;
    pos->move = realloc(pos->move, pos->maxMoves * sizeof(Move));
  }

  for (int i = 0; i < pos->num; i++) {
    int pt = pos->pt[i];
    if ((pt >> 3) != pos->stm)
      continue;
    if ((pt & 7) == PAWN) { // pawn moves
      int sq = pos->sq[i];
      int fwd = pos->stm == WHITE ? 0x10 : -0x10;
      if (pos->bd[sq + fwd] < 0) {
        if (!rank18(sq + fwd))
          pos->move[m++] = make_quiet(i, sq + fwd);
        else
          pos->move[m++] = make_prom(i, sq + fwd);
      }
    } else { // non-pawn moves
      if (!sliding[pt])
        for (int k = 0; delta[pt][k]; k++) {
          int sq = pos->sq[i] + delta[pt][k];
          if (!(sq & 0x88) && pos->bd[sq] < 0)
            pos->move[m++] = make_quiet(i, sq);
        }
      else
        for (int k = 0; delta[pt][k]; k++) {
          int sq = pos->sq[i];
          while (!((sq += delta[pt][k]) & 0x88))
            if (pos->bd[sq] < 0)
              pos->move[m++] = make_quiet(i, sq);
            else
              break;
        }
    }
  }

  pos->state[pos->idx + 1].firstMove = m;
  return m - pos->state[pos->idx].firstMove;
}

INLINE bool king_attacked(TB_Position *pos, int stm)
{
  int kingSq = pos->sq[stm];

  for (int i = 0; i < pos->num; i++) {
    if ((pos->pt[i] >> 3) == stm)
      continue;
    if (piece_attacks(pos, i, kingSq))
      return true;
  }
  return false;
} 

INLINE bool opp_king_attacked(TB_Position *pos)
{
  return king_attacked(pos, pos->stm ^ 1);
}

bool TB_in_check(TB_Position *pos)
{
  return king_attacked(pos, pos->stm);
}

bool TB_do_move(TB_Position *pos, int m)
{
  if (pos->idx + 2 >= pos->maxIdx) {
    pos->maxIdx += 100;
    pos->state = realloc(pos->state, pos->maxIdx * sizeof(struct State));
  }

  Move move = pos->move[pos->state[pos->idx].firstMove + m];

  int i = move & 7, j = move >> 8;
  pos->state[pos->idx++].fromSquare = pos->sq[i];
  pos->bd[pos->sq[i]] = -1;
  if (move & PROM_FLAG)
    pos->pt[i] += QUEEN - PAWN;
  if (move & CAPT_FLAG) {
    pos->sq[i] = pos->sq[j];
    pos->bd[pos->sq[i]] = i;
    pos->pt[pos->num--] = pos->pt[j];
    pos->cnt[pos->stm ^ 1]--;
    if (j < pos->num) {
      pos->sq[j] = pos->sq[pos->num];
      pos->pt[j] = pos->pt[pos->num];
      pos->bd[pos->sq[j]] = j;
    }
  } else {
    pos->sq[i] = j;
    pos->bd[j] = i;
  }
  pos->stm ^= 1;

  if (opp_king_attacked(pos)) {
    TB_undo_move(pos, m);
    return false;
  }

  return true;
}

void TB_undo_move(TB_Position *pos, int m)
{
  Move move = pos->move[pos->state[--pos->idx].firstMove + m];

  int i = move & 7, j = move >> 8;
  if (move & CAPT_FLAG) {
    pos->pt[pos->num] = pos->pt[j];
    pos->sq[pos->num] = pos->sq[j];
    pos->bd[pos->sq[pos->num]] = pos->num;
    pos->pt[j] = pos->pt[++pos->num];
    pos->sq[j] = pos->sq[i];
    pos->cnt[pos->stm]++;
    pos->bd[pos->sq[j]] = j;
  } else {
    pos->bd[j] = -1;
  }
  if (move & PROM_FLAG)
    pos->pt[i] -= QUEEN - PAWN;
  pos->sq[i] = pos->state[pos->idx].fromSquare;
  pos->bd[pos->sq[i]] = i;
  pos->stm ^= 1;
}

// Set up a position from a FEN string.
// The halfmove clock is returned to the caller via cnt70.
void TBitf_set_from_fen(TB_Position *pos, const char *fen, int *cnt70)
{
  int f, r;
  char c;

  pos->pt[0] = KING;
  pos->pt[1] = KING + 8;
  pos->sq[0] = pos->sq[1] = 0xff;
  pos->num = 2;
  pos->idx = 0;
  pos->state[0] = (struct State){ 0 };
  *cnt70 = 0;

  for (f = 0, r = 7; (c = *fen) && !isspace(c); fen++) {
    if (c == '/' && r > 0) {
      r--;
      f = 0;
      continue;
    }
    if (f == 8)
      goto illegal_fen;
    if (isdigit(c) && f + (c - '0') <= 8)
      f += c - '0';
    else if (c == 'K' && pos->sq[0] == 0xff)
      pos->sq[0] = (r << 4) | f++;
    else if (c == 'k' && pos->sq[1] == 0xff)
      pos->sq[1] = (r << 4) | f++;
    else if (c == 'K' || c == 'k')
      goto illegal_fen;
    else if (strchr(pieceChar, c)) {
      if (pos->num == 7)
        goto too_many_pieces;
      pos->pt[pos->num] = strchr(pieceChar, c) - pieceChar;
      pos->sq[pos->num++] = (r << 4) | f++;
    }
    else
      goto illegal_fen;
  }
  if (!isspace(c) || r > 0 || pos->sq[0] == 0xff || pos->sq[1] == 0xff)
    goto illegal_fen;

  // side to move
  while (isspace(c = *fen++)) ;
  if (c == 'w')
    pos->stm = WHITE;
  else if (c == 'b')
    pos->stm = BLACK;
  else
    goto illegal_fen;

  // castling rights
  while (isspace(c = *fen++)) ;
  if (!c)
    goto finish;
  if (strchr("kqKQ", c)) {
    fprintf(stderr, "Castling rights not allowed.\n");
    exit(EXIT_FAILURE);
  }
  if (c != '-')
    goto illegal_fen;

  for (int i = 0; i < 120; i++)
    pos->bd[i] = -1;
  for (int i = 0; i < pos->num; i++)
    pos->bd[pos->sq[i]] = i;

  // ep rights
  while (isspace(c = *fen++)) ;
  if (!c)
    goto finish;
  if (c != '-')
    goto illegal_fen;

  // Halfmove clock
  while (isspace(c = *fen++)) ;
  if (!c)
    goto finish;
  while (isdigit(c)) {
    *cnt70 = *cnt70 * 10 + (c - '0');
    c = *fen++;
  }

  // We ignore the fullmove number.

finish:
  pos->cnt[WHITE] = pos->cnt[BLACK] = 0;
  for (int i = 2; i < pos->num; i++)
    pos->cnt[pos->pt[i] >> 3]++;

  if (!opp_king_attacked(pos))
    return;

illegal_fen:
  fprintf(stderr, "Illegal fen.\n");
  exit(EXIT_FAILURE);

too_many_pieces:
  fprintf(stderr, "Too many pieces.\n");
  exit(EXIT_FAILURE);
}

// Convert the mth move into algebraic notation
void TBitf_move_to_string(TB_Position *pos, int m, char *str)
{
  Move move = pos->move[pos->state[pos->idx].firstMove + m];

  int i = move & 7, j = move >> 8;
  int from = pos->sq[i];
  int to = (move & CAPT_FLAG) ? pos->sq[j] : j;
  if ((pos->pt[i] & 7) == PAWN) {
    if (move & CAPT_FLAG)
      *str++ = 'a' + (from & 7);
  } else {
    *str++ = pieceChar[pos->pt[i] & 7];
    bool sameTo = false, sameFile = false, sameRank = false;
    int num = pos->state[pos->idx + 1].firstMove - pos->state[pos->idx].firstMove;
    for (int m1 = 0; m1 < num; m1++) {
      int move1 = pos->move[pos->state[pos->idx].firstMove + m1];
      if (   (move1 & 7) != i
          && (move1 >> 8) == j
          && pos->pt[move1 & 7] == pos->pt[i]
          && TB_do_move(pos, m1))
      {
        TB_undo_move(pos, m1);
        sameTo = true;
        int from1 = pos->sq[move1 & 7];
        if ((from1 & 7) == (from & 7))
          sameFile = true;
        if ((from1 >> 4) == (from >> 4))
          sameRank = true;
      }
    }
    if (sameTo) {
      if (!sameFile || sameRank)
        *str++ = 'a' + (from & 7);
      if (sameFile)
        *str++ = '1' + (from >> 4);
    }
  }
  if (move & CAPT_FLAG)
    *str++ = 'x';
  *str++ = 'a' + (to & 7);
  *str++ = '1' + (to >> 4);
  if (move & PROM_FLAG)
    *str++ = 'Q';
  if (TB_do_move(pos, m)) {
    if (TB_in_check(pos))
      *str++ = TB_no_legal_moves(pos) ? '#' : '+';
    TB_undo_move(pos, m);
  }
  *str = 0;
}

// Convert the mth move to uci notation
void TBitf_move_to_string_uci(TB_Position *pos, int m, char *str)
{
  Move move = pos->move[pos->state[pos->idx].firstMove + m];

  int i = move & 7, j = move >> 8;
  int from = pos->sq[i];
  int to = (move & CAPT_FLAG) ? pos->sq[j] : j;
  *str++ = 'a' + (from & 7);
  *str++ = '1' + (from >> 4);
  *str++ = 'a' + (to & 7);
  *str++ = '1' + (to >> 4);
  *str++ = 'q';
  *str = 0;
}

void TBitf_print_pos(TB_Position *pos)
{
  printf("\n +---+---+---+---+---+---+---+---+\n");

  for (int r = 7; r >= 0; r--) {
    for (int f = 0; f <= 7; f++) {
      int i = pos->bd[(r<<4) + f];
      char c = i < 0 ? ' ' : pieceChar[pos->pt[i]];
      printf(" | %c", c);
    }

    printf(" | %d\n +---+---+---+---+---+---+---+---+\n", r + 1);
  }

  printf("   a   b   c   d   e   f   g   h\n\n");
}
