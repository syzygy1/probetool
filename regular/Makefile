CC = gcc
#CC = x86_64-w64-mingw32-gcc
#CFLAGS = -O3 -std=c11 -Wall -Wextra -pedantic -DNDEBUG
CFLAGS = -g3 -std=c11 -Wall -Wextra -pedantic

# Make a choice between bitboard-based move generation and 0x88-based move generation.
TBINTERFACE = tbinterface_bb
#TBINTERFACE = tbinterface_0x88

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $*.o

all: probetool

probetool.o: probetool.c tbprobe.h tbinterface.h
tbprobe.o: tbprobe.c tbprobe.h
tbinterface.o: $(TBINTERFACE).c tbprobe.h tbinterface.h

probetool: probetool.o tbprobe.o $(TBINTERFACE).o
	$(CC) $(CFLAGS) -o probetool probetool.o tbprobe.o $(TBINTERFACE).o

probetool_bb: probetool.o tbprobe.o tbinterface_bb.o
	$(CC) $(CFLAGS) -o probetool_bb probetool.o tbprobe.o tbinterface_bb.o

probetool_0x88: probetool.o tbprobe.o tbinterface_0x88.o
	$(CC) $(CFLAGS) -o probetool_0x88 probetool.o tbprobe.o tbinterface_0x88.o

clean:
	rm -rf *.o

.PHONY: clean
