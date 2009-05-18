# NewScript Makefile
#

CFLAGS = -ggdb -std=c99 `sdl-config --cflags`
LIBS = `sdl-config --libs` -framework OpenGL

all : ns

.PHONY: clean
clean:
	rm ns

.PHONY: commit
commit:
	git commit -a

ns : ns.c
	gcc -o ns ns.c $(CFLAGS) $(LIBS)
