# Probetool

This is a tool for probing Syzygy tablebases. The main goal of this project
is to provide tablebase access code which can relatively easily be
incorporated into chess engines, GUIs and other programs.

The license for this code is: do whatever you want with it (but don't hold
me liable for any games your loses).

## Probetool command
The probetool command accepts a FEN string encoding a chess position,
retrieves the position's WDL and DTZ50 values, and outputs a DTZ50-optimal
mating line if the position is not drawn.

The program looks for tablebase files in the directories listed in the
RTBPATH environment variable. Alternatively, the path can be specified by
means of the -p option. As usual, directories (folders) are separated by
';' characters on windows and by ':' characters on other platforms.

The -u option will force the moves to be output in UCI notation.

Example:
```
$ ./probetool "K7/N7/k7/8/3p4/8/N7/8 w - -"
Found 163 WDL, 163 DTZ and 0 DTM tablebase files.

 +---+---+---+---+---+---+---+---+
 | K |   |   |   |   |   |   |   | 8
 +---+---+---+---+---+---+---+---+
 | N |   |   |   |   |   |   |   | 7
 +---+---+---+---+---+---+---+---+
 | k |   |   |   |   |   |   |   | 6
 +---+---+---+---+---+---+---+---+
 |   |   |   |   |   |   |   |   | 5
 +---+---+---+---+---+---+---+---+
 |   |   |   | p |   |   |   |   | 4
 +---+---+---+---+---+---+---+---+
 |   |   |   |   |   |   |   |   | 3
 +---+---+---+---+---+---+---+---+
 | N |   |   |   |   |   |   |   | 2
 +---+---+---+---+---+---+---+---+
 |   |   |   |   |   |   |   |   | 1
 +---+---+---+---+---+---+---+---+
   a   b   c   d   e   f   g   h

WDL = 1 (cursed win)
DTZ50 = 164 ply

DTZ50-optimal mating line:
[FEN "K7/N7/k7/8/3p4/8/N7/8 w - -"]
1.Nb4+ Kb6 2.Nd3 Kc7 3.Nb5+ Kc6 4.Na3 Kb6 5.Kb8 Kc6 6.Nc2 Kb5 7.Kc7 Ka4 8.Kb6
Kb3 9.Nce1 Ka2 10.Kc5 Kb3 11.Kb5 Kc3 12.Ka4 Kd2 13.Kb3 Kd1 14.Nf3 Ke2 15.Nfe5
Kd2 16.Kb2 Kd1 17.Nc4 Ke2 18.Kc2 Kf3 19.Kd1 Ke4 20.Ke2 Kd5 21.Nd2 Kc6 22.Kf3
Kd5 23.Kf4 Ke6 24.Kg5 Kd5 25.Kf5 Kd6 26.Ke4 Ke6 27.Nc4 Kf6 28.Kd5 Ke7 29.Ke5
Kf7 30.Kd6 Kf6 31.Nd2 Kf5 32.Ke7 Kg6 33.Ke6 Kg5 34.Ke5 Kg6 35.Ne4 Kg7 36.Kd6
Kh6 37.Nd2 Kg7 38.Ke6 Kf8 39.Nc4 Ke8 40.Na3 Kd8 41.Nb5 Kc8 42.Kd6 Kb7 43.Kd7
Ka6 44.Na3 Ka5 45.Nb1 Kb5 46.Nd2 Kb6 47.Kd6 Kb5 48.Kc7 Ka6 49.Kc6 Ka5 50.Kc5
Ka6 51.Nc4 Kb7 52.Kd6 Kc8 53.Na5 Kd8 54.Nb7+ Ke8 55.Ke6 Kf8 56.Nd6 Kg7 57.Kf5
Kh6 58.Kf6 Kh5 59.Ne4 Kg4 60.Ng5 Kh4 61.Kf5 Kg3 62.Ke4 Kg4 63.Nf7 Kg3 64.Nfe5
Kh4 65.Kf5 Kh5 66.Ng4 Kh4 67.Nf6 Kh3 68.Ke5 Kg3 69.Ke4 Kh3 70.Kf3 Kh4 71.Kf4
Kh3 72.Ne4 Kh4 73.Ng3 Kh3 74.Nf5 Kg2 75.Kg4 Kf1 76.Ng3+ Kg1 77.Ne4 Kh2 78.Nd2
Kg2 79.Kh4 Kg1 80.Kh3 Kh1 81.Kg3 Kg1 82.Nf2 d3 83.Nd1 Kh1 84.Nf3 d2 85.Nf2#
```
The FEN tag is added to allow easy copy and paste of the mating line into a
GUI. Note that the DTZ50 value can be off by one.

## Tablebase access code
The core tablebase probing code is in tbprobe.c and tbprobe.h. It
implements the functions TB\_init(), TB\_release(), TB\_free(),
TB\_probe\_wdl(), TB\_probe\_dtz() and TB\_probe\_dtm(). See tbprobe.h for
details. Note that DTM Syzygy tables are currently not available, so
TB\_probe\_dtm() can be ignored for now.

To let an engine or other program interact directly with this code:
- Add the tbprobe.c and tbprobe.h files to your source tree.
- Implement the TB\_Position struct and the TB\_* functions listed in
  tbprobe.h under "Functions required by tbprobe.c".

The functions to be implemented are functions to generate chess moves, to
do/undo moves and to test certain properties such as move legality. It
should normally be relatively straightforward to implement these functions
using the existing board representation and move generation code of a chess
engine.

Alternatively, a program can use one of two example interface
implementations.

## Interface code
The two example interface implementations are tbinterface\_bb.c and
tbinterface\_0x88.bb. To use one of these:
- Add the tbinterface\_bb/\_0x88.c and tbinterface.h files to your source
  tree.
- Allocate a TB\_Position struct using TBitf\_alloc\_position() and set up
  a position from a FEN string using TBitf\_set\_from\_fen(). Pass a
  pointer to the TB\_Position struct to TB\_probe\_wdl/\_dtz() to probe the
  position.

See tbinterface.h (and tbprobe.h) for details.

If you use one of the example implementations and speed is important, you
may want to replace TBiitf\_set\_from\_fen() with a function that more
directly initializes a TB\_Position struct from your program's internal
board representation.

## Tablebase root probing code
It can be tricky to let a chess engine correctly use tablebase information
when the game has reached a position that is in the tablebases. If DTM is
not available, then always playing the DTZ50-optimal move leads to
unnatural play (as the example given above shows). Even if DTM tables were
available, in some positions the engine would have to deviate from the
DTM-optimal line to preserve a win under the 50-move rule.

My preferred approach is to use the tablebases to preselect the set of
moves that are equivalent in terms of outcome and then to carry out a
regular search (without tablebase probing) on only those moves. The move
that comes out on top will not be an "unnatural" move such as an
unnecessary queen sacrifice and will normally make progress towards a mate.
Only if no clear progress is being made (e.g. as measured by a high value
of DTZ that does not go down as the engine makes moves) will it be
necessary to preselect the moves that lower DTZ. The extreme case of no
progress is the engine repeating (or allowing a repeat of) an earlier
position, in which case it is best to keep playing DTZ-optimal moves until
the 50-move clock is reset by a pawn move or capture. (Note that playing
DTZ-optimal moves until the next zeroing move will avoid further
repetitions because each next position will have a lower DTZ value, whereas
repeated positions necessarily have the same DTZ value.)

To cut a long story short: see the functions root\_probe\_wdl() and
root\_probe\_dtz() (and root\_probe\_dtm()) in probetool.c. First
root\_probe\_dtz() should be called on the root (tablebase) position. If
this fails because some DTZ files are missing, root\_probe\_wdl() should be
called. A successful call results in a list of ranked and scored moves.
Equally ranked moves are equivalent in terms of outcome. The move to be
played should therefore be picked from the set of highest-ranked moves. The
scores assigned to a move are intended to be used as the UCI score for a
move (overriding the score returned by the search).

The ranks and scores assigned per move allow for a simple implementation of
the UCI searchmoves command for tablebase root positions. The UCI multipv
command can be implemented by searching the group of highest-ranked moves
first, then the next group of equally ranked moves, etc.

The output of root\_probe\_dtz() can be shown using the -r option. To see
the output of root\_probe\_wdl(), additionally add the -w option.

