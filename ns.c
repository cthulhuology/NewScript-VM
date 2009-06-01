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
#include "SDL_image.h"
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
#define RAM_SIZE	268435456
#define FLASH_SIZE	268435456
#define NET_SIZE	4096

////////////////////////////////////////////////////////////////////////////////
// timings
#define REFRESH_RATE	(1000 / 24)
#define INTERRUPT_RATE	1000	
#define NETWORK_RATE	100

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
cell samples = 0;	// Clock rate samples
cell rate = 0;		// floating rate average
cell ticks;		// System clock
cell period;		// Ticks per frame refresh
cell last;		// Last Frame in ms
cell now;		// Current Time in ms
cell* ms;		// Memory Source
cell* md;		// Memory Destination

////////////////////////////////////////////////////////////////////////////////
// vm functions
INLINE void nop() { return; }
INLINE cell tos() { return ds[dsi]; }			// Top of Stack
INLINE cell nos() { return ds[7&(dsi-1)]; }		// Next on Stack
INLINE void up(cell c) { ds[dsi = 7&(dsi+1)] = c; }	// Push c onto Data Stack
INLINE void down() { dsi = 7&(dsi-1); }			// Drop Top of Stack
INLINE void stos(cell c) { ds[dsi] = c; }		// Store Top of Stack
INLINE void snos(cell c) { ds[7&(dsi-1)] = c; }		// Store Next on Stack
INLINE cell rtos() { return rs[rsi]; }			// Return Stack Top of Stack
INLINE void upr(cell c) { rs[rsi = 7&(rsi+1)] = c; }	// Push c onto Return Stack
INLINE void downr() { rsi = 7&(rsi-1); }		// Drop Top of Return Stack

////////////////////////////////////////////////////////////////////////////////
// memory address translation functions
void source() {					// Switch between
	ms = src & 0x80000000 ? &flash[src & 0x7fffffff]:	// VM addressing 
		src < 0x1000 ? &rom[src] :			// and C style native
		src < 0x7ffffff9 ? &ram[src] :			// addressing for
		NULL;						// NUMA architecture.
}								// These functions
								// map flash memory
void destination() {				// address 0x80000000+
	md = dst & 0x80000000 ? &flash[dst] :			// to the memory image
		dst < 0x1000 ? &im[dst] :			// and addresses < 0x1000
		dst < 0x7ffffff9 ? &ram[dst] :			// to ROM or IM with all
		NULL;						// others going to RAM
}								// I/O devices -> NULL

////////////////////////////////////////////////////////////////////////////////
// network functions

char* net_device = NULL;		// name of the interface on which we listen
cell net_addr = 0;			// our network address, eg. 192.168.1.1 
cell net_mask = 0;			// our netmask, eg. 255.255.255.0
pcap_t* net_capture = NULL;		// a handle to the packet capture device

cell net_read_buffer[NET_SIZE];		// an input buffer for incoming packets
cell net_read_index = 0;		// an index into the read buffer
cell net_read_len = 0;			// the number of bytes read last read

cell net_write_buffer[NET_SIZE];	// an output buffer for outgoing packets
cell net_write_index = 0;		// an index into the write buffer

void net_read_callback() {
	struct pcap_pkthdr hdr;					// We read the next
	const Uint8* packet = pcap_next(net_capture,&hdr);	// packet available
	if (!packet) return;					// and copy as many
	fprintf(stderr,"Got packet of %d bytes\n",hdr.len);	// bytes to the read
	memcpy(net_read_buffer,packet,hdr.caplen);		// buffer as we can
	net_read_index = 0;					// and then reset the
	net_read_len = hdr.caplen;				// read index / length
}

cell net_read() {					// Read from Network Interface
	if (net_read_index >= net_read_len) return 0;	// read_len is amount read
	return net_read_buffer[net_read_index++];	// reads one cell at a time
}							// callback will reset on us!

void net_write_callback() {				// Writes the full output buffer
	if (net_write_index == 0) return;
	if (0>pcap_inject(net_capture,net_write_buffer,net_write_index))
		pcap_perror(net_capture,"Write Error: ");
	net_write_index = 0;				// And resets the write index
}

void net_write(cell val) {				// Write to Network Interface
	net_write_index %= NET_SIZE;			// one cell at a time, and loops
	net_write_buffer[net_write_index++] = val;	// if we write more than fits 
}							// within the write buffer!

char err[PCAP_ERRBUF_SIZE];		// Net error buffer for libpcap functions
void net_error(cell c) {		// Dump out the libpcap error message and
	fprintf(stderr,"%s\n",err);	// return one of our exit codes if something
	exit(c);			// goes wrong.  We don't bother recovering
}					// as there are too many contingencies on user code.

void network_init() {			// To initialize the network we need to gain root access
	int id = getuid();		// so that the BPF or LPF can be set read/write and get
	seteuid(0);			// raw link level data out of the NIC.  As such the app
	if (id == geteuid()) {		// must be setuid to root if we want to use network support
		fprintf(stderr,"Disabling network, to enable chmod u+s ns; chown root ns\n");
		return;			// If the app isn't setuid to root, we disable networking.
	}				// On the other hand, we use libpcap to streamline the 
	if (! (net_device = pcap_lookupdev(err))) 	// initialization of the BPF/LPF devices
		net_error(NO_NET_DEVICE);		// and quit using net_error if we can't get
	if (pcap_lookupnet(net_device,&net_addr,&net_mask,err)) 	// access to a valid device
		net_error(NO_NET_ADDR);			// Some times we fail due to no NIC being present
	if (! (net_capture = pcap_open_live(net_device,NET_SIZE,0,1,err))) // Other times because 
		net_error(NO_CAPTURE);	// We have no address for the NIC.  But when we're done
	seteuid(id);			// we restore the effective privs to user level ones, and
	net_read_callback();		// attempt to use the device to ensure we can still read packets.
}

void net_interrupt() {
	if (!net_capture) return;
	net_read_callback();
	net_write_callback();
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
		SDLK_QUOTE, SDLK_LEFTBRACKET, SDLK_RIGHTBRACKET, SDLK_BACKSLASH, 
		SDLK_BACKQUOTE, SDLK_MINUS, SDLK_EQUALS, SDLK_SPACE, 
	};
	for (cell i = 0; map[i]; ++i)		// i is the key value in our character set
		if (map[i] == c)
			return event.key.keysym.mod & KMOD_SHIFT ? 0x30 + i : i;
	return	c == SDLK_RETURN ? 0x61 : 	// Non Printing Characters / Meta Keys
		c == SDLK_TAB ? 0x60 : 
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
cell texture_id;				// OpenGL texture handle
cell video_color[2];				// VGDD Color Buffer, 0 RGBA line, 1 RGBA fill
cell video_command[3];				// VGDD command buffer, 0 tag 1 data 2 data
cell video_index = 0;				// VGDD command buffer index 

void video_init() {
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);		// In order to simulate a vector graphics
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);		// display device (VGDD), we are using a
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);		// 32 bit OpenGL scene, which we will use
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);		// as a 2D drawing surface, with nice 
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);		// line smoothing, rectangles, and texture
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);		// drawing.  Using the double buffer and 
	display = SDL_SetVideoMode(1280,720,32,SDL_OPENGL);	// a timed sync routine, we can emulate the
	glViewport(0,0,1280,720);				// output of a device that implements our
	glClearColor(1.0,1.0,1.0,1.0);				// drawing state machine.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();			
	glOrtho(0.0,1280.0,0.0,720.0,1.0,-1.0);		
	glMatrixMode(GL_MODELVIEW);				
	glShadeModel(GL_SMOOTH);
	glEnable(GL_LINE_SMOOTH);			
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);				
	glLoadIdentity();				
	glGenTextures(1,&texture_id);		
	if (! display) exit(NO_DISPLAY);			
}

void vid_clear() {
	glClear(GL_COLOR_BUFFER_BIT);	// This routine is op 0 in the VGDD instruction set.  It clears
	texture_index = 0;		// the video display and returns it to the base white color.
	video_index = 0;		// If a different background is desired, one can write a colored
}					// rectangle to the extent of the screen.

void vid_at() {					// Set x,y position
	x = (Sint16)(video_command[1]&0xffff);	// This is op 1 in the VGDD instruction set.  It takes
	y = (Sint16)(video_command[2]&0xffff);	// two args which set the 16bit signed offset of the
	video_index = 0;			// current drawing location.
}

void vid_to() {					// Alter x,y by dx,dy
	x += (Sint16)(video_command[1]&0xffff);	// This is op 2 in the VGDD instruction set.  It modifies
	y += (Sint16)(video_command[2]&0xffff);	// the current drawing position by two 16bit signed offsets.
	video_index = 0;
}

void vid_by() {					// Set dx,dy dimensions
	dx = (Sint16)(video_command[1]&0xffff); // Op 3 in the VGDD instruction set, this routine sets the
	dy = (Sint16)(video_command[2]&0xffff);	// dimensions of the drawing commands using two 16bit signed
	video_index = 0;			// values.  The current drawing positions is updated to 
}						// the x+dx,y+dy location upon completion of any drawing.


void vid_set_color(cell i) {
	glColor4ub(video_color[i]&0xff,		// This function sets the RGBA color, R is the LSB.
		(video_color[i]>>8)&0xff,	// The argument to this op is a single 32 bit value which
		(video_color[i]>>16)&0xff,	// contains all 4 8 bit unsigned 0-255 color values.
		(video_color[i]>>24)&0xff);	// This is the foreground or stroke color for line drawing
}

void vid_line() {				// Draw a line from x,y to x+dx,y+dy
	vid_set_color(0);			// Op 4 will draw a line from the current drawing position
	glBegin(GL_LINE_STRIP);			// to the point defined by the delta set with op 3.
	glVertex3i(x,y,0);			// The line uses the color set by Op 7, and will do an full
	glVertex3i(x+dx,y+dy,0);		// alpha blend with the underlying image.
	glEnd();
	x += dx;				// Upon completion the new drawing position will be at
	y += dy;				// x + dx, y + dy  and the next video instruction will
	video_index = 0;			// be loaded
}

void vid_arc() {				// Draw an arc 
	double dis = sqrt(dx*dx+dy*dy);		// This routine contains Op 5 which draws either a horizontal
	vid_set_color(0);			// or vertical arc, depending on the argument.  The arc
	glBegin(GL_LINE_STRIP);			// starts at x,y and goes to x+dx,y+dy, and is smooth
	if (video_command[1]) 
		for ( double d = 0; d < 355.0/226; d += 355.0/(113*dis) ) // If the arg is 1, the arc is
			glVertex3d(x + dx*sin(d),y+dy*(1-cos(d)),0);	  // tangential to a line parallel to 
	else								  // y through x,y and if the arg 
		for ( double d = 0; d < 355.0/226; d += 355.0/(113*dis) ) // is 0 then the arc is tangential
			glVertex3d(x + dx*(1-sin(d)),y+dy*cos(d),0);	  // to x through x,y.
	glEnd();
	x += dx;				// Upon completion the final drawing position is updated
	y += dy;				// to x+dx,y+dy, and the next instruction is executed.
	video_index = 0;
}

void vid_rect() {				// Draw a rectangle at x,y to x+dx,y+dy
	vid_set_color(1);			// Op 6 will fill a rectangle at x,y that is dx wide and dy
	glBegin(GL_QUADS);			// high, in the fill color set by Op 8.  
	glVertex3i(x,y+dy,0);
	glVertex3i(x+dx,y+dy,0);		// Upon completion the drawing position is updated
	glVertex3i(x+dx,y,0);			// to x+dx,y+dy and the next instruction is executed
	glVertex3i(x,y,0);
	glEnd();
	video_index = 0;
	x += dx;
	y += dy;
}

void vid_color() {				// set line color
	video_color[0] = video_command[1];	// Op 7, the arg is a 32 bit RGBA color value
	video_index = 0;			// with R in the LSB
}

void vid_fill() {				// set fill color
	video_color[1] = video_command[1];	// Op 8, the arg is a 32 bit RGBA color value
	video_index = 0;			// with R in the LSB
}

void vid_draw() {				// Draw a texture image at x,y to dx,dy
	glBindTexture(GL_TEXTURE_2D, texture_id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	glEnable(GL_TEXTURE_2D);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	glBegin(GL_QUADS);
	glTexCoord2s(0,0);
	glVertex3i(x,y+dy,0);
	glTexCoord2s(1,0);
	glVertex3i(x+dx,y+dy,0);
	glTexCoord2s(1,1);
	glVertex3i(x+dx,y,0);
	glTexCoord2s(0,1);
	glVertex3i(x,y,0);
	glEnd();
	x += dx;
	y += dy;
	video_index = 0;
}

void vid_blit() {				// Write to VGDD Texture Memory
	source();
	texture_index = 0;
	for (int i = 0; i < cnt; ++i)  texture_memory[texture_index++] = ms[i];
	glBindTexture(GL_TEXTURE_2D,texture_id);
	glTexImage2D(GL_TEXTURE_2D,0,4,dx,dy,0,GL_RGBA,GL_UNSIGNED_BYTE,texture_memory);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	video_index = 0;
}

struct {
	void (*cmd)();
	cell count;
} vid_vector[] = {
	{ vid_clear, 1 }, { vid_at, 3 },  { vid_to, 3 },   { vid_by, 3 },   
	{ vid_line, 1 },  { vid_arc, 2 }, { vid_rect, 1 }, 
	{ vid_color, 2 }, { vid_fill, 2 }, 
	{ vid_draw, 1 },  { vid_blit, 1 }
};

void vid_write(cell val) {			// Write to VGDD command buffer
	video_index %= 3;			// by writing to port 0x7ffffffe one can issue VGDD opcodes
	video_command[video_index++] = val;	// to the video coprocessor
	if (vid_vector[video_command[0]].count == video_index) 
		vid_vector[video_command[0]].cmd();
}						// to reset the video display, the sequence: 0 0 0

////////////////////////////////////////////////////////////////////////////////
// audio functions
cell audio_memory[44100];	// default buffer is 1sec of audio 44100Hz 2 channels 16bit PCM linear
cell audio_index = 0;		// index into audio memory, current write address
cell audio_cb_index = 0;	// read offset of the callback routine

void audio_callback(void *userdata, Uint8 *stream, int len) {   // the audio emulation system uses a circular
	if (audio_cb_index == audio_index) {			// write buffer, and a chasing read pointer
		audio_cb_index = 0;				// the system will play whatever audio is
		SDL_PauseAudio(1);				// written to the audio memory, and stop
		return;						// as soon as the current write location 
	}							// is reached.  Mixing occurs at max volume
	len = len > sizeof(cell)*(audio_index - audio_cb_index) ? 	// so the data written must be 
		sizeof(cell)*(audio_index - audio_cb_index): 		// pre-mixed if a lower volume is
		len;							// required.
	SDL_MixAudio(stream, (Uint8*)&audio_memory[audio_cb_index], len, SDL_MIX_MAXVOLUME);
	audio_cb_index += len/sizeof(cell);			// Up to one second of audio may be written
}								// and played back at a time.

void audio_init() {			// We initialize audio playback to Red Book CD audio, 44.1kHz
	audio_cb_index = 0;		// 16bit signed linear PCM data.  We use a low latency read buffer
	audio_index = 0;		// so that sound can be responsive if we want it that way.
	SDL_AudioSpec as = { 44100, AUDIO_S16LSB, 2, 0, 1024, 0, 0, audio_callback, 0 };
	if (SDL_OpenAudio(&as,NULL)) exit(NO_AUDIO);	// Failure to initialize sound exits with error
}

void aud_write(cell val) {				// Write to audio memory
	SDL_LockAudio();				
	source();					// Individual samples, 2 channels at a time, are 
	if (!ms) return;				// scheduled as mass write using the DMA copy
	if (utl&0x08) {					// instructions.  The prefered method is to copy
		audio_index %= 44100;			// up to 1/44 seconds of audio, and to prime the
		audio_memory[audio_index++] = val;	// buffers with new sound every 1/44th of a second
		return;					// this keeps the latency fairly low, and avoids
	}						// buffer starvation.  Now fewer than 1024 samples
	memcpy(audio_memory,ms,cnt*sizeof(cell));	// should be written at any time.
	audio_index += cnt;
	SDL_UnlockAudio();				// Writes are exclusive, and upon completion will
	if (SDL_AUDIO_PLAYING != SDL_GetAudioStatus()) SDL_PauseAudio(0);	// trigger audio playback
}

////////////////////////////////////////////////////////////////////////////////
// memory functions
cell* device_read(device_fi f) {			// This utility function is used to do a simple
	for (int i = 0; i < cnt; ++i) stos(f());	// device read routine.  It can pull 0 to cnt
	return NULL;					// register cells and place them on the stack.
}							// NB: the stack is only 8 deep!

cell* device_write(cell* src, device_fo f) {		// This utility function will write a sequence 
	if (! src || ! f ) return NULL;			// of bytes from an area in memory to the
	for (int i = 0; i < cnt; ++i) f(src[i]);	// desired function.  This is useful for wrapping
	return NULL;					// device writes in the DMA routines.
}

// Read one byte from a memory addr
void mem_read(cell addr) {				// Read from a memory address (device I/O too)
	addr & 0x80000000 ? stos(flash[addr & 0x7fffffff]):
	addr == 0x7fffffff ? stos(net_read()):		// As we have both memory mapped IO and multiple
	addr == 0x7ffffffe ? stos(0):			// distinct addressible memory regions, this 
	addr == 0x7ffffffd ? stos(0):			// function handles the read mapping for each
	addr == 0x7ffffffc ? stos(mouse_read()):	// address region.  Those devices which are 
	addr == 0x7ffffffb ? stos(key_read()):		// output only, will return 0 when read.
	addr == 0x7ffffffa ? stos(0):
	addr == 0x7ffffff9 ? stos(0):			// Reads from addresses below 0x1000 will fetch
	addr < 0x1000 ? stos(rom[addr]):		// from ROM, and not instruction memory which
	stos(ram[addr]);				// is considered write only!
}

// Write one byte from a memory addr
void mem_write(cell addr, cell value) {			// Write to a memory address (device I/O too)
	addr & 0x80000000 ? (flash[addr & 0x7fffffff] = value):
	addr == 0x7fffffff ? net_write(value):		// Similarly, writes to each of the address regions
	addr == 0x7ffffffe ? vid_write(value):		// require mapping from address to device or memory
	addr == 0x7ffffffd ? aud_write(value): 		// structure.  For those devices that are input only
	addr == 0x7ffffffc ? nop():			// this routine is effective an expensive nop()
	addr == 0x7ffffffb ? nop():
	addr == 0x7ffffffa ? nop():			// Writes to addresses below 0x1000 address 
	addr == 0x7ffffff9 ? nop():			// instruction memory and modify the executing code
        addr < 0x1000 ? im[addr] = value:		// no the code stored in ROM!  IM is write only
	(ram[addr] = value);				// and can be restored from ROM at any time.
}

void mem_move(int d) {				// Copy memory from one location to another
	utl &= 0xfffffff7;			// When we want to copy memory from one region to another
	source();		// this routine will safely write it to a I/O device or
	destination();		// copy it to the correct region. The direction flag
	if (ms && md) 				// indicates whether we are writing up or down.
		d < 0 ? memmove(md-cnt,ms-cnt,cnt*sizeof(cell)) : memmove(md,ms,cnt*sizeof(cell));	
	else if (!ms) 
		0x7fffffff == src ? device_read(net_read):		// For the devices a cell at a time
		0x7ffffffc == src ? device_read(mouse_read):		// is written to the device's address
		0x7ffffffb == src ? device_read(key_read) : nop();	// For reads, a cell at a time is
	else if (!md)							// pulled from that address.
		0x7fffffff == dst ? device_write(ms,net_write):
		0x7ffffffe == dst ? device_write(ms,vid_write):		// writing device data from a device
		0x7ffffffd == dst ? device_write(ms,aud_write) : nop();	// read is a bad idea and ill advised
	utl |= 0x08;
}

// NB: we can't compare device data!  Copy to a buffer first.  dst = im, src = rom
void mem_cmp() {					// Compare to regions of memory (no device I/O)
	utl &= 0xfffffff7;				// When two regions of memory need to be compared
	source();					// this routine will set the cnt register to 0
	destination();					// if the two regions are identical.
	if (ms && md) 					// If the regions are different the cnt register
		cnt = memcmp(ms,md,cnt*sizeof(cell));	// will continue to contain a non-zero value.
	utl |= 0x08;					// devices can not be compared in this fashion
}
////////////////////////////////////////////////////////////////////////////////
// end simulation
void end() {
	munmap(flash,flash_size);	// First we save the current flash image, and close the file
	close(flash_fd);		// handle, deconstruct the system resources, and then exit
	SDL_Quit();			// with a message describing the observed system performance
	fprintf(stderr,"Effective Speed: %dMHz @ %d samples\n",rate/1000,samples);
	exit(0);
}

////////////////////////////////////////////////////////////////////////////////
// interrupt simulation
void interrupt() {			// Simulate a device interrupt
	now = SDL_GetTicks();
	utl &= 0xfffffff0;
	if (SDL_PollEvent(&event)) switch(event.type) {
		case SDL_QUIT:
			end();
		case SDL_KEYDOWN:
			utl |= 1;
			key_buffer = 0x80 | keymap();
			if (event.key.keysym.sym == SDLK_ESCAPE) end();
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
		default: break;
	}
}

////////////////////////////////////////////////////////////////////////////////
// system clock simulation
void update() {					// Simulate attached devices
	++samples;				// update statistical sample count
	rate = (rate*samples + (24*(ticks - period)/1000))/samples; // avg ticks per frame
	period = ticks;				// reset the priod counter
	SDL_GL_SwapBuffers();			// update the video frame
	last = now;				// reset the frame refresh window
}

////////////////////////////////////////////////////////////////////////////////
// cpu simulation
void go() {					// Simulate Decoder & ALU
	cell instr;				// This cell holds the current instruction pointer
	int a, b;				// a and b are temporary variables
fetch:						// First update the psystem clock "tick". 
	++ticks;					// In order to mimic hardware updates, 
	if (!(ticks % INTERRUPT_RATE)) interrupt();	// we fire off periodic interrupts
	if (now - last >= REFRESH_RATE) update();	// and device updates, on the uptick
	if (!(ticks % NETWORK_RATE)) net_interrupt();	
	ip &= 0x0fff;				// Then we fetch the next instruction, keeping
	instr = im[ip++];			// the instruction pointer within the 0x1000 byte
	if (! (instr & 0x80000000)) { 		// instruction memory.  We read 1 cell at a time
		up(instr);			// which contains either 1 literal instruction or
		goto fetch;			// 4 opcode based instructions.  Literals kick us
	}					// to the next system clock, but all 4 instructions
next_op:					// are evaluated within a single system clock "tick"
	switch(instr & 0xff) {			// reading the LSB->MSB encoded opcodes we execute:
		case 0x80: goto next;					// nop
		case 0x81: upr(ip); ip = tos(); down(); goto fetch;	// call
		case 0x82: down(); goto next;				// drop
		case 0x83: snos(tos()); down(); goto next;		// nip
		case 0x84: upr(tos()); down(); goto next;		// push
		case 0x85: stos(~tos()); goto next;			// not
		case 0x86: snos(tos()&nos()); down(); goto next;	// and
		case 0x87: snos(tos()|nos()); down(); goto next;	// or
		case 0x88: snos(tos()^nos()); down(); goto next;	// xor
		case 0x89: mem_read(tos()); goto next;			// fetch
		case 0x8a: up(nos() < tos() ? -1 : 0); goto next;	// less
		case 0x8b: up(nos() == tos() ? -1 : 0); goto next;	// equal
		case 0x8c: stos(tos()<<1); goto next;			// shift left 
		case 0x8d: stos(tos()<<8); goto next;			// shift char left
		case 0x8e: up(0); goto next;				// zero
		case 0x8f: up(1); goto next;				// one
		case 0x90: ip = rtos(); downr(); goto fetch;		// jump
		case 0x91: if (!nos()) down(); down(); goto next; 	// conditional jump
				ip = tos(); down(); down(); goto fetch; // (nos ~= 0 -> jump)
		case 0x92: up(tos()); goto next;			// dup
		case 0x93: up(nos()); goto next;			// over
		case 0x94: up(rtos()); downr(); goto next;		// pop
		case 0x95: stos(-(int)tos()); goto next;		// neg
		case 0x96: snos(tos()+nos()); down(); goto next;	// add
		case 0x97: snos((int)tos()*(int)nos()); goto next; 	// muliply
		case 0x98: a = tos(); b = nos(); stos(a/b); snos(a%b); goto next;	// divide/modulus
		case 0x99: mem_write(tos(),nos()); down(); goto next;	// store
		case 0x9a: up(nos() > tos() ? -1 : 0); goto next;	// greater
		case 0x9b: up(nos() != tos() ? -1 : 0); goto next;	// unequal
		case 0x9c: stos(tos()>>1); goto next;			// shift right
		case 0x9d: stos(tos()>>8); goto next;			// shift char right
		case 0x9e: up(utl); goto next;				// utility register
		case 0x9f: up(-1); goto next;				// negative one
		case 0xa0: mem_move(-1); goto next;			// copy down
		case 0xa1: up(cnt); goto next;				// fetch count
		case 0xa2: up(src); goto next;				// fetch source		
		case 0xa3: up(dst); goto next;				// fetch destination
		case 0xc0: mem_cmp(); goto next;			// compare up
		case 0xc1: ++cnt; goto next;				// increment count
		case 0xc2: up(0); mem_read(src++); goto next;		// source read
		case 0xc3: mem_write(dst++,tos()); goto next;		// destination write
		case 0xe0: mem_move(1); goto next;			// copy up
		case 0xe1: cnt = tos(); goto next;			// store count
		case 0xe2: src = tos(); goto next;			// store source
		case 0xe3: dst = tos(); goto next;			// store destination
		default: break;					// ignore unkown opcodes
	}
next:						// When each opcode finishes, we advance instr to the 
	instr >>= 8;				// next one.  If we've shifted all 4 opcodes out we
	if (instr == 0) goto fetch;		// go on to fetch the next one, otherwise, we eval the
	goto next_op;				// next op stored in the LSB of instr
}

////////////////////////////////////////////////////////////////////////////////
// platform initialization
void init() {				// Initialize Platform Specific Application Settings
	ticks = 0;			// Here we clear the fake system clock and
	SDL_Init(SDL_INIT_EVERYTHING);	// tell SDL to setup video, audio, and devices
	video_init();			// assuming each of these work we will have a
	audio_init();			// fully functional environment.  Otherwise
	network_init();			// any of these routines may exit with error code
}

////////////////////////////////////////////////////////////////////////////////
// vm initialization
void reset() {					// Reset the VM, unmap flash if loaded
	ip = dsi = rsi = cnt = src = dst = utl = 0;
	ram = mmap(NULL,RAM_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
	if (ram == (cell*)0xffffffff) exit(NO_RAM);	// We use an anonymous block as our RAM
	if (flash) {				// the flash file is mapped to an actual
		munmap(flash,flash_size);	// file on the host system.  This mimics
		close(flash_fd);		// the NUMA architecture of the emulated
	}					// device.  Address ranges are handled
	flash = NULL;				// through translation.
}

////////////////////////////////////////////////////////////////////////////////
// boot simulation
void boot() {						// Load the flash image, and start VM
	struct stat st;					// As the flash image may be of different sizes
	flash_fd = open(flash_file,O_RDWR,0600);	// we query the host image for the 
	if (flash_fd < 0) exit(NO_FILE);		// actual size and attempt to map that 
	fstat(flash_fd,&st);				// image into memory.  Once the image is 
	flash_size = st.st_size;			// loaded, we copy the first 16kB from the
	flash = mmap(NULL,flash_size,PROT_READ|PROT_WRITE,MAP_FILE|MAP_SHARED,flash_fd,0);
	if (flash < 0) exit(NO_MAP);			// file into both our ROM buffer and our
	if (flash_size < ROM_SIZE) exit(NO_ROM);	// instruction memory buffer.
	memcpy(rom,flash,ROM_SIZE);			// This allows us to treat these as distinct
	memcpy(im,flash,ROM_SIZE);			// entities, and alterations to flash will not
}							// alter our ROMs at runtime.

////////////////////////////////////////////////////////////////////////////////
// entry point
int main (int argc, char** argv) {	//  Main Program Entry point
	if (argc != 2) {
		fprintf(stderr,"Usage: %s [file]\n",argv[0]);
		return 0;
	}
	flash_file = argv[1];		// The user must specify a flash memory image
	init();				// which we then boot to after initializing
	reset();			// our various system attached devices.  The
	boot();				// process of initializing and booting may
	go();				// exit prematurely.  But if it all works, we
	return 0;			// simply start executing instruction 0 in 
}					// the instruciton memory loaded from flash.

////////////////////////////////////////////////////////////////////////////////
// end
