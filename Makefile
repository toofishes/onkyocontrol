# Makefile for Onkyo Receiver communication program
#CFLAGS = -Wall -Wextra -ggdb -O2 -fstrict-aliasing -march=native -std=c99 -fprofile-arcs -ftest-coverage
CFLAGS = -Wall -Wextra -ggdb -O2 -fstrict-aliasing -flto -march=native -std=c99
LDFLAGS = -Wl,-O1,--as-needed -ggdb -O2 -fstrict-aliasing -march=native -std=c99 -fwhole-program

program = onkyocontrol
objects = command.o onkyo.o receiver.o util.o
asm = command.s onkyo.s receiver.s util.s

.PHONY: all clean doc

all: $(program)

asm: $(asm)

clean:
	rm -f $(program) $(program).exe
	rm -f $(objects)
	rm -f $(asm)
	rm -rf doc

$(program): $(objects)
	@rm -f $(program)
	$(CC) $(objects) $(LDFLAGS) -o $(program)

%.s : %.c
	$(CC) -S $(CFLAGS) $(CPPFLAGS) $< -o $@

command.o: Makefile command.c onkyo.h

receiver.o: Makefile receiver.c onkyo.h

onkyo.o: Makefile onkyo.c onkyo.h

util.o: Makefile util.c onkyo.h

doc:
	mkdir -p doc
	doxygen

install: $(program)
	install -m755 $(program) /usr/bin/
	install -m755 frontend.py /usr/bin/onkyo-frontend

uninstall:
	rm -f /usr/bin/$(program)
	rm -rf /usr/bin/onkyo-frontend
