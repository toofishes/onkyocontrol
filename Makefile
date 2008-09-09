# Makefile for Onkyo Receiver communication program
#CFLAGS=-Wall -g -O2
CFLAGS=-Wall -g

.PHONY: all clean

all: onkyo

clean:
	rm -f onkyo command.o onkyo.o receiver.o

onkyo: command.o onkyo.o receiver.o

command.o: command.c onkyo.h

receiver.o: receiver.c onkyo.h

onkyo.o: onkyo.c onkyo.h

