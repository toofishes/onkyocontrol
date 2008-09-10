# Makefile for Onkyo Receiver communication program
#CFLAGS = -Wall -Wextra -g -O2 -march=native
CFLAGS = -Wall -Wextra -g -pedantic -fstrict-aliasing -Wunreachable-code -fstack-protector -march=native

program = onkyocontrol
objects = command.o onkyo.o receiver.o

.PHONY: all clean

all: $(program)

clean:
	rm -f $(program)
	rm -f $(objects)

$(program): $(objects)
	@rm -f $(program)
	$(CC) $(objects) -o $(program)

command.o: Makefile command.c onkyo.h

receiver.o: Makefile receiver.c onkyo.h

onkyo.o: Makefile onkyo.c onkyo.h

