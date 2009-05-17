// ns.c
//
// Copyright (C) 2009 David J. Goehrig
// All Rights Reserved
//
//	A prototype NewScript portable VM
//

#include "SDL.h"
#include <mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

// errors
#define NO_RAM		1
#define NO_FILE		2
#define MAP_FAILED	3
#define NO_ROM		4

// sizes
#define CACHE_SIZE	4096
#define ROM_SIZE	4096
#define RAM_SIZE	1073741824	(1GB currently)
#define FLASH_SIZE	1073741824	(1GB currently)

// typedefs
typedef UInt32 cell;

// globals
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
cell flash_size;
cell flash_fd;
cell flash_file;

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

void mem_read(cell addr) {
	addr & 0x80000000 ? stos(flash[addr] & 0x7fffffff):
	addr == 0x7fffffff ? stos(net_read()):
	addr == 0x7ffffffe ? stos(vid_read()):
	addr == 0x7ffffffd ? stos(aud_read()):
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
	ram[addr] = value;
}

void mem_move(int dir) {
	cell* ms = NULL, md = NULL;

	src & 0x80000000 ? ms = &flash[src]:
	src < 0x1000 ? ms = &rom[src]:
	0x7fffffff == src ? net_read(-1):
	0x7ffffffc == src ? mouse_read(-1):
	0x7ffffffb == src ? key_read(-1):
	ms = &ram[src];

	dst & 0x80000000 ? md = &flash[dst]:
	dst < 0x1000 ? md = &im[dst]:
	0x7fffffff == dst ? net_write(-1):
	0x7ffffffe == dst ? vid_write(-1):
	0x7ffffffd == dst ? aud_write(-1):
	md = &ram[dst];

	if (ms && md) 
		d < 0 ? memmove(md-cnt,ms-cnt,cnt*sizeof(cell)) : memmove(md,ms,cnt*sizeof(cell));	
}

void mem_cmp() {

}

void go() {
	int a, b;
fetch:
	cell instr = im[ip++];
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
		case 0x90: ip = rtos(); downr(); goto fetch();
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
		default:
	}
next:
	instr >>= 8;
	if (instr == 0) goto fetch;
	goto next_op;
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
	struct stat_struct st;
	flash_fd = open(flash_file,O_RDWR,0600);
	if (flash_fd < 0) exit(NO_FILE);
	fstat(flash_fd,&st);
	flash_size = st.st_size;
	flash = mmap(NULL,flash_size,PROT_READ|PROT_WRITE,MAP_FILE|MAP_SHARED,flash_fd,0);
	if (flash < 0) exit(MAP_FAILED);
	if (flash_size < ROM_SIZE) exit(NO_ROM);
	memcpy(rom,flash,ROM_SIZE);
	memcpy(im,flash,ROM_SIZE);
	go();
}

int main (int argc, char** argv) {
	init();
	reset();
	boot();
	return 0;
}
