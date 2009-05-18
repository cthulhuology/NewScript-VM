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
typedef Uint32 cell;	// 32bit unsigned integer 

// VM globals
cell ip;		// Instruction Pointer
cell dsi;		// Data Stack Index
cell ds[8];		// Data Stack
cell rsi;		// Return Stack Index
cell rs[8];		// Return STack
cell cnt;		// Count Register
cell src;		// Source Register
cell dst;		// Destination Register
cell utl;		// Utility / Status Register
cell im[CACHE_SIZE];	// Instruction Memory (modified Havard Architecture)
cell rom[ROM_SIZE];	// Read Only Memory (first 16k of Flash Image)
cell* ram;		// RAM pointer	(1GB)
cell* flash;		// FLASH Image pointer

// System globals
cell flash_size;	// Size of Flash Image
cell flash_fd;		// File Descriptor of Flash Image
char* flash_file;	// Filename of Flash Image
SDL_Surface* display;	// Display Device
SDL_Event event;	// Last Event Polled
cell ticks;		// System clock

// functions

cell tos() { return ds[dsi]; }			// Top of Stack
cell nos() { return ds[7&(dsi-1)]; }		// Next on Stack
void up(cell c) { ds[dsi = 7&(dsi+1)] = c; }	// Push c onto Data Stack
void down() { dsi = 7&(dsi-1); }		// Drop Top of Stack
void stos(cell c) { ds[dsi] = c; }		// Store Top of Stack
void snos(cell c) { ds[7&(dsi-1)] = c; }	// Store Next on Stack
cell rtos() { return rs[rsi]; }			// Return Stack Top of Stack
void upr(cell c) { rs[rsi = 7&(rsi+1)] = c; }	// Push c onto Return Stack
void downr() { rsi = 7&(rsi-1); }		// Drop Top of Return Stack

cell net_read() {				// Read from Network Interface

}

cell mouse_buffer[3] = { 0, 0, 0 };		// Last Mouse Event data buffer
cell mouse_buffer_index = 0;			// Index into Mouse Buffer (3 cycle read)
cell mouse_read() {				// Read one cell from mouse buffer, cyclic
	mouse_buffer_index %= 3;
	return mouse_buffer[mouse_buffer_index++];
}

cell key_buffer = 0;				// Last Key Event data buffer
cell key_read() { return key_buffer; }		// Read one cell from last key buffer

void net_write(cell val) {			// Write to Network Interface

}

SDL_Rect video_rect;				// VGDD State Machine position data x,y,dx,dy
cell texture_memory[1280*720];			// 720p VGDD Texture Memory, used for blitting to screen
cell texture_index = 0;				// Index into Texture Memory
cell video_color[2];				// VGDD Color Buffer, 0 RGBA line, 1 RGBA fill
cell video_command[3];				// VGDD command buffer, 0 tag 1 data 2 data
cell video_index = 0;				// VGDD command buffer index 

void vid_write(cell val) {			// Write to VGDD command buffer
	
	
}

void vid_data() {				// Write to VGDD Texture Memory
	texture_memory[texture_index++] = 0;
}

void vid_at(cell x, cell y) {			// Set x,y position
	video_rect.x = (Sint16)x;
	video_rect.y = (Sint16)y;
}

void vid_to(cell dx, cell dy) {			// Alter x,y by dx,dy
	video_rect.x += (Sint16)dx;
	video_rect.y += (Sint16)dy;
}

void vid_by(cell dx, cell dy) {			// Set dx,dy dimensions
	video_rect.w = (Uint16)dx;
	video_rect.h = (Uint16)dy;
}

void vid_as(cell x, cell y, cell dx, cell dy) {	// Set x,y,dx,dy position and dimensions
	video_rect.x = (Sint16)x;
	video_rect.y = (Sint16)y;
	video_rect.w = (Uint16)dx;
	video_rect.h = (Uint16)dy;
}

void vid_line() {				// Draw a line from x,y to x+dx,y+dy
	glColor4ub(video_color[0]&0xff,(video_color[0]>>8)&0xff,(video_color[0]>>16)&0xff,(video_color[0]>>24)&0xff);
	glBegin(GL_LINES);
	glVertex3i(video_rect.x,video_rect.y,0);
	glVertex3i(video_rect.x+(Sint16)video_rect.w,video_rect.y+(Sint16)video_rect.h,0);
	glEnd();
}

void vid_arc() {			// Draw an arc from x,y tangential to (x,y)(cx,cy) to (x+dx,y+dy)
	glColor4ub(video_color[0]&0xff,(video_color[0]>>8)&0xff,(video_color[0]>>16)&0xff,(video_color[0]>>24)&0xff);
	// TODO
}

void vid_rect() {				// Draw a rectangle at x,y to x+dx,y+dy
	glColor4ub(video_color[1]&0xff,(video_color[1]>>8)&0xff,(video_color[1]>>16)&0xff,(video_color[1]>>24)&0xff);
	glBegin(GL_QUADS);
	glVertex3i(video_rect.x,video_rect.y+video_rect.h,0);
	glVertex3i(video_rect.x+video_rect.w,video_rect.y+video_rect.h,0);
	glVertex3i(video_rect.x+video_rect.w,video_rect.y,0);
	glVertex3i(video_rect.x,video_rect.y,0);
	glEnd();
}

void vid_color() {				// set line color
	video_color[0] = video_command[1];
}

void vid_fill() {				// set fill color
	video_color[1] = video_command[1];
}

cell audio_memory[44100];	// default buffer is 1sec of audio 44100Hz 2 channels 16bit PCM linear
cell audio_index = 0;		// index into audio memory
void aud_write(cell val) {	// Write to audio memory
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

void mem_read(cell addr) {			// Read from a memory address (device I/O too)
	addr & 0x80000000 ? stos(flash[addr] & 0x7fffffff):
	addr == 0x7fffffff ? stos(net_read()):
	addr == 0x7ffffffc ? stos(mouse_read()):
	addr == 0x7ffffffb ? stos(key_read()):
	addr < 0x1000 ? stos(rom[addr]):
	stos(ram[addr]);
}

void mem_write(cell addr, cell value) {		// Write to a memory address (device I/O too)
	addr & 0x80000000 ? (flash[addr] = value):
	addr == 0x7fffffff ? net_write(value):
	addr == 0x7ffffffe ? vid_write(value):
	addr == 0x7ffffffd ? aud_write(value):
	(ram[addr] = value);
}

void mem_move(int d) {				// Copy memory from one location to another
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
void mem_cmp() {					// Compare to regions of memory (no device I/O)
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

int keymap() {					// Maps from keyboard to Firth character map
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

void interrupt() {			// Simulate a device interrupt
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
void update() {					// Update the system clock & simulate attached devices
	++ticks;
	if (!(ticks % INTERRUPT_RATE)) interrupt();
	if (!(ticks % REFRESH_RATE)) SDL_GL_SwapBuffers;
}

// This is the main VM loop
void go() {					// Simulate Decoder & ALU
	cell instr;
	int a, b;
fetch:
	update(); // System hook to simulate hardware, 1 clock each
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
	instr >>= 8;
	if (instr == 0) goto fetch;
	goto next_op;
}

void init() {				// Initialize Platform Specific Application Settings
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

void reset() {				// Reset the VM, unmap flash if loaded
	ip = dsi = rsi = cnt = src = dst = utl = 0;
	ram = mmap(NULL,RAM_SIZE,PROT_READ|PROT_WRITE,MAP_ANON,-1,0);
	if (ram < 0) exit(NO_RAM);
	if (flash) {
		munmap(flash,flash_size);
		close(flash_fd);
	}
	flash = NULL;
}

void boot() {				// Load the flash image, and start VM
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

int main (int argc, char** argv) {	//  Main Program Entry point
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
