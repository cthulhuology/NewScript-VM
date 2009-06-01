# NewScript Makefile
#

CFLAGS = -DINLINE= -ggdb -std=c99
SDLFLAGS = `sdl-config --cflags`

LIBS = 
SDLLIBS = `sdl-config --libs` -framework OpenGL -lpcap -lSDL_image


all : ns nsc

.PHONY: clean
clean:
	rm -rf ns ns.dSYM
	rm -rf nsc nsc.dSYM

.PHONY: commit
commit:
	git commit -a

.PHONY: metrics
metrics:
	cat ns.c | cmetrics.pl
	cat nsc.c | cmetrics.pl
	cat *.c | cmetrics.pl

.PHONY: push
push:
	git push origin master

ns : ns.c
	gcc $(CFLAGS) $(SDLFLAGS) -o ns ns.c $(LIBS) $(SDLLIBS)

nsc : nsc.c
	gcc $(CFLAGS) -o nsc nsc.c $(LIBS)

install: ns
	sudo install -o root -g staff -m 4755 ns /usr/local/bin
