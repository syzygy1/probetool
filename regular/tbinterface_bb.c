/*
  Copyright (c) 2025 Ronald de Man
  This file may be redistributed and/or modified without restrictions.
*/

// This code implements simple bitboard-based move generation routines
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
// See also tbinterface_0x88.c, which uses 0x88-based move generation.
// Lighter on memory, but it requires initializing a TB_Position struct
// which is a bit larger.

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbprobe.h"
#include "tbinterface.h"

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

/* Magic sliding attack generation */

typedef uint64_t Bitboard;

INLINE int popcount(Bitboard b)
{
  return __builtin_popcountll(b);
}

INLINE int lsb(Bitboard b)
{
  return __builtin_ctzll(b);
}

INLINE int pop_lsb(Bitboard *b)
{
  int s = lsb(*b);
  *b &= *b - 1;
  return s;
}

struct Magic {
  Bitboard *data;
  Bitboard mask;
  uint64_t magic;
};

static Bitboard AttacksTable[88772];
static struct Magic BishopMagic[64];
static struct Magic RookMagic[64];

static Bitboard Bit[64];

static Bitboard PawnAttacks[2][64];
static Bitboard KnightAttacks[64];
static Bitboard KingAttacks[64];

INLINE Bitboard bit(int sq)
{
  return Bit[sq];
}

INLINE Bitboard bishop_attacks(int sq, Bitboard occ)
{
  struct Magic *mag = &BishopMagic[sq];
  return mag->data[((occ & mag->mask) * mag->magic) >> (64-9)];
}

INLINE Bitboard rook_attacks(int sq, Bitboard occ)
{
  struct Magic *mag = &RookMagic[sq];
  return mag->data[((occ & mag->mask) * mag->magic) >> (64-12)];
}

INLINE Bitboard queen_attacks(int sq, Bitboard occ)
{
  return bishop_attacks(sq, occ) | rook_attacks(sq, occ);
}

INLINE Bitboard knight_attacks(int sq)
{
  return KnightAttacks[sq];
}

INLINE Bitboard king_attacks(int sq)
{
  return KingAttacks[sq];
}

INLINE Bitboard pawn_attacks(int c, int sq)
{
  return PawnAttacks[c][sq];
}

struct MagicInit {
  Bitboard magic;
  int index;
};

// Fixed-shift magics found by Volker Annuss
// from: http://talkchess.com/forum/viewtopic.php?p=727500#727500

static struct MagicInit BishopInit[64] = {
  { 0x007fbfbfbfbfbfffu,   5378 },
  { 0x0000a060401007fcu,   4093 },
  { 0x0001004008020000u,   4314 },
  { 0x0000806004000000u,   6587 },
  { 0x0000100400000000u,   6491 },
  { 0x000021c100b20000u,   6330 },
  { 0x0000040041008000u,   5609 },
  { 0x00000fb0203fff80u,  22236 },
  { 0x0000040100401004u,   6106 },
  { 0x0000020080200802u,   5625 },
  { 0x0000004010202000u,  16785 },
  { 0x0000008060040000u,  16817 },
  { 0x0000004402000000u,   6842 },
  { 0x0000000801008000u,   7003 },
  { 0x000007efe0bfff80u,   4197 },
  { 0x0000000820820020u,   7356 },
  { 0x0000400080808080u,   4602 },
  { 0x00021f0100400808u,   4538 },
  { 0x00018000c06f3fffu,  29531 },
  { 0x0000258200801000u,  45393 },
  { 0x0000240080840000u,  12420 },
  { 0x000018000c03fff8u,  15763 },
  { 0x00000a5840208020u,   5050 },
  { 0x0000020008208020u,   4346 },
  { 0x0000804000810100u,   6074 },
  { 0x0001011900802008u,   7866 },
  { 0x0000804000810100u,  32139 },
  { 0x000100403c0403ffu,  57673 },
  { 0x00078402a8802000u,  55365 },
  { 0x0000101000804400u,  15818 },
  { 0x0000080800104100u,   5562 },
  { 0x00004004c0082008u,   6390 },
  { 0x0001010120008020u,   7930 },
  { 0x000080809a004010u,  13329 },
  { 0x0007fefe08810010u,   7170 },
  { 0x0003ff0f833fc080u,  27267 },
  { 0x007fe08019003042u,  53787 },
  { 0x003fffefea003000u,   5097 },
  { 0x0000101010002080u,   6643 },
  { 0x0000802005080804u,   6138 },
  { 0x0000808080a80040u,   7418 },
  { 0x0000104100200040u,   7898 },
  { 0x0003ffdf7f833fc0u,  42012 },
  { 0x0000008840450020u,  57350 },
  { 0x00007ffc80180030u,  22813 },
  { 0x007fffdd80140028u,  56693 },
  { 0x00020080200a0004u,   5818 },
  { 0x0000101010100020u,   7098 },
  { 0x0007ffdfc1805000u,   4451 },
  { 0x0003ffefe0c02200u,   4709 },
  { 0x0000000820806000u,   4794 },
  { 0x0000000008403000u,  13364 },
  { 0x0000000100202000u,   4570 },
  { 0x0000004040802000u,   4282 },
  { 0x0004010040100400u,  14964 },
  { 0x00006020601803f4u,   4026 },
  { 0x0003ffdfdfc28048u,   4826 },
  { 0x0000000820820020u,   7354 },
  { 0x0000000008208060u,   4848 },
  { 0x0000000000808020u,  15946 },
  { 0x0000000001002020u,  14932 },
  { 0x0000000401002008u,  16588 },
  { 0x0000004040404040u,   6905 },
  { 0x007fff9fdf7ff813u,  16076 } 
};

static struct MagicInit RookInit[64] = {
  { 0x00280077ffebfffeu,  26304 },
  { 0x2004010201097fffu,  35520 },
  { 0x0010020010053fffu,  38592 },
  { 0x0040040008004002u,   8026 },
  { 0x7fd00441ffffd003u,  22196 },
  { 0x4020008887dffffeu,  80870 },
  { 0x004000888847ffffu,  76747 },
  { 0x006800fbff75fffdu,  30400 },
  { 0x000028010113ffffu,  11115 },
  { 0x0020040201fcffffu,  18205 },
  { 0x007fe80042ffffe8u,  53577 },
  { 0x00001800217fffe8u,  62724 },
  { 0x00001800073fffe8u,  34282 },
  { 0x00001800e05fffe8u,  29196 },
  { 0x00001800602fffe8u,  23806 },
  { 0x000030002fffffa0u,  49481 },
  { 0x00300018010bffffu,   2410 },
  { 0x0003000c0085fffbu,  36498 },
  { 0x0004000802010008u,  24478 },
  { 0x0004002020020004u,  10074 },
  { 0x0001002002002001u,  79315 },
  { 0x0001001000801040u,  51779 },
  { 0x0000004040008001u,  13586 },
  { 0x0000006800cdfff4u,  19323 },
  { 0x0040200010080010u,  70612 },
  { 0x0000080010040010u,  83652 },
  { 0x0004010008020008u,  63110 },
  { 0x0000040020200200u,  34496 },
  { 0x0002008010100100u,  84966 },
  { 0x0000008020010020u,  54341 },
  { 0x0000008020200040u,  60421 },
  { 0x0000820020004020u,  86402 },
  { 0x00fffd1800300030u,  50245 },
  { 0x007fff7fbfd40020u,  76622 },
  { 0x003fffbd00180018u,  84676 },
  { 0x001fffde80180018u,  78757 },
  { 0x000fffe0bfe80018u,  37346 },
  { 0x0001000080202001u,    370 },
  { 0x0003fffbff980180u,  42182 },
  { 0x0001fffdff9000e0u,  45385 },
  { 0x00fffefeebffd800u,  61659 },
  { 0x007ffff7ffc01400u,  12790 },
  { 0x003fffbfe4ffe800u,  16762 },
  { 0x001ffff01fc03000u,      0 },
  { 0x000fffe7f8bfe800u,  38380 },
  { 0x0007ffdfdf3ff808u,  11098 },
  { 0x0003fff85fffa804u,  21803 },
  { 0x0001fffd75ffa802u,  39189 },
  { 0x00ffffd7ffebffd8u,  58628 },
  { 0x007fff75ff7fbfd8u,  44116 },
  { 0x003fff863fbf7fd8u,  78357 },
  { 0x001fffbfdfd7ffd8u,  44481 },
  { 0x000ffff810280028u,  64134 },
  { 0x0007ffd7f7feffd8u,  41759 },
  { 0x0003fffc0c480048u,   1394 },
  { 0x0001ffffafd7ffd8u,  40910 },
  { 0x00ffffe4ffdfa3bau,  66516 },
  { 0x007fffef7ff3d3dau,   3897 },
  { 0x003fffbfdfeff7fau,   3930 },
  { 0x001fffeff7fbfc22u,  72934 },
  { 0x0000020408001001u,  72662 },
  { 0x0007fffeffff77fdu,  56325 },
  { 0x0003ffffbf7dfeecu,  66501 },
  { 0x0001ffff9dffa333u,  14826 } 
};

static signed char PawnDelta[2][2][2] = {
  { {  7,  15 }, {  9,  17 } },
  { { -9, -17 }, { -7, -15 } }
};

static signed char KnightDelta[8][2] = {
  { -17, -33 }, { -15, -31 }, { -10, -18 }, { -6, -14 },
  {   6,  14 }, {  10,  18 }, {  15,  31 }, { 17,  33 }
};

static signed char BishopDelta[4][2] = {
  { -9, -17 }, { -7, -15 }, { 7, 15 }, { 9, 17 }
};

static signed char RookDelta[4][2] = {
  { -8, -16 }, { -1, -1 }, { 1, 1 }, { 8, 16 }
};

static signed char KingDelta[8][2] = {
  { -9, -17 }, { -8, -16 }, { -7, -15 }, { -1, -1 },
  {  1,   1 }, {  7,  15 }, {  8,  16 }, {  9, 17 }
};

INLINE bool valid(int sq, signed char delta[2])
{
  int sq88 = sq + (sq & ~7);
  return !((sq88 + delta[1]) & 0x88);
}

static Bitboard calc_attacks(int sq, signed char delta[][2], int num)
{
  Bitboard bb = 0;

  for (int d = 0; d < num; d++)
    if (valid(sq, delta[d]))
      bb |= bit(sq + delta[d][0]);

  return bb;
}

static void init_magics(struct MagicInit *magicInit, struct Magic *magic,
    signed char delta[][2], int shift)
{
  for (int sq = 0; sq < 64; sq++) {
    magic[sq].magic = magicInit[sq].magic;
    magic[sq].data = &AttacksTable[magicInit[sq].index];

    // Calculate mask
    Bitboard mask = 0;
    for (int i = 0; i < 4; i++) {
      if (!valid(sq, delta[i])) continue;
      for (int s = sq + delta[i][0]; valid(s, delta[i]); s += delta[i][0])
        mask |= bit(s);
    }
    magic[sq].mask = mask;

    // Use Carry-Rippler trick
    Bitboard b  = 0;
    do {
      Bitboard attacks = 0;
      for (int j = 0; j < 4; j++)
        for (int s = sq; valid(s, delta[j]); s += delta[j][0]) {
          attacks |= bit(s + delta[j][0]);
          if (b & bit(s + delta[j][0])) break;
        }
      magic[sq].data[(b * magic[sq].magic) >> shift] = attacks;
      b = (b - mask) & mask;
    } while (b);
  }
}

// Call once to initialize the move-generation arrays.
void TBitf_init(void)
{
  for (int sq = 0; sq < 64; sq++)
    Bit[sq] = 1ULL << sq;

  for (int sq = 0; sq < 64; sq++) {
    PawnAttacks[WHITE][sq] = calc_attacks(sq, PawnDelta[WHITE], 2);
    PawnAttacks[BLACK][sq] = calc_attacks(sq, PawnDelta[BLACK], 2);
    KnightAttacks[sq]      = calc_attacks(sq, KnightDelta,      8);
    KingAttacks[sq]        = calc_attacks(sq, KingDelta,        8);
  }

  init_magics(BishopInit, BishopMagic, BishopDelta, 64 - 9);
  init_magics(RookInit,   RookMagic,   RookDelta,   64 - 12);
}

INLINE Bitboard piece_attacks(int pt, int sq, Bitboard occ)
{
  switch (pt & 7) {
  case KNIGHT:
    return knight_attacks(sq);
  case BISHOP:
    return bishop_attacks(sq, occ);
  case ROOK:
    return rook_attacks(sq, occ);
  case QUEEN:
    return queen_attacks(sq, occ);
  case KING:
    return king_attacks(sq);
  }
  assume(0);
}

INLINE Bitboard attacks(int pt, int sq, Bitboard occ)
{
  if ((pt & 7) == PAWN)
    return pawn_attacks(pt >> 3, sq);
  else
    return piece_attacks(pt, sq, occ);
}

/* Board and move representation */

struct State {
  int firstMove;
  uint8_t fromSquare;
  int8_t epPiece;
};

typedef uint16_t Move;

// Board representation for up to 8 pieces.
// The number N of pieces is stored in num.
// The squares of the pieces (A1=0, H1=7, H8=63) are listed in sq[0..N-1].
// The piece types (WPAWN=1, BKING=14) are listed in pt[0..N-1].
// It is mandatory that pt[0]=6 (= WKING) and pt[1]=14 (= BKING).
// The side to move (WHITE=0, BLACK=1) is stored in stm.
// A bitboard indicating the occupied(=1) squares is stored in occ.
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
  uint64_t occ;
  uint8_t sq[9];
  uint8_t pt[9];
  int num;
  int stm;
  int idx;
  int maxIdx;
  int maxMoves;
  struct State *state;
  Move *move;
};

typedef struct TB_Position TB_Position;

#define CAPT_FLAG 0x08
#define EP_FLAG   0x10
#define PROM_FLAG 0x20
#define PROM_MASK 0xc0

// A capture move encodes the indices of the capturing and captured pieces.
INLINE Move make_capture(int i, int j)
{
  return i | CAPT_FLAG | (j << 8);
}

INLINE Move make_ep_capture(int i, int j)
{
  return i | CAPT_FLAG | EP_FLAG | (j << 8);
}

INLINE Move make_prom_capture(int i, int j, int prom_type)
{
  return i | CAPT_FLAG | PROM_FLAG | ((prom_type - KNIGHT) << 6) | (j << 8);
}

// A non-capture move encodes the index of the moving piece and the
// destination square.
INLINE Move make_quiet(int i, int sq)
{
  return i | (sq << 8);
}

INLINE Move make_prom(int i, int sq, int prom_type)
{
  return i | PROM_FLAG | ((prom_type - KNIGHT) << 6) | (sq << 8);
}

// Allocate a TB_Position struct. Each search thread should have one.
TB_Position *TBitf_alloc_position(void)
{
  TB_Position *pos = malloc(sizeof *pos);
  pos->maxIdx = 8;
  pos->maxMoves = 500;
  pos->state = malloc(pos->maxIdx * sizeof(struct State));
  pos->move = malloc(pos->maxMoves * sizeof(Move));

  return pos;
}

// Free an allocated TB_Position struct.
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

  // We can skip the two kings
  for (int i = 2; i < pos->num; i++)
    key += matKey[pos->pt[i]];

  return key;
}

uint64_t TB_material_key_from_counts(int whiteCounts[8], int blackCounts[8])
{
  uint64_t key = 0;

  for (int i = PAWN; i <= QUEEN; i++)
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

void TB_list_squares(TB_Position *pos, const uint8_t *restrict pt, bool flip,
    uint8_t *restrict p)
{
  for (int i = 0; i < pos->num; ) {
    int t = pt[i] ^ (flip << 3);
    for (int j = 0; j < pos->num; j++)
      if (pos->pt[j] == t)
        p[i++] = pos->sq[j] ^ (flip ? 0x38 : 0x00);
  }
}

bool TB_white_to_move(TB_Position *pos)
{
  return pos->stm == WHITE;
}

bool TB_bare_kings(TB_Position *pos)
{
  return pos->num == 2;
}

bool TB_has_en_passant(TB_Position *pos)
{
  return pos->state[pos->idx].epPiece >= 0;
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

bool TB_move_is_ep(TB_Position *pos, int m)
{
  Move move = pos->move[pos->state[pos->idx].firstMove + m];
  return move & EP_FLAG;
}

bool TB_move_is_pawn_move(TB_Position *pos, int m)
{
  Move move = pos->move[pos->state[pos->idx].firstMove + m];
  return (pos->pt[move & 7] & 7) == PAWN;
}

INLINE int piece_idx(TB_Position *pos, int sq)
{
  for (int i = 0; i < pos->num; i++)
    if (pos->sq[i] == sq)
      return i;
  assume(false); // signal to the compiler that this path is unreachable
  return 0;
}

INLINE bool rank18(int sq)
{
  return (unsigned)(sq - 8) >= 48;
}

int TB_generate_captures(TB_Position *pos)
{
  int m = pos->state[pos->idx].firstMove;

  if (m + 150 >= pos->maxMoves) {
    pos->maxMoves += 500;
    pos->move = realloc(pos->move, pos->maxMoves * sizeof(Move));
  }

  // generate ep captures
  if (pos->state[pos->idx].epPiece >= 0) {
    int epSquare = pos->sq[pos->state[pos->idx].epPiece] ^ 8;
    Bitboard b = pawn_attacks(pos->stm ^ 1, epSquare) & pos->occ;
    while (b) {
      int sq = pop_lsb(&b);
      int i = piece_idx(pos, sq);
      if (pos->pt[i] == PAWN + (pos->stm << 3))
        pos->move[m++] = make_ep_capture(i, pos->state[pos->idx].epPiece);
    }
  }

  // generate regular captures and captures with promotion
  for (int i = 0; i < pos->num; i++) {
    if ((pos->pt[i] >> 3) != pos->stm)
      continue;
    if ((pos->pt[i] & 7) == PAWN) {
      Bitboard b = pawn_attacks(pos->pt[i] >> 3, pos->sq[i]) & pos->occ;
      while (b) {
        int sq = pop_lsb(&b);
        int j = piece_idx(pos, sq);
        if (!((pos->pt[i] ^ pos->pt[j]) & 8))
          continue;
        if (!rank18(sq))
          pos->move[m++] = make_capture(i, j);
        else {
          pos->move[m++] = make_prom_capture(i, j, QUEEN);
          pos->move[m++] = make_prom_capture(i, j, KNIGHT);
          pos->move[m++] = make_prom_capture(i, j, ROOK);
          pos->move[m++] = make_prom_capture(i, j, BISHOP);
        }
      }
    }
    else {
      Bitboard b = piece_attacks(pos->pt[i], pos->sq[i], pos->occ) & pos->occ;
      while (b) {
        int sq = pop_lsb(&b);
        int j = piece_idx(pos, sq);
        if ((pos->pt[i] ^ pos->pt[j]) & 0x08)
          pos->move[m++] = make_capture(i, j);
      }
    }
  }

  pos->state[pos->idx + 1].firstMove = m;
  return m - pos->state[pos->idx].firstMove;
}

int TB_generate_quiets(TB_Position *pos, int start)
{
  int m = pos->state[pos->idx].firstMove + start;
  Bitboard b;

  if (m + 150 >= pos->maxMoves) {
    pos->maxMoves += 500;
    pos->move = realloc(pos->move, pos->maxMoves * sizeof(Move));
  }

  for (int i = 0; i < pos->num; i++) {
    if ((pos->pt[i] >> 3) != pos->stm)
      continue;
    if ((pos->pt[i] & 7) == PAWN) { // pawn moves
      int sq = pos->sq[i];
      int fwd = pos->stm == WHITE ? 8 : -8;
      if (!(bit(sq + fwd) & pos->occ)) {
        if (!rank18(sq + fwd)) {
          pos->move[m++] = make_quiet(i, sq + fwd);
          if (rank18(sq - fwd) && (!(bit(sq ^ 16) & pos->occ)))
            pos->move[m++] = make_quiet(i, sq ^ 16);
        } else {
          pos->move[m++] = make_prom(i, sq + fwd, QUEEN);
          pos->move[m++] = make_prom(i, sq + fwd, KNIGHT);
          pos->move[m++] = make_prom(i, sq + fwd, ROOK);
          pos->move[m++] = make_prom(i, sq + fwd, BISHOP);
        }
      }
    } else { // non-pawn moves
      b = piece_attacks(pos->pt[i], pos->sq[i], pos->occ) & ~pos->occ;
      while (b) {
        int sq = pop_lsb(&b);
        pos->move[m++] = make_quiet(i, sq);
      }
    }
  }

  pos->state[pos->idx + 1].firstMove = m;
  return m - pos->state[pos->idx].firstMove;
}

INLINE bool king_attacked(TB_Position *pos, int stm)
{
  Bitboard b = bit(pos->sq[stm]); // square of king
  for (int i = 0; i < pos->num; i++)
    if (   (pos->pt[i] >> 3) == (stm ^ 1)
        && (attacks(pos->pt[i], pos->sq[i], pos->occ) & b))
      return true;
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
  pos->state[pos->idx].fromSquare = pos->sq[i];
  pos->state[++pos->idx].epPiece = -1;
  pos->occ ^= bit(pos->sq[i]);
  if (move & PROM_FLAG)
    pos->pt[i] += KNIGHT - PAWN + ((move & PROM_MASK) >> 6);
  if (move & CAPT_FLAG) {
    pos->sq[i] = pos->sq[j];
    if (move & EP_FLAG) {
      pos->occ ^= bit(pos->sq[i]) ^ bit(pos->sq[i] ^ 8);
      pos->sq[i] ^= 8;
    }
    pos->pt[pos->num--] = pos->pt[j];
    pos->sq[j] = pos->sq[pos->num];
    pos->pt[j] = pos->pt[pos->num];
  } else {
    if ((pos->pt[i] & 7) == PAWN && (pos->sq[i] ^ j) == 16)
      pos->state[pos->idx].epPiece = i;
    pos->sq[i] = j;
    pos->occ ^= bit(j);
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
    pos->pt[j] = pos->pt[++pos->num];
    pos->sq[j] = pos->sq[i];
    if (move & EP_FLAG) {
      pos->occ ^= bit(pos->sq[j]) ^ bit(pos->sq[j] ^ 8);
      pos->sq[j] ^= 8;
    }
  } else {
    pos->occ ^= bit(j);
  }
  if (move & PROM_FLAG)
    pos->pt[i] = (pos->pt[i] & 8) | PAWN;
  pos->sq[i] = pos->state[pos->idx].fromSquare;
  pos->occ ^= bit(pos->sq[i]);
  pos->stm ^= 1;
}

// Set up a position from a FEN string.
// The halfmove clock is returned to the caller via cnt50.
void TBitf_set_from_fen(TB_Position *pos, const char *fen, int *cnt50)
{
  int f, r;
  char c;

  pos->pt[0] = KING;
  pos->pt[1] = KING + 8;
  pos->sq[0] = pos->sq[1] = 0xff;
  pos->num = 2;
  pos->idx = 0;
  pos->state[0] = (struct State){ .firstMove = 0, .epPiece = -1 };
  *cnt50 = 0;

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
      pos->sq[0] = (r << 3) | f++;
    else if (c == 'k' && pos->sq[1] == 0xff)
      pos->sq[1] = (r << 3) | f++;
    else if (c == 'K' || c == 'k')
      goto illegal_fen;
    else if (strchr(pieceChar, c)) {
      if (pos->num == 8)
        goto too_many_pieces;
      pos->pt[pos->num] = strchr(pieceChar, c) - pieceChar;
      pos->sq[pos->num++] = (r << 3) | f++;
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

  // ep rights
  while (isspace(c = *fen++)) ;
  if (!c)
    goto finish;
  if (c != '-') {
    int f = c - 'a';
    if (f < 0 || f > 7)
      goto illegal_fen;
    c = *fen++;
    if (c != (pos->stm == WHITE ? '6' : '3'))
      goto illegal_fen;
    int i, sq = ((c - '1') << 3) + f;
    for (i = 0; i < pos->num; i++)
      if (pos->sq[i] == (sq ^ 8)) break;
    if (i == pos->num || pos->pt[i] != (pos->stm == WHITE ? PAWN + 8 : PAWN))
      goto illegal_fen;
    pos->state[0].epPiece = i;
  }

  // Halfmove clock
  while (isspace(c = *fen++)) ;
  if (!c)
    goto finish;
  while (isdigit(c)) {
    *cnt50 = *cnt50 * 10 + (c - '0');
    c = *fen++;
  }

  // We ignore the fullmove number.

finish:
  pos->occ = 0;
  for (int i = 0; i < pos->num; i++)
    pos->occ |= bit(pos->sq[i]);

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
    if (move & CAPT_FLAG) {
      *str++ = 'a' + (from & 7);
      if (move & EP_FLAG)
        to ^= 8;
    }
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
        if ((from1 >> 3) == (from >> 3))
          sameRank = true;
      }
    }
    if (sameTo) {
      if (!sameFile || sameRank)
        *str++ = 'a' + (from & 7);
      if (sameFile)
        *str++ = '1' + (from >> 3);
    }
  }
  if (move & CAPT_FLAG)
    *str++ = 'x';
  *str++ = 'a' + (to & 7);
  *str++ = '1' + (to >> 3);
  if (move & PROM_FLAG)
    *str++ = pieceChar[KNIGHT + ((move & PROM_MASK) >> 6)];
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
  *str++ = '1' + (from >> 3);
  *str++ = 'a' + (to & 7);
  *str++ = '1' + (to >> 3);
  if (move & PROM_FLAG)
    *str++ = pieceChar[8 + KNIGHT + ((move & PROM_MASK) >> 6)];
  *str = 0;
}

void TBitf_print_pos(TB_Position *pos)
{
  int board[64];

  for (int i = 0; i < 64; i++)
    board[i] = 0;
  for (int i = 0; i < pos->num; i++)
    board[pos->sq[i]] = pos->pt[i];

  printf("\n +---+---+---+---+---+---+---+---+\n");

  for (int r = 7; r >= 0; r--) {
    for (int f = 0; f <= 7; f++)
      printf(" | %c", pieceChar[board[8 * r + f]]);

    printf(" | %d\n +---+---+---+---+---+---+---+---+\n", r + 1);
  }

  printf("   a   b   c   d   e   f   g   h\n\n");
}
