// nsc.c
//
// A NewScript compiler
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

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>

#define OPCODES		44
#define IMAGE_SIZE	8388608
#define STRINGS_OFFSET	2097152
#define LEXICON_OFFSET	2017152

typedef unsigned int cell;

// This character maps is used for translation from ASCII to the Firth character set which is more logical
char char_map[] = "0123456789abcdefghijklmnopqrstuvwxyz,./;'[]\\`-= )!@#$%^&*(ABCDEFGHIJKLMNOPQRSTUVWXYZ<>?:\"{}|~_+\t\n";

struct {		// This table provides the system with a base list of opcodes for the VM
  cell opcode;		// We will later translate the ASCII strings to their Firth representations
  const char* name;	// This process occurs in the function init_strings();
} opcodes[] = {
	{ 0x80, "nop" },{ 0x81, "call" }, { 0x82, "," },  { 0x83, ";" }, 
	{ 0x84, ">r" }, { 0x85, "~" },    { 0x86, "&" },  { 0x87, "|" }, 
	{ 0x88, "\\" }, { 0x89, "@" },    { 0x8a, "<" },  { 0x8b, "=" }, 
	{ 0x8c, "<<" }, { 0x8d, "<<<" },  { 0x8e, "0" },  { 0x8f, "1" }, 
	{ 0x90, "." },  { 0x91, "?" },    { 0x92, ":" },  { 0x93, "^" }, 
	{ 0x94, "r>" }, { 0x95, "-" },    { 0x96, "+" },  { 0x97, "*" }, 
	{ 0x98, "/" },  { 0x99, "!" },    { 0x9a, ">" },  { 0x9b, "~=" }, 
	{ 0x9c, ">>" }, { 0x9d, ">>>" },  { 0x9e, "@u" }, { 0x9f, "-1" }, 
	{ 0xa0, "<-" }, { 0xa1, "@#" },   { 0xa2, "@$" }, { 0xa3, "@%" }, 
	{ 0xc0, "==" }, { 0xc1, "#" },    { 0xc2, "$" },  { 0xc3, "%" }, 
	{ 0xe0, "->" }, { 0xe1, "!#" },   { 0xe2, "!$" }, { 0xe3, "!%" }
};

struct {		// This structure stores the translated opcode names and the associated 
	cell key;	// instruction values.  This table is populated using the above data in
	cell value;	// init_strings();  It is used by opcode() to find opcodes.  This allows
} ops[OPCODES];		// us to bootstrap the system, without knowing about the Core object.

cell instr = 0;		// Pointer to currently compiling instruction cell
cell slot = 0;		// Current slot 0,1,2,3 within the instruction cell

cell* memory = NULL;			// Base address of the relocateable memory image, 0x80000000 in
cell memory_size = IMAGE_SIZE;		// the VM, the size of the image produced determined by the 
cell fd = -1;				// macros above.  The fd holds the OS filehandle on the image file.

cell lexicon_end = LEXICON_OFFSET;	// Top address in the lexicon, minimum string address
cell lexicon = LEXICON_OFFSET;		// This is the current address in the lexicon, grows down.

cell strings_end = STRINGS_OFFSET;	// Top address in the strings table, should be the top of the image
cell strings = STRINGS_OFFSET;		// current address in the strings table, supports 20k unique strings.

cell hex = 0;		// Flag indicating whether or not the first character in a word is #
cell number = 0;	// The current numeric value of word being parsed, we always calculate just in case
cell key = 0;		// This is the translated key value from the next character in stdin
cell input[4];		// A 4 cell structure which holds the current word being read in, it can contain
cell input_index = 0;	// no more than 16 characters total, hence input_index is between 0 and 3
cell input_slot = 0;	// similarly, input_slot is either 0,1,2,3 representing the byte at input[input_index]
cell line = 0;		// The line is a 4 mode, 0 = Object 1 = verb 2 = code 3 = comment line descriptor

cell object;	// current active object, this is used to look up method names
cell ident;	// identity of active element, holds the string pointer to the string we are looking for

cell keymap(int c) {	
	for (cell i = 0; i < sizeof(char_map); ++i)	// We walk through the character map and
		if (char_map[i] == c) return i;		// if we find a matching character return its index
	return 0x66;					// Otherwise we return 0x66, the unknown character
}

cell space() {
	if (key == 0x5f) ++line;					// increment line for each tab
	fprintf(stderr,"Key is %p Line is %d\n",key,line);
	if (key == 0x2f || key == 0x5f || key == 0x60) return -1;	// space, tab, newline
	return 0;							// all other characters
}

cell inkey() { return key = keymap(getchar()); }

cell word() {
	memset(input,0xff,4*sizeof(cell));	// Word parses the values from stdin, into separate words
	hex = number = 0;			// with a maximum length of 16, it also converts each word
						// into a numeric value, which may be used as a literal value
	input_index = input_slot = 0;		// if the word is not found in the lexicon or as an opcode
						// The input buffer is filled in 1 byte at a time until the
	while (0x66 != inkey()) {		// character 0x66 is encountered
		if (space()) return -1;				// A word is done if a space is encountered
		input[input_index] <<= 8;			// and stored in the String table in MSB order
		input[input_index] |= (0xff & key);		// the last character is stored in the LSB
		if (0x33 == key) hex = 1;			// and we always calculate the numeric value
		else number = (number * (hex ? 16 : 10)) + (0xff & key);
		++input_slot;					// # prefixes hexidecimal numbers, 
		if ((input_slot &= 3) == 0) ++input_index;	 
	}							// If we get the  unknown character
	return 0;						// from stdin, it means getchar() -> EOF
}

cell dump() {
	unsigned char* str = (unsigned char*)&memory[ident];
	for (cell i = 0; i < 16 && (*(str+i) != 0xff); ++i)
		fprintf(stderr,"%c",char_map[*(str+i)]);
}

cell string() {
	if (!input_index  && ! input_slot) return 0;	// If the string is empty return 0
	for (cell i = strings; i < strings_end; i+=4)	// This function looks up the input buffer's
		if (memory[i] == input[0] 		// value in the string table, and returns the
		&& memory[i+1] == input[1] 		// applicable index if found, otherwise
		&& memory[i+2] == input[2] 		// it will allocate 16bytes in the strings table
		&& memory[i+3] == input[3]) return i;	// and place a copy of the input buffer there
	strings -= 4;					// which means that each string in the source 
	memcpy(&memory[strings],input,sizeof(cell)*4);	// will appear only once in the image file
	return strings;					// We can use these strings for any purpose.
}

void byte(cell c) {
	memory[instr] |= ((c&0xff) << (8*slot));	// This function will compile one byte to the
	++slot;						// begining of instruction memory.  It will 
	if ((slot &= 3) == 0) ++instr;			// switch cells on slot overflow.
}

void pad() {
	while (slot) byte(0x80);		// Pad is used to align to a cell boundary, writes nops
}

cell begin() {		
	memory[--lexicon] = 0;	// with no tabs at the begining and a word starting with a capital letter
	memory[--lexicon] = ident;	// we save the value of that line for later writing to the lexicon
	object = lexicon;		// and initialize the current object to this object
	fprintf(stderr,"Compiling object ");
	dump();
	fprintf(stderr,"\n");
}

cell find() {						// Find uses the object names set by begin/end
	for (int i = lexicon; i < lexicon_end;) {	// to locate the current set of slots in which
		if (memory[i] == ident) return object = i;	// a method may be found.  Using an object's
		i += ((memory[i+1]<<1)+2);		// name switches which current method buffer is
	}						// queried at compile time.
	return 0;					// if no object is found, then 
}

void define() {			
	pad();						// To define a new method, we copy down the 
	lexicon -= 2;					// object's header, incrementing the method count
	memory[lexicon] = memory[lexicon+2];		// and then set the method name and address
	memory[lexicon+1] = memory[lexicon+3]+1;	// after padding to the next full cell address
	memory[lexicon+2] = ident;
	memory[lexicon+3] = instr;
	object = lexicon;				// Finally we reset the current object to here!
	fprintf(stderr,"Defining method ["); dump(); fprintf(stderr,"]\n");
}

cell method() {						// This finds a method in an object
	for (int i = 0; i < memory[object+1]; i += 2)	// We run 2 cells at a time, name=addr
		if (memory[object+i+2] == ident)	// If we find the current ident, return addr
			return memory[object+i+3];		
	return 0;					// Otherwise return 0
}

void function() {			// Compiles a function call
	pad();				// pad to current address so we can compile a literal
	memory[instr++] = method();	// compile the literal address of the method
	byte(0x81);			// write the call opcode
}

cell opcode() {
	for (int i = 0; i < OPCODES; ++i)		// For each opcode
		if (ops[i].key == ident) { 		// if the opcode's key == ident
			fprintf(stderr,"Compiling opcode %d\n",ops[i].value);
			return ops[i].value;		// return the opcode value
		}
	return 0;					// or 0 if not an opcode
}

void literal() {
	pad();						// Pad with nops to current cell boundary
	memory[instr++] = number & 0x80000000 ? 	// if the literal is negative
		-number & 0x7fffffff: 			// compile the positive value
		number & 0x7fffffff;			// otherwise compile the literal value
	if (number&0x80000000) byte(0x95);		// for negative number compile a negate opcode
}

void skip() { 
	while(0x60 != inkey()); 
	line = 0;
}

void unknown() {
	fprintf(stderr,"%d >> unknown word [",line); dump(); fprintf(stderr,"]\n");
	switch(line) {					// Unknown at start of the line are
		case 0: begin(); break;			// names of objects, while unknown
		case 1: define(); break;		// one tab in are verbs, otherwise
		case 2: literal(); break;		// we compile a number literal or
		default: skip(); break;			// 3 tabs skip comment to end of line
	}
} 

void nop() {}

void compile() {
	while(word()) {				// For each word in input
		if (line > 2) skip();		// Skip to end of line if we are in a comment
		ident = string();		// Find string identity
		if (ident == 0) continue;	// If it is an empty string continue
		fprintf(stderr,"found string ["); dump(); fprintf(stderr,"]\n");
		opcode() ? byte(opcode()): 	// If opcode compile it otherwise
		method() ? function():		// Else see if it is a method, and compile a function call
		find() ? nop() : unknown();	// Otherwise see if it is an object, or unknown
		if (key == 0x60) line = 0;	// reset line on newline
	}
}

void init_memory(const char* filename) {
	cell buffer[4096];
	memset(buffer,0,sizeof(buffer));
	fd = open(filename,O_CREAT|O_RDWR,0600);
	if (fd < 0) exit(1);
	fprintf(stderr,"Creating Image: %s\n", filename);
	for(int i = 0; i < memory_size/sizeof(buffer); ++i) 
		write(fd,&buffer,sizeof(buffer));
	fprintf(stderr,"Created Image: %s\n",filename);
	memory = mmap(NULL,memory_size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FILE,fd,0);
	if (memory < 0) exit(2);
}

void load_string(const char* str) {
	input_slot = input_index = 0;				// reset input buffer
	memset(input,0xffffffff,4*sizeof(cell));		// set all bits high
	for (int j = 0; str[j]; ++j) {				// for each character in the string
		input[input_index] <<= 8; 			// shift the input buffer
		input[input_index] |= (0xff & keymap(str[j]));  // translate the character
		++input_slot;					// then shift to the next slot
		if ((input_slot &= 3) == 0) ++input_index;	// and which input cell on overflow
	}
}

void init_strings() {
	for (int i = 0; i < OPCODES; ++i)  {		// For each opcode
		load_string(opcodes[i].name);		// copy the string into the input buffer
		ops[i].key = string();			// Then initialize the ops table
		ops[i].value = opcodes[i].opcode;	// with the correct targe values
	}
}

void init_lexicon() {
	for (int i = 0; i < OPCODES; ++i) {		// For each opcode we compile a simple definition
		memory[--lexicon] = ops[i].value;	// which will form the basis for the Core Object
		memory[--lexicon] = ops[i].key;		// In the native compiler, we'll use these as lits
	}
	load_string("Core");				// We can load the string "Core" for the core
	memory[--lexicon] = OPCODES;			// Object, but we never use this with find
	memory[--lexicon] = string(); 			// it is just a tool for debugging
}

void fini_memory() {
	munmap(memory,memory_size);	// save changes to the target image	
	close(fd);			// and release the file
}

int main(int argc, char** argv) {
	if (argc != 2) {		// Ensure that the user supplies a image filename
		fprintf(stderr,"Usage: %s image.nsi\n",argv[0]);
		return 1;		// Return error and usage message if no file supplied
	}
	init_memory(argv[1]);		// create a new memory image of the suppied filename
	init_strings();			// setup of core system strings, and opcode tables
	init_lexicon();			// define a basic lexicon
	compile();			// compile the source listings from stdin
	fini_memory();			// save the memory image and close the file
	return 0;			// return success
}
