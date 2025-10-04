/*
  Copyright (c) 2025 Ronald de Man
  This file may be redistributed and/or modified without restrictions.
*/

#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tbprobe.h"
#include "tbinterface.h"

#undef min
#undef max
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))

// Just some reasonable values for MATE and TB_WIN
enum {
  VALUE_MATE   = 32000,
  VALUE_TB_WIN = 30000,
};

static int WdlToDtz[] = { -1, -101, 0, 101, 1 };

static int cnt50;
static char *fen;
static int col;
static bool uciMoves = false;
static bool reportStats = false;

// Don't go over 79 chars per line
void output(const char *str)
{
  if (!uciMoves && col + (int)strlen(str) > 79) {
    putc('\n', stdout);
    col = 0;
  }
  if (!col && str[0] == ' ')
    str++;
  fputs(str, stdout);
  col += strlen(str);
}

// Print a DTZ50-optimal line of moves leading to mate.
void print_pv_dtz(TB_Position *pos)
{
  char str[32], moveStr[16];
  int pv[2000], k = 0;
  bool success;

  printf("\nDTZ50-optimal mating line:\n[FEN \"%s\"]\n", fen);
  col = 0;
  TB_ProbeCount[TB_WDL] = TB_ProbeCount[TB_DTZ] = 0;

  int bestScore;
  do {
    int numCaps = TB_generate_captures(pos);
    int numMoves = TB_generate_quiets(pos, numCaps);

    int v, dtz, bestMove;
    bestScore = -10001;
    for (int m = 0; m < numMoves; m++) {
      bool zeroingMove = m < numCaps || TB_move_is_pawn_move(pos, m);
      if (!TB_do_move(pos, m))
        continue;
      if (zeroingMove) {
        int wdl = -TB_probe_wdl(pos, &success);
        dtz = WdlToDtz[wdl + 2];
      } else {
        dtz = -TB_probe_dtz(pos, &success);
        dtz +=  dtz > 0 ?  1
              : dtz < 0 ? -1
              : 0;
      }
      // Convert dtz to a "linear" score that allows for easy comparison.
      v =  dtz > 0 ?  10000 - dtz
         : dtz < 0 ? -10000 - dtz
         : 0;
      // Check for mate (i.e. mate in 1)
      if (v >= 9997 && TB_in_check(pos) && TB_no_legal_moves(pos))
        v = 10000;
      TB_undo_move(pos, m);
      if (v > bestScore) {
        bestScore = v;
        bestMove = m;
      }
      if (!success) {
        output(" *");
        goto exit;
      }
    }
    if (bestScore == -10001)
      break;
    if (!uciMoves && TB_white_to_move(pos))
      sprintf(str, " %d.", 1 + (k + 1) / 2);
    else if (!uciMoves && k == 0)
      strcpy(str, "1...");
    else
      strcpy(str, " ");
    if (uciMoves)
      TBitf_move_to_string_uci(pos, bestMove, moveStr);
    else
      TBitf_move_to_string(pos, bestMove, moveStr);
    strcat(str, moveStr);
    output(str);
    TB_do_move(pos, bestMove);
    pv[k++] = bestMove;
  } while (bestScore < 10000 && k < 2000);

exit:
  while (k > 0)
    TB_undo_move(pos, pv[--k]);

  printf("\n");
  if (reportStats) {
    printf("ProbeCount[WDL] = %"PRIu64"\n", TB_ProbeCount[TB_WDL]);
    printf("ProbeCount[DTZ] = %"PRIu64"\n", TB_ProbeCount[TB_DTZ]);
  }
}

// Print a DTM-optimal line of moves leading to mate.
// This function must be called with the known DTM value of the position.
void print_pv_dtm(TB_Position *pos, int dtm)
{
  char str[32], moveStr[16];
  int pv[2000], k = 0;
  bool success;

  if (dtm == 0)
    return;

  printf("\nDTM-optimal mating line:\n[FEN \"%s\"]\n", fen);
  col = 0;
  TB_ProbeCount[TB_WDL] = TB_ProbeCount[TB_DTM] = 0;

  do {
    int numCaps = TB_generate_captures(pos);
    int numMoves = TB_generate_quiets(pos, numCaps);

    // Calculate the DTM value of the optimal successor position.
    dtm = (dtm > 0) - dtm;
    int m;
    for (m = 0; m < numMoves; m++) {
      if (!TB_do_move(pos, m))
        continue;
      bool isOptimal = TB_probe_dtm_test(pos, dtm, &success);
      TB_undo_move(pos, m);
      if (!success) {
        output(" *");
        goto exit;
      }
      if (isOptimal)
        break;
    }
    if (m == numMoves) {
      output(" *");
      goto exit;
    }
    if (!uciMoves && TB_white_to_move(pos))
      sprintf(str, " %d.", 1 + (k + 1) / 2);
    else if (!uciMoves && k == 0)
      strcpy(str, "1...");
    else
      strcpy(str, " ");
    if (uciMoves)
      TBitf_move_to_string_uci(pos, m, moveStr);
    else
      TBitf_move_to_string(pos, m, moveStr);
    strcat(str, moveStr);
    output(str);
    TB_do_move(pos, m);
    pv[k++] = m;
  } while (dtm && k < 2000);

exit:
  while (k > 0)
    TB_undo_move(pos, pv[--k]);

  printf("\n");

  if (reportStats) {
    printf("ProbeCount[WDL] = %"PRIu64"\n", TB_ProbeCount[TB_WDL]);
    printf("ProbeCount[DTM] = %"PRIu64"\n", TB_ProbeCount[TB_DTM]);
  }
}

struct RootMove {
  int m;
  int rank;
  int score;
};

typedef struct RootMove RootMove;

static int numRootMoves;

#define MAX_MOVES 150
static RootMove rootMove[MAX_MOVES];

// Use the DTZ tables to rank and score all root moves.
// Returns true if successful.
//
// If the game has reached a TB position, the engine is recommended to
// search (without accessing TBs within the search) only the set of root
// moves of the highest "rank" among all the root moves. This should
// guarantee an optimal outcome given the available DTZ or WDL information.
// The assigned score is intended to be displayed as the move's UCI score.
//
// Note that this mechanism can be made to interoperate well with the
// UCI "searchmoves" command and with the UCI "multipv" command.
bool root_probe_dtz(TB_Position *pos)
{
  // Check whether there was a repeat of position (not necessarily the
  // current position) since the last zeroing move. If there was, we need
  // to play DTZ-optimal moves if winning to avoid a potential third
  // repetition of a same position.
  bool rep = false; // probetool does not accept game history
//  bool rep = has_repeated(pos);

  // Generate root moves
  int numCaps = TB_generate_captures(pos);
  int num = TB_generate_quiets(pos, numCaps);

  int dtz, i = 0;
  bool success;

  for (int m = 0; m < num; m++) {
    bool zeroingMove = m < numCaps || TB_move_is_pawn_move(pos, m);
    if (!TB_do_move(pos, m))
      continue;

    if (zeroingMove) {
      // If the move resets the 50-move counter, dtz is -101/-1/0/1/101.
      int v = -TB_probe_wdl(pos, &success);
      dtz = WdlToDtz[v + 2];
    } else {
      // Otherwise, take dtz for the new position and correct by 1 ply.
      dtz = -TB_probe_dtz(pos, &success);
      dtz += dtz > 0 ? 1 : dtz < 0 ? -1 : 0;
    }
    // Make sure that a mating move is assigned value 1.
    if ((dtz == 2 || dtz == 3) && TB_in_check(pos) && TB_no_legal_moves(pos))
      dtz = 1;
    TB_undo_move(pos, m);
    if (!success) return false;

    // Calculate a "rank" for the move.
    // Better moves are ranked higher. Guaranteed wins are ranked equally.
    // Losing moves are ranked equally unless a 50-move draw is in sight.
    // Note that moves ranked 900 have dtz + cnt50 == 100, which in rare
    // cases may be insufficient to win as dtz may be one off (see the
    // comments in tbprobe.c above TB_probe_dtz()).
    int r =  dtz > 0 ? (dtz + cnt50 <= 99 && !rep ? 1000 : 1000 - (dtz + cnt50))
           : dtz < 0 ? (-dtz * 2 + cnt50 < 100 ? -1000 : -1000 + (-dtz + cnt50))
           : 0;

    // Determine the score to be displayed for this move. Assign at least
    // 1 cp to cursed wins and let it grow to 49 cp as the position gets
    // closer to a real win.
    int s =  r >= 900 ?  VALUE_TB_WIN
           : r >  0   ?  max( 2, r - 800) / 2
           : r == 0   ?  0
           : r > -900 ?  min(-2, r + 800) / 2
           :            -VALUE_TB_WIN;

    rootMove[i] = (struct RootMove){ m, r, s };
    i++;
  }
  numRootMoves = i;

  return true;
}

// Use the WDL tables to rank all root moves in the list.
// This is a fallback in case some or all DTZ tables are missing.
// Returns true if successful.
bool root_probe_wdl(TB_Position *pos)
{
  static int WdlToRank[] = { -1000, -899, 0, 899, 1000 };
  static int WdlToScore[] = { -VALUE_TB_WIN, -1, 0, 1, VALUE_TB_WIN };

  // Generate root moves
  int numCaps = TB_generate_captures(pos);
  int num = TB_generate_quiets(pos, numCaps);

  int v, i = 0;
  bool success;

  // Probe, rank and score each move.
  for (int m = 0; m < num; m++) {
    if (!TB_do_move(pos, m))
      continue;
    v = -TB_probe_wdl(pos, &success);
    TB_undo_move(pos, m);
    if (!success)
      return false;
    rootMove[i] = (struct RootMove){ m, WdlToRank[v + 2], WdlToScore[v + 2] };
    i++;
  }

  numRootMoves = i;
  return true;
}

// Use the DTM tables to find mate scores.
// Before calling TB_root_probe_dtm(), either root_probe_dtz() or
// root_probe_wdl() must have been called successfully.
bool root_probe_dtm(TB_Position *pos)
{
  // Generate root moves
  int numCaps = TB_generate_captures(pos);
  TB_generate_quiets(pos, numCaps);

  int val[MAX_MOVES];
  bool success;

  // Probe each move
  for (int i = 0; i < numRootMoves; i++) {
    // Find out whether the move is winning or losing or drawing based
    // on the DTZ or WDL root probe.
    int s = rootMove[i].score;
    int wdl = s > 100 ? 2 : s < -100 ? -2 : 0;
    if (wdl == 0)
      val[i] = s; // pure draw or cursed win or blessed loss
    else {
      int m = rootMove[i].m;
      TB_do_move(pos, m); // We know it's legal
      int dtm = -TB_probe_dtm(pos, wdl < 0, &success);
      TB_undo_move(pos, m);
      // Convert dtm into a mate score
      val[i] = dtm > 0 ?  VALUE_MATE - (2 * dtm - 1)
                       : -VALUE_MATE - 2 * dtm;
      if (!success)
        return false;
    }
  }

  // All DTM probes were successful. Now adjust TB scores and ranks.
  for (int i = 0; i < numRootMoves; i++) {
    rootMove[i].score = val[i];

    // Let rank correspond to mate score, except for critical moves
    // ranked 900, which we rank below all other mates for safety.
    // By ranking mates above 1000 or below -1000, we let the search
    // know it need not search those moves.
    rootMove[i].rank = rootMove[i].rank == 900 ? 1001 : val[i];
  }

  return true;
}

static int compare(const void *a, const void *b)
{
  const RootMove *m1 = (const RootMove *)a;
  const RootMove *m2 = (const RootMove *)b;

  return m1->rank != m2->rank ? m2->rank - m1->rank : m2->score - m1->score;
}

void score_to_string(int v, char *str)
{
  if (abs(v) <= VALUE_TB_WIN )
    sprintf(str, "cp %d", v);
  else
    sprintf(str, "mate %d",
                 (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2);
}

static void score_to_meaning(int score, char *meaning)
{
  meaning[0] = 0;
  if (score > VALUE_TB_WIN)
    strcpy(meaning, "mate score");
  else if (score == VALUE_TB_WIN)
    strcpy(meaning, "TB win");
  else if (score > 0)
    strcpy(meaning, "cursed win (draw)");
  else if (score == 0)
    strcpy(meaning, "draw");
  else if (score > -VALUE_TB_WIN)
    strcpy(meaning, "blessed loss (draw)");
  else if (score == -VALUE_TB_WIN)
    strcpy(meaning, "TB loss");
  else
    strcpy(meaning, "mate score");
}

static void print_root_moves(TB_Position *pos)
{
  char str[16], scoreStr[20], meaning[32];

  qsort(rootMove, numRootMoves, sizeof(RootMove), compare);

  printf("move       rank   score       meaning\n");
  for (int i = 0; i < numRootMoves; i++) {
    if (!uciMoves)
      TBitf_move_to_string(pos, rootMove[i].m, str);
    else
      TBitf_move_to_string_uci(pos, rootMove[i].m, str);
    score_to_string(rootMove[i].score, scoreStr);
    score_to_meaning(rootMove[i].score, meaning);
    printf("%-8s %6d   %-9s   %s\n", str, rootMove[i].rank, scoreStr,
        meaning);
  }
}

extern char *optarg;

static char *wdlStr[] = {
  "loss", "blessed loss", "draw", "cursed win", "win"
};

int main(int argc, char *argv[])
{
  char *path = getenv("RTBPATH");

  bool rootProbe = false, wdlRootProbe = false;
  int val;
  while ((val = getopt(argc, argv, "p:ruws")) != -1) {
    switch (val) {
    case 'p':
      path = optarg;
      break;
    case 'r':
      rootProbe = true;
      break;
    case 'u':
      uciMoves = true;
      break;
    case 'w':
      wdlRootProbe = true;
      break;
    case 's':
      reportStats = true;
      break;
    }
  }

  TB_init(path);
  printf("Found %d WDL, %d DTZ and %d DTM tablebase files.\n",
      TB_NumTables[TB_WDL], TB_NumTables[TB_DTZ], TB_NumTables[TB_DTM]);

  if (optind >= argc) {
    printf("USAGE: %s [-p <path>] [-u] [-r] [-w] [-s] <fen>\n", argv[0]);
    return 0;
  }
  fen = argv[optind];

  TBitf_init();

  TB_Position *pos = TBitf_alloc_position();
  TBitf_set_from_fen(pos, fen, &cnt50);

  TBitf_print_pos(pos);

  bool successWdl, successDtz, successDtm = false;
  int wdl, dtz, dtm = 0;

  wdl = TB_probe_wdl(pos, &successWdl);
  if (successWdl)
    printf("WDL = %d (%s)\n", wdl, wdlStr[wdl + 2]);
  else
    printf("WDL probe failed.\n");

  dtz = TB_probe_dtz(pos, &successDtz);
  if (successDtz)
    printf("DTZ50 = %d ply\n", dtz);
  else
    printf("DTZ probe failed.\n");

  if (TB_NumTables[TB_DTM] > 0) {
    dtm = TB_probe_dtm(pos, wdl > 0, &successDtm);
    if (successDtm)
      printf("DTM = %d moves\n", dtm);
    else
      printf("DTM probe failed.\n");
  }

  if (successDtz && wdl != 0)
    print_pv_dtz(pos);

  if (successDtm && wdl != 0)
    print_pv_dtm(pos, dtm);

  if (!rootProbe)
    goto exit;

  if (wdlRootProbe)
    successDtz = false;
  if (successDtz) {
    printf("\nroot_probe_dtz():\n");
    if ((successDtz = root_probe_dtz(pos)))
      print_root_moves(pos);
    else
      printf("Failed.\n");
  }

  if (successWdl && (wdlRootProbe || !successDtz)) {
    printf("\nroot_probe_wdl():\n");
    if ((successWdl = root_probe_wdl(pos)))
      print_root_moves(pos);
    else
      printf("Failed.\n");
  }

  if ((successWdl || successDtz) && successDtm) {
    printf("\nroot_probe_dtm():\n");
    if (root_probe_dtm(pos))
      print_root_moves(pos);
    else
      printf("Failed.\n");
  }

exit:
  TBitf_free_position(pos);
  TB_free();

  return 0;
}
