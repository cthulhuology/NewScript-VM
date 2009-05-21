////////////////////////////////////////////////////////////////////////////////
// ns.c
//
// Copyright 2009 David J. Goehrig  <dave@nexttolast.com>
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////////////////////
// headers
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
#include <sys/socket.h>
#include <math.h>
#include <net/bpf.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <pcap.h>

////////////////////////////////////////////////////////////////////////////////
// errors
#define NO_RAM		1
#define NO_FILE		2
#define NO_MAP		3
#define NO_ROM		4
#define NO_DISPLAY	5
#define NO_AUDIO	6
#define NO_NET_DEVICE	7
#define NO_NET_ADDR	8
#define NO_CAPTURE	9

////////////////////////////////////////////////////////////////////////////////
// sizes
#define CACHE_SIZE	4096
#define ROM_SIZE	4096
#define RAM_SIZE	1073741824
#define FLASH_SIZE	1073741824
#define NET_SIZE	4096

////////////////////////////////////////////////////////////////////////////////
// timings
#define REFRESH_RATE	1000000	
#define INTERRUPT_RATE	10000	

////////////////////////////////////////////////////////////////////////////////
// typedefs
typedef Uint32 cell;	// 32bit unsigned integer 
typedef void (*device_fo)(cell);
typedef cell (*device_fi)();

////////////////////////////////////////////////////////////////////////////////
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

////////////////////////////////////////////////////////////////////////////////
// System globals
cell flash_size;	// Size of Flash Image
cell flash_fd;		// File Descriptor of Flash Image
char* flash_file;	// Filename of Flash Image
SDL_Surface* display;	// Display Device
SDL_Event event;	// Last Event Polled
cell ticks;		// System clock
cell* ms;		// Memory Source
cell* md;		// Memory Destination

////////////////////////////////////////////////////////////////////////////////
// vm functions
void nop() { return; }
cell tos() { return ds[dsi]; }			// Top of Stack
cell nos() { return ds[7&(dsi-1)]; }		// Next on Stack
void up(cell c) { ds[dsi = 7&(dsi+1)] = c; }	// Push c onto Data Stack
void down() { dsi = 7&(dsi-1); }		// Drop Top of Stack
void stos(cell c) { ds[dsi] = c; }		// Store Top of Stack
void snos(cell c) { ds[7&(dsi-1)] = c; }	// Store Next on Stack
cell rtos() { return rs[rsi]; }			// Return Stack Top of Stack
void upr(cell c) { rs[rsi = 7&(rsi+1)] = c; }	// Push c onto Return Stack
void downr() { rsi = 7&(rsi-1); }		// Drop Top of Return Stack

////////////////////////////////////////////////////////////////////////////////
// memory address translation functions
void memory_source_address() {
	ms = src & 0x80000000 ? &flash[src & 0x7fffffff]:
		src < 0x1000 ? &rom[src] :
		src < 0x7ffffff9 ? &ram[src] :
		NULL;
}

void memory_destination_address() {
	md = dst & 0x80000000 ? &flash[dst] :
		dst < 0x1000 ? &im[dst] :
		dst < 0x7ffffff9 ? &ram[dst] :
		NULL;
}

////////////////////////////////////////////////////////////////////////////////
// network functions

char* net_device;
cell net_addr;
cell net_mask;
pcap_t* net_capture;

cell net_read_buffer[NET_SIZE];
cell net_read_index;
cell net_read_len;

cell net_write_buffer[NET_SIZE];
cell net_write_index;

void net_read_callback() {
	struct pcap_pkthdr hdr;
	const Uint8* packet = pcap_next(net_capture,&hdr);
	if (!packet) return;
	fprintf(stderr,"Got packet of %d bytes\n",hdr.len);
	memcpy(net_read_buffer,packet,hdr.caplen);
	net_read_index = 0;
	net_read_len = hdr.caplen;
}

cell net_read() {				// Read from Network Interface
	if (net_read_index >= net_read_len) return 0;
	return net_read_buffer[net_read_index++];
}

void net_write_callback() {
	if (0>pcap_inject(net_capture,net_write_buffer,net_write_index))
		pcap_perror(net_capture,"Write Error: ");
	net_write_index = 0;
}

void net_write(cell val) {			// Write to Network Interface
	net_write_index %= NET_SIZE;	
	net_write_buffer[net_write_index++] = val;
}

void network_init() {
	int id = getuid();
	seteuid(0);
	if (id == geteuid()) {
		fprintf(stderr,"Disabling network, to enable chmod u+s ns; chown root ns\n");
		return;
	}
	char err[PCAP_ERRBUF_SIZE];
	if (! (net_device = pcap_lookupdev(err))) {
		fprintf(stderr,"%s\n",err);
		exit(NO_NET_DEVICE);
	}
	if (pcap_lookupnet(net_device,&net_addr,&net_mask,err)) {
		fprintf(stderr,"%s\n",err);
		exit(NO_NET_ADDR);
	}
	if (! (net_capture = pcap_open_live(net_device,NET_SIZE,0,1,err))) {
		fprintf(stderr,"%s\n",err);
		exit(NO_CAPTURE);
	}
	seteuid(id);
	net_read_callback();
}

////////////////////////////////////////////////////////////////////////////////
// mouse functions
cell mouse_buffer[3] = { 0, 0, 0 };		// Last Mouse Event data buffer
cell mouse_buffer_index = 0;			// Index into Mouse Buffer (3 cycle read)
cell mouse_read() {				// Read one cell from mouse buffer, cyclic
	mouse_buffer_index %= 3;
	return mouse_buffer[mouse_buffer_index++];
}

////////////////////////////////////////////////////////////////////////////////
// keyboard functions
cell key_buffer = 0;				// Last Key Event data buffer
cell key_read() { return key_buffer; }		// Read one cell from last key buffer

cell keymap() {					// Maps from keyboard to Firth character map
	int c = event.key.keysym.sym;
	int map[] = { 				// Key to Character Map
		SDLK_0, SDLK_1, SDLK_2, SDLK_3, SDLK_4, SDLK_5, SDLK_6, SDLK_7, 
		SDLK_8, SDLK_9, SDLK_a, SDLK_b, SDLK_c, SDLK_d, SDLK_e, SDLK_f, 
		SDLK_g, SDLK_h, SDLK_i, SDLK_j, SDLK_k, SDLK_l, SDLK_m, SDLK_n, 
		SDLK_o, SDLK_p, SDLK_q, SDLK_r, SDLK_s, SDLK_t, SDLK_u, SDLK_v, 
		SDLK_w, SDLK_x, SDLK_y, SDLK_z, SDLK_COMMA, SDLK_PERIOD, SDLK_SLASH, SDLK_SEMICOLON, 
		SDLK_QUOTE, SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET, SDLK_BACKSLASH, SDLK_BACKQUOTE, SDLK_MINUS, SDLK_EQUALS, SDLK_SPACE, 
	};
	for (cell i = 0; map[i]; ++i)		// i is the key value in our character set
		if (map[i] == c)
			return event.key.keysym.mod & KMOD_SHIFT ? 0x30 + i : i;
	return	c == SDLK_RETURN ? 0x61 : 	// Non Printing Characters / Meta Keys
		c == SDLK_TAB ? 0x51 : 
		c == SDLK_BACKSPACE ? 0x62 : 
		c == SDLK_LMETA ? 0x63 : 
		c == SDLK_RMETA ? 0x63 : 
		c == SDLK_LALT ? 0x64 : 
		c == SDLK_RALT ? 0x64 : 
		c == SDLK_LCTRL ? 0x65 : 
		c == SDLK_RCTRL ? 0x65 : 
		0x66;				// #66 = unknown key
}


////////////////////////////////////////////////////////////////////////////////
// video functions
Sint16 x,y,dx,dy;				// VGDD State Machine position data x,y,dx,dy
cell texture_memory[1280*720];			// 720p VGDD Texture Memory, used for blitting to screen
cell texture_index = 0;				// Index into Texture Memory
cell video_color[2];				// VGDD Color Buffer, 0 RGBA line, 1 RGBA fill
cell video_command[3];				// VGDD command buffer, 0 tag 1 data 2 data
cell video_index = 0;				// VGDD command buffer index 

void video_init() {
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	display = SDL_SetVideoMode(1280,720,32,SDL_OPENGL);
	glViewport(0,0,1280,720);
	glClearColor(1.0,1.0,1.0,1.0);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0,1280.0,0.0,720.0,1.0,-1.0);
	glEnable(GL_LINE_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	if (! display) exit(NO_DISPLAY);
}

void vid_clear() {
	glClear(GL_COLOR_BUFFER_BIT);
	texture_index = 0;
	video_index = 0;
}

void vid_at() {					// Set x,y position
	x = (Sint16)(video_command[1]&0xffff);
	y = (Sint16)(video_command[2]&0xffff);
	video_index = 0;
}

void vid_to() {					// Alter x,y by dx,dy
	x += (Sint16)(video_command[1]&0xffff);
	y += (Sint16)(video_command[2]&0xffff);
	video_index = 0;
}

void vid_by() {					// Set dx,dy dimensions
	dx = (Sint16)(video_command[1]&0xffff);
	dy = (Sint16)(video_command[2]&0xffff);
	video_index = 0;
}


void vid_set_color(cell i) {
	glColor4ub(video_color[i]&0xff,
		(video_color[i]>>8)&0xff,
		(video_color[i]>>16)&0xff,
		(video_color[i]>>24)&0xff);
}

void vid_line() {				// Draw a line from x,y to x+dx,y+dy
	vid_set_color(0);
	glBegin(GL_LINE_STRIP);
	glVertex3i(x,y,0);
	glVertex3i(x+dx,y+dy,0);
	glEnd();
	x += dx;
	y += dy;
	video_index = 0;
}

void vid_arc() {			// Draw an arc tangential to (x,y)(cx,cy), from x,y to x+dx,y+dy
	double dis = sqrt(dx*dx+dy*dy);
	vid_set_color(0);
	glBegin(GL_LINE_STRIP);
	if (video_command[1]) 
		for ( double d = 0; d < 355.0/226; d += 355.0/(113*dis) ) 
			glVertex3d(x + dx*sin(d),y+dy*(1-cos(d)),0);
	else
		for ( double d = 0; d < 355.0/226; d += 355.0/(113*dis) ) 
			glVertex3d(x + dx*(1-sin(d)),y+dy*cos(d),0);
	glEnd();
	x += dx;
	y += dy;
	video_index = 0;
}

void vid_rect() {				// Draw a rectangle at x,y to x+dx,y+dy
	vid_set_color(1);
	glBegin(GL_QUADS);
	glVertex3i(x,y+dy,0);
	glVertex3i(x+dx,y+dy,0);
	glVertex3i(x+dx,y,0);
	glVertex3i(x,y,0);
	glEnd();
	video_index = 0;
	x += dx;
	y += dy;
}

void vid_color() {				// set line color
	video_color[0] = video_command[1];
	video_index = 0;
}

void vid_fill() {				// set fill color
	video_color[1] = video_command[1];
	video_index = 0;
}

void vid_draw() {
	x += dx;
	y += dy;
	video_index = 0;
}

void vid_blit() {				// Write to VGDD Texture Memory
	for (int i = 0; i < cnt; ++i) {
		texture_memory[texture_index++] = 0;
	}
	video_index = 0;
}

void vid_write(cell val) {			// Write to VGDD command buffer
	video_index %= 3;
	video_command[video_index++] = val;
	switch(video_command[0]) {
		case 0x0: if (video_index == 1) vid_clear(); break;
		case 0x1: if (video_index == 3) vid_at(); break;
		case 0x2: if (video_index == 3) vid_to(); break;
		case 0x3: if (video_index == 3) vid_by(); break;
		case 0x4: if (video_index == 1) vid_line(); break;
		case 0x5: if (video_index == 2) vid_arc(); break;
		case 0x6: if (video_index == 1) vid_rect(); break;
		case 0x7: if (video_index == 2) vid_color(); break;
		case 0x8: if (video_index == 2) vid_fill(); break;
		case 0x9: if (video_index == 1) vid_draw(); break;
		case 0xa: if (video_index == 1) vid_blit(); break;
		default: break;
	}
}

cell vid_frame[] = {
	0,
	7,0xffff00ff,
	3,0,180,
	4,
	3,20,20,
	5,0,
	3,380,0,
	4,
	3,20,-20,
	5,1,
	3,0,-180,
	4,
	3,-20,-20,
	5,0,
	3,-380,0,
	4,
	3,-20,20,
	5,1 };

////////////////////////////////////////////////////////////////////////////////
// audio functions
cell audio_memory[44100];	// default buffer is 1sec of audio 44100Hz 2 channels 16bit PCM linear
cell audio_index = 0;		// index into audio memory
cell audio_cb_index = 0;

void audio_callback(void *userdata, Uint8 *stream, int len) {
	if (audio_cb_index >= audio_index) {
		audio_cb_index = 0;
		SDL_PauseAudio(1);
		return;
	}
	len = len > sizeof(cell)*(audio_index - audio_cb_index) ? 	
		sizeof(cell)*(audio_index - audio_cb_index): 
		len;
	SDL_MixAudio(stream, (Uint8*)&audio_memory[audio_cb_index], len, SDL_MIX_MAXVOLUME);
	audio_cb_index += len/sizeof(cell);
}

void audio_init() {
	audio_cb_index = 0;
	audio_index = 0;
	SDL_AudioSpec as = { 44100, AUDIO_S16LSB, 2, 0, 1024, 0, 0, audio_callback, 0 };
	if (SDL_OpenAudio(&as,NULL)) exit(NO_AUDIO);
}

void aud_write(cell val) {	// Write to audio memory
	SDL_LockAudio();
	memory_source_address();
	if (!ms) return;
	if (utl&0x08) {
		audio_index %= 44100;
		audio_memory[audio_index++] = val;
		return;
	}
	memcpy(audio_memory,ms,cnt*sizeof(cell));
	audio_index += cnt;
	SDL_UnlockAudio();
	if (SDL_AUDIO_PLAYING != SDL_GetAudioStatus()) SDL_PauseAudio(0);
}

////////////////////////////////////////////////////////////////////////////////
// memory functions
cell* device_read(device_fi f) {
	for (int i = 0; i < cnt; ++i) stos(f());
	return NULL;
}

cell* device_write(cell* src, device_fo f) {
	if (! src || ! f ) return NULL;
	for (int i = 0; i < cnt; ++i) f(src[i]);
	return NULL;
}

// Read one byte from a memory addr
void mem_read(cell addr) {			// Read from a memory address (device I/O too)
	addr & 0x80000000 ? stos(flash[addr & 0x7fffffff]):
	addr == 0x7fffffff ? stos(net_read()):
	addr == 0x7ffffffe ? stos(0):
	addr == 0x7ffffffd ? stos(0):
	addr == 0x7ffffffc ? stos(mouse_read()):
	addr == 0x7ffffffb ? stos(key_read()):
	addr == 0x7ffffffa ? stos(0):
	addr == 0x7ffffff9 ? stos(0):
	addr < 0x1000 ? stos(rom[addr]):
	stos(ram[addr]);
}

// Write one byte from a memory addr
void mem_write(cell addr, cell value) {		// Write to a memory address (device I/O too)
	addr & 0x80000000 ? (flash[addr & 0x7fffffff] = value):
	addr == 0x7fffffff ? net_write(value):
	addr == 0x7ffffffe ? vid_write(value):
	addr == 0x7ffffffd ? aud_write(value): 
	addr == 0x7ffffffc ? nop():
	addr == 0x7ffffffb ? nop():
	addr == 0x7ffffffa ? nop():
	addr == 0x7ffffff9 ? nop():
	(ram[addr] = value);
}

void mem_move(int d) {				// Copy memory from one location to another
	utl &= 0xfffffff7;
	memory_source_address();
	memory_destination_address();
	if (ms && md) 
		d < 0 ? memmove(md-cnt,ms-cnt,cnt*sizeof(cell)) : memmove(md,ms,cnt*sizeof(cell));	
	else if (!ms) 
		0x7fffffff == src ? device_read(net_read):
		0x7ffffffc == src ? device_read(mouse_read):
		0x7ffffffb == src ? device_read(key_read) : nop();
	else if (!md)
		0x7fffffff == dst ? device_write(ms,net_write):
		0x7ffffffe == dst ? device_write(ms,vid_write):
		0x7ffffffd == dst ? device_write(ms,aud_write) : nop();
	utl |= 0x08;
}

// NB: we can't compare device data!  Copy to a buffer first.  dst = im, src = rom
void mem_cmp() {					// Compare to regions of memory (no device I/O)
	utl &= 0xfffffff7;
	memory_source_address();
	memory_destination_address();
	if (ms && md) 
		cnt = memcmp(ms,md,cnt*sizeof(cell));
	utl |= 0x08;
}

////////////////////////////////////////////////////////////////////////////////
// interrupt simulation
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
			if (event.key.keysym.sym == SDLK_ESCAPE) {
				munmap(flash,flash_size);
				close(flash_fd);
				SDL_Quit();
				exit(0);
			}
			break;
		case SDL_KEYUP:
			utl |= 1;
			key_buffer = 0x7f & keymap();
			break;
		case SDL_MOUSEMOTION:
			utl |= 2;
			mouse_buffer[0] = event.motion.x;
			mouse_buffer[1] = event.motion.y;
			break;
		case SDL_MOUSEBUTTONDOWN:
			utl |= 2;
			mouse_buffer[2] = 0x80 | (1 << (event.button.button-1));
			break;
		case SDL_MOUSEBUTTONUP:
			utl |= 2;
			mouse_buffer[2] = 0x7f & (1 << (event.button.button-1));
			break;
		case SDL_USEREVENT:		// Use this to process incoming network connections
			utl |= 4;
			break;
		default: break;
	}
}

////////////////////////////////////////////////////////////////////////////////
// system clock simulation
void update() {					// Update the system clock & simulate attached devices
	++ticks;
	if (!(ticks % INTERRUPT_RATE)) interrupt();
	if (!(ticks % REFRESH_RATE)) {
		vid_write(1);
		vid_write(mouse_read());
		vid_write(720-mouse_read());
		mouse_read();
		memcpy(&ram[0x4000],vid_frame,sizeof(vid_frame));
		src = 0x4000;
		dst = 0x7ffffffe;
		cnt = sizeof(vid_frame)/sizeof(cell);
		mem_move(1);
		SDL_GL_SwapBuffers();
	}
}

////////////////////////////////////////////////////////////////////////////////
// cpu simulation
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

////////////////////////////////////////////////////////////////////////////////
// platform initialization
void init() {				// Initialize Platform Specific Application Settings
	ticks = 0;
	SDL_Init(SDL_INIT_EVERYTHING);
	video_init();
	audio_init();
	network_init();
}

////////////////////////////////////////////////////////////////////////////////
// vm initialization
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

////////////////////////////////////////////////////////////////////////////////
// boot simulation
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

////////////////////////////////////////////////////////////////////////////////
// entry point
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

////////////////////////////////////////////////////////////////////////////////
// end
