CC = gcc
#CC = x86_64-w64-mingw32-gcc
CFLAGS = -O3 -std=c11 -Wall -Wextra -pedantic -DNDEBUG
#CFLAGS = -g3 -std=c11 -Wall -Wextra -pedantic

# Make a choice between bitboard-based move generation and 0x88-based move generation.
TBINTERFACE = jtbinterface_bb
#TBINTERFACE = jtbinterface_0x88

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $*.o

all: jprobetool

jprobetool.o: jprobetool.c jtbprobe.h jtbinterface.h
jtbprobe.o: jtbprobe.c jtbprobe.h
jtbinterface.o: $(TBINTERFACE).c jtbprobe.h jtbinterface.h

jprobetool: jprobetool.o jtbprobe.o $(TBINTERFACE).o
	$(CC) $(CFLAGS) -o jprobetool jprobetool.o jtbprobe.o $(TBINTERFACE).o

clean:
	rm -rf *.o

.PHONY: clean
