# Makefile for Onkyo Receiver communication program
#CFLAGS = -Wall -Wextra -ggdb -O2 -fstrict-aliasing -march=native -std=c99 -fprofile-arcs -ftest-coverage
#LDFLAGS = -lgcov
CFLAGS = -Wall -Wextra -ggdb -O2 -fstrict-aliasing -march=native -std=c99
LDFLAGS =

program = onkyocontrol
objects = command.o onkyo.o receiver.o util.o

.PHONY: all clean doc

all: $(program)

clean:
	rm -f $(program) $(program).exe
	rm -f $(objects)
	rm -rf doc

$(program): $(objects)
	@rm -f $(program)
	$(CC) $(objects) $(LDFLAGS) -o $(program)

command.o: Makefile command.c onkyo.h

receiver.o: Makefile receiver.c onkyo.h

onkyo.o: Makefile onkyo.c onkyo.h

util.o: Makefile util.c onkyo.h

doc:
	mkdir -p doc
	doxygen

install: $(program)
	install -m755 $(program) /usr/bin/

uninstall:
	rm -f /usr/bin/$(program)
