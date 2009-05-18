// ns.c
//
// Copyright (C) 2009 David J. Goehrig
// All Rights Reserved
//
//	A prototype NewScript portable VM
//

#include "SDL.h"
#include "SDL_opengl.h"
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

// errors
#define NO_RAM		1
#define NO_FILE		2
#define NO_MAP		3
#define NO_ROM		4
#define NO_DISPLAY	5

// sizes
#define CACHE_SIZE	4096
#define ROM_SIZE	4096
#define RAM_SIZE	1073741824
#define FLASH_SIZE	1073741824

// timings
#define REFRESH_RATE	1000000
#define INTERRUPT_RATE	10000

// typedefs
typedef Uint32 cell;

// VM globals
cell ip;
cell dsi;
cell ds[8];
cell rsi;
cell rs[8];
cell cnt;
cell src;
cell dst;
cell utl;
cell im[CACHE_SIZE];
cell rom[ROM_SIZE];
cell* ram;
cell* flash;

// System globals
cell flash_size;
cell flash_fd;
char* flash_file;
SDL_Surface* display;
SDL_Event event;
cell ticks;

// functions

cell tos() { return ds[dsi]; }
cell nos() { return ds[7&(dsi-1)]; }
void up(cell c) { ds[dsi = 7&(dsi+1)] = c; }
void down() { dsi = 7&(dsi-1); }
void stos(cell c) { ds[dsi] = c; }
void snos(cell c) { ds[7&(dsi-1)] = c; }
cell rtos() { return rs[rsi]; }
void upr(cell c) { rs[rsi = 7&(rsi+1)] = c; }
void downr() { rsi = 7&(rsi-1); }

cell net_read() {

}

cell mouse_buffer[3] = { 0, 0, 0 };
cell mouse_buffer_index = 0;
cell mouse_read() {
	mouse_buffer_index %= 3;
	return mouse_buffer[mouse_buffer_index++];
}

cell key_buffer = 0;
cell key_read() { return key_buffer; }

void net_write(cell val) {

}

SDL_Rect video_rect;
cell texture_memory[1280*720];	// default screen resolution is 720p
cell video_color[4];
cell video_input[3];
cell video_index = 0;

void vid_write(cell val) {
	
	
}

void vid_data() {
	video_index %= 1280*720;
	texture_memory[video_index++] = val;
}

void vid_at(cell x, cell y) {
	video_rect.x = (Sint16)x;
	video_rect.y = (Sint16)y;
}

void vid_to(cell dx, cell dy) {
	video_rect.x += (Sint16)dx;
	video_rect.y += (Sint16)dy;
}

void vid_by(cell dx, cell dy) {
	video_rect.w = (Uint16)dx;
	video_rect.h = (Uint16)dy;
}

void vid_as(cell x, cell y, cell dx, cell dy) {
	video_rect.x = (Sint16)x;
	video_rect.y = (Sint16)y;
	video_rect.w = (Uint16)dx;
	video_rect.h = (Uint16)dy;
}

void vid_line() {
	glBegin(GL_LINES);
	glVertex3i(video_rect.x,video_rect.y,0);
	glVertex3i(video_rect.x+(Sint16)video_rect.w,video_rect.y+(Sint16)video_rect.h,0);
	glEnd();
}

void vid_arc() {
	// TODO
}

void vid_rect() {
	glBegin(GL_LINE_LOOP);
	glVertex3i(video_rect.x,video_rect.y+video_rect.h,0);
	glVertex3i(video_rect.x+video_rect.w,video_rect.y+video_rect.h,0);
	glVertex3i(video_rect.x+video_rect.w,video_rect.y,0);
	glVertex3i(video_rect.x,video_rect.y,0);
	glEnd();
}

void vid_fill() {
	glBegin(GL_QUADS);
	glVertex3i(video_rect.x,video_rect.y+video_rect.h,0);
	glVertex3i(video_rect.x+video_rect.w,video_rect.y+video_rect.h,0);
	glVertex3i(video_rect.x+video_rect.w,video_rect.y,0);
	glVertex3i(video_rect.x,video_rect.y,0);
	glEnd();
}

void vid_color() {
	glColor4ub(video_color[0],video_color[1],video_color[2],video_color[3]);
}

cell audio_memory[44100];	// default buffer is 1sec of audio 44100Hz 2 channels 16bit PCM linear
cell audio_index = 0;
void aud_write(cell val) {
	cell* ms = NULL;
	if (utl&0x08) {
		audio_index %= 44100;
		audio_memory[audio_index++] = val;
		return;
	}
	ms = src & 0x80000000 ? &flash[src]:
		src < 0x1000 ? &rom[src]:
		&ram[src];
	memcpy(audio_memory,ms,cnt*sizeof(cell));
}

void mem_read(cell addr) {
	addr & 0x80000000 ? stos(flash[addr] & 0x7fffffff):
	addr == 0x7fffffff ? stos(net_read()):
	addr == 0x7ffffffc ? stos(mouse_read()):
	addr == 0x7ffffffb ? stos(key_read()):
	addr < 0x1000 ? stos(rom[addr]):
	stos(ram[addr]);
}

void mem_write(cell addr, cell value) {
	addr & 0x80000000 ? (flash[addr] = value):
	addr == 0x7fffffff ? net_write(value):
	addr == 0x7ffffffe ? vid_write(value):
	addr == 0x7ffffffd ? aud_write(value):
	(ram[addr] = value);
}

void mem_move(int d) {
	cell* ms = NULL; 
	cell* md = NULL;
	utl &= 0xfffffff7;
	src & 0x80000000 ? (ms = &flash[src]):
	src < 0x1000 ? (ms = &rom[src]):
	0x7fffffff == src ? net_read():
	0x7ffffffc == src ? mouse_read():
	0x7ffffffb == src ? key_read():
	(ms = &ram[src]);
	dst & 0x80000000 ? (md = &flash[dst]):
	dst < 0x1000 ? (md = &im[dst]):
	0x7fffffff == dst ? net_write(0):
	0x7ffffffe == dst ? vid_write(0):
	0x7ffffffd == dst ? aud_write(0):
	(md = &ram[dst]);
	if (ms && md) 
		d < 0 ? memmove(md-cnt,ms-cnt,cnt*sizeof(cell)) : memmove(md,ms,cnt*sizeof(cell));	
	utl |= 0x08;
}

// NB: we can't compare device data!  Copy to a buffer first.  dst = im, src = rom
void mem_cmp() {
	cell* ms = NULL; 
	cell* md = NULL;
	utl &= 0xfffffff7;
	ms = src & 0x80000000 ? &flash[src] :
		src < 0x1000 ? &rom[src] :
		src < 0x7ffffffb ? &ram[src] :
		NULL;
	md = dst & 0x80000000 ? &flash[dst] :
		src < 0x1000 ? &im[dst] :
		src < 0x7ffffffb ? &ram[dst] :
		NULL;
	if (ms && md) 
		cnt = memcmp(ms,md,cnt*sizeof(cell));
	utl |= 0x08;
}

int keymap() {
	int c = event.key.keysym.sym;
	int map[] = { 
		SDLK_0, SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5, SDLK_6, SDLK_7, 
		SDLK_8, SDLK_9, SDLK_a, SDLK_b, SDLK_c, SDLK_d, SDLK_e, SDLK_f, 
		SDLK_g, SDLK_h, SDLK_i, SDLK_j, SDLK_k, SDLK_l, SDLK_m, SDLK_n, 
		SDLK_o, SDLK_p, SDLK_q, SDLK_r, SDLK_s, SDLK_t, SDLK_u, SDLK_v, 
		SDLK_w, SDLK_x, SDLK_y, SDLK_z, SDLK_COMMA, SDLK_PERIOD, SDLK_SLASH, SDLK_SEMICOLON, 
		SDLK_QUOTE, SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET, SDLK_BACKSLASH, SDLK_BACKQUOTE, SDLK_MINUS, SDLK_EQUALS, SDLK_SPACE, 
	};
	for (int i = 0; map[i]; ++i)
		if (map[i] == c)
			return event.key.keysym.mod & KMOD_SHIFT ? 0x30 + i : i;
	return	c == SDLK_RETURN ? 0x61 : 
		c == SDLK_TAB ? 0x51 : 
		c == SDLK_BACKSPACE ? 0x62 : 
		c == SDLK_LMETA ? 0x63 : 
		c == SDLK_RMETA ? 0x63 : 
		c == SDLK_LALT ? 0x64 : 
		c == SDLK_RALT ? 0x64 : 
		c == SDLK_LCTRL ? 0x65 : 
		c == SDLK_RCTRL ? 0x65 : 
		0x66;
}

void interrupt() {
	utl &= 0xfffffff0;
	if (SDL_PollEvent(&event)) switch(event.type) {
		case SDL_QUIT:
			munmap(flash,flash_size);
			close(flash_fd);
			SDL_Quit();
			exit(0);
		case SDL_KEYDOWN:
			utl |= 1;
			key_buffer = 0x80 | keymap();
			break;
		case SDL_KEYUP:
			utl |= 1;
			key_buffer = 0x7f & keymap();
			break;
		case SDL_MOUSEMOTION:
			utl |= 2;
			mouse_buffer[0] = event.motion.y;
			mouse_buffer[1] = event.motion.x;
			break;
		case SDL_MOUSEBUTTONDOWN:
			utl |= 2;
			mouse_buffer[0] = event.button.y;
			mouse_buffer[1] = event.button.x;
			mouse_buffer[2] = 0x80 | event.button.button;
			break;
		case SDL_MOUSEBUTTONUP:
			utl |= 2;
			mouse_buffer[0] = event.button.y;
			mouse_buffer[1] = event.button.x;
			mouse_buffer[2] = 0x7f & event.button.button;
			break;
		case SDL_USEREVENT:		// Use this to process incoming network connections
			utl |= 4;
			break;
		default: break;
	}
}

// NB: this routine handles simulating the hardware, updates video and 
void update() {
	++ticks;
	if (ticks % INTERRUPT_RATE == 0) interrupt();
	if (ticks % REFRESH_RATE == 0) SDL_GL_SwapBuffers;
}

// This is the main VM loop
void go() {
	cell instr;
	int a, b;
fetch:
	ip &= 0x0fff;
	instr = im[ip++];
	if (! (instr & 0x80000000)) { 
		up(instr);
		goto fetch;
	}
next_op:
	switch(instr & 0xff) {
		case 0x80: goto next;
		case 0x81: upr(ip); ip = tos(); down(); goto fetch;
		case 0x82: down(); goto next;
		case 0x83: snos(tos()); down(); goto next;
		case 0x84: upr(tos()); down(); goto next;
		case 0x85: stos(~tos()); goto next;
		case 0x86: snos(tos()&nos()); down(); goto next;
		case 0x87: snos(tos()|nos()); down(); goto next;
		case 0x88: snos(tos()^nos()); down(); goto next;
		case 0x89: mem_read(tos()); goto next;
		case 0x8a: up(nos() < tos() ? -1 : 0); goto next;
		case 0x8b: up(nos() == tos() ? -1 : 0); goto next;
		case 0x8c: stos(tos()<<1); goto next;
		case 0x8d: stos(tos()<<8); goto next;
		case 0x8e: up(0); goto next;
		case 0x8f: up(1); goto next;
		case 0x90: ip = rtos(); downr(); goto fetch;
		case 0x91: if (!nos()) down(); down(); goto next; 
				ip = tos(); down(); down(); goto fetch; 
		case 0x92: up(tos()); goto next;
		case 0x93: up(nos()); goto next;
		case 0x94: up(rtos()); downr(); goto next;
		case 0x95: stos(-(int)tos()); goto next;
		case 0x96: snos(tos()+nos()); down(); goto next;
		case 0x97: snos((int)tos()*(int)nos()); goto next; 
		case 0x98: a = tos(); b = nos(); stos(a/b); snos(a%b); goto next;
		case 0x99: mem_write(tos(),nos()); down(); goto next;
		case 0x9a: up(nos() > tos() ? -1 : 0); goto next;
		case 0x9b: up(nos() != tos() ? -1 : 0); goto next;
		case 0x9c: stos(tos()>>1); goto next;
		case 0x9d: stos(tos()>>8); goto next;
		case 0x9e: up(utl); goto next;
		case 0x9f: up(-1); goto next;
		case 0xa0: mem_move(-1); goto next;
		case 0xa1: up(cnt); goto next;
		case 0xa2: up(src); goto next;
		case 0xa3: up(dst); goto next;
		case 0xc0: mem_cmp(); goto next;
		case 0xc1: ++cnt; goto next;
		case 0xc2: up(0); mem_read(src++); goto next;
		case 0xc3: mem_write(dst++,tos()); goto next;
		case 0xe0: mem_move(1); goto next;
		case 0xe1: cnt = tos(); goto next;
		case 0xe2: src = tos(); goto next;
		case 0xe3: dst = tos(); goto next;
		default: break;
	}
next:
	update(); // System hook to simulate hardware
	instr >>= 8;
	if (instr == 0) goto fetch;
	goto next_op;
}

void init() {
	ticks = 0;
	SDL_Init(SDL_INIT_EVERYTHING);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	display = SDL_SetVideoMode(1280,720,32,SDL_OPENGL);
	glClearColor(1.0,1.0,1.0,1.0);
	if (! display) exit(NO_DISPLAY);
}

void reset() {
	ip = dsi = rsi = cnt = src = dst = utl = 0;
	ram = mmap(NULL,RAM_SIZE,PROT_READ|PROT_WRITE,MAP_ANON,-1,0);
	if (ram < 0) exit(NO_RAM);
	if (flash) {
		munmap(flash,flash_size);
		close(flash_fd);
	}
	flash = NULL;
}

void boot() {
	struct stat st;
	flash_fd = open(flash_file,O_RDWR,0600);
	if (flash_fd < 0) exit(NO_FILE);
	fstat(flash_fd,&st);
	flash_size = st.st_size;
	flash = mmap(NULL,flash_size,PROT_READ|PROT_WRITE,MAP_FILE|MAP_SHARED,flash_fd,0);
	if (flash < 0) exit(NO_MAP);
	if (flash_size < ROM_SIZE) exit(NO_ROM);
	memcpy(rom,flash,ROM_SIZE);
	memcpy(im,flash,ROM_SIZE);
	go();
}

int main (int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr,"Usage: %s [file]\n",argv[0]);
		return 0;
	}
	flash_file = argv[1];
	init();
	reset();
	boot();
	SDL_Quit();
	return 0;
}
