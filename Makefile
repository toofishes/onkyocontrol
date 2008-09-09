# Makefile for Onkyo Receiver communication program
#CFLAGS=-Wall -g -O2
CFLAGS=-Wall -g

.PHONY: all clean

all: onkyo

clean:
	rm -f onkyo command.o onkyo.o

onkyo: command.o onkyo.o

command.o: command.c onkyo.h

onkyo.o: onkyo.c onkyo.h

