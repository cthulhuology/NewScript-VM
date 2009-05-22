# NewScript Makefile
#

CFLAGS = -O2 -pg -ggdb -std=c99 `sdl-config --cflags`
LIBS = `sdl-config --libs` -framework OpenGL -lpcap

all : ns

.PHONY: clean
clean:
	rm -rf ns ns.dSYM

.PHONY: commit
commit:
	git commit -a

ns : ns.c
	gcc $(CFLAGS) -o ns ns.c $(LIBS)

install: ns
	sudo install -o root -g staff -m 4755 ns /usr/local/bin
