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
	cell value;	// init_strings();  It is used by lookup() to find opcodes.  This allows
} ops[OPCODES];		// us to bootstrap the system, without knowing about the Core object.

cell instr = 0;		// Pointer to currently compiling instruction cell
cell slot = 0;		// Current slot 0,1,2,3 within the instruction cell

cell* memory = NULL;			// Base address of the relocateable memory image, 0x80000000 in
cell memory_size = IMAGE_SIZE;		// the VM, the size of the image produced determined by the 
cell fd;				// macros above.  The fd holds the OS filehandle on the image file.

cell lexicon_end = LEXICON_OFFSET;	// Top address in the lexicon, minimum string address
cell lexicon = LEXICON_OFFSET;		// This is the current address in the lexicon, grows down.

cell strings_end = STRINGS_OFFSET;	// Top address in the strings table, should be the top of the image
cell strings = STRINGS_OFFSET;		// current address in the strings table, supports 20k unique strings.

cell hex;		// Flag indicating whether or not the first character in a word is #
cell number;		// The current numeric value of word being parsed, we always calculate just in case
cell key;		// This is the translated key value from the next character in stdin
cell input[4];		// A 4 cell structure which holds the current word being read in, it can contain
cell input_index;	// no more than 16 characters total, hence input_index is between 0 and 3
cell input_slot;	// similarly, input_slot is either 0,1,2,3 representing the byte at input[input_index]

cell keymap(int c) {	
	for (cell i = 0; i < sizeof(char_map); ++i)	// We walk through the character map and
		if (char_map[i] == c) return i;		// if we find a matching character return its index
	return 0x66;					// Otherwise we return 0x66, the unknown character
}

cell space() {
	if (key == 0x2f || key == 0x5f || key == 0x60) return -1;	// space, tab, new line
	return 0;							// all other characters
}

cell word() {
	memset(input,0xff,sizeof(cell));	// Word parses the values from stdin, into separate words
	number = 0;				// with a maximum length of 16, it also converts each word
	hex = 0;				// into a numeric value, which may be used as a literal value
	input_slot = 0;				// if the word is not found in the lexicon or as an opcode
	input_index = 0;			// The input buffer is filled in 1 byte at a time
	while (0x66 != (key = keymap(getchar()))) {	// until the unknown character 0x66 is encountered
		if (space()) return -1;		// If a space is encountered, then the word is complete.
		input[input_index] <<= 8;	// Characters are stored in the String table in MSB order
		input[input_index] |= (0xff & key);	// with the most recent character stored in the LSB
		if (0x33 == key) hex = 1;	// Since we are walking the string already we calculate
		else number = (number * (hex ? 16 : 10)) + (0xff & key); // the numeric value, using # to 
		++input_slot;			// prefix hexidecimal numbers, it is easier to do this now
		if ((input_slot &= 3) == 0) ++input_index;	// than to have to walk the buffer later
	}					// When we only get the unknown character, we are done
	return 0;				// with stdin, which means getchar() -> EOF
}

cell string() {
	for (cell i = strings; i < strings_end; i+=4)	// This function looks up the input buffer's
		if (memory[i] == input[0] 		// value in the string table, and returns the
		&& memory[i+1] == input[1] 		// applicable index if found, otherwise
		&& memory[i+2] == input[2] 		// it will allocate 16bytes in the strings table
		&& memory[i+3] == input[3]) return i;	// and place a copy of the input buffer there
	strings -= 4;					// which means that each string in the source 
	memcpy(&memory[strings],input,sizeof(cell)*4);	// will appear only once in the image file
	return strings;					// We can use these strings for any purpose.
}

void  byte(cell c) {
	memory[instr] |= ((c&0xff) << (8*slot));	// This function will compile one byte to the
	++slot;						// begining of instruction memory.  It will 
	if ((slot &= 3) == 0) ++instr;			// switch cells on slot overflow.
}

void pad() {
	while (slot) byte(0x80);		// Pad is used to align to a cell boundary, writes nops
}

cell object;	// current active object, this is used to look up method names
cell ident;	// identity of active element, holds the string pointer to the string we are looking for
cell name;	// name of current object, holds the string pointer for the name of the current object
cell methods;	// number of methods in current object, used to speed object lookup in the lexicon

cell begin() {		// We call begin to start a new object, this is represented in code by a line
	methods = 0;	// with no whitespace at the begining and a word starting with a capital letter
	name = ident;	// we save the value of that line for later writing to the lexicon
}

cell end() {				// End is called as a counterpart to begin, and it finalizes
	memory[--lexicon] = methods;	// the current object's definition.  Once end is called the 
	memory[--lexicon] = name;	// object may not be modified, and the number of methods is set.
}

cell find() {						// Find uses the object names set by begin/end
	for (int i = lexicon; i != lexicon_end;) {	// to locate the current set of slots in which
		if (memory[i] == ident) return object = i; // a method may be found.  Using an object's
		i += (memory[i+1]<<1);			// name switches which current method buffer is
	}						// queried at compile time.
	return 0;					// if no object is found, then 
}

cell current() {

}

cell define(cell c) {

}

cell method() {

}

void function() {


}

cell lookup() {
	for (int i = 0; i < OPCODES; ++i)		// For each opcode
		if (ops[i].key == ident) 		// if the opcode's key == ident
			return ops[i].value;		// return the opcode value
	return 0;					// or 0 if not an opcode
}

void literal() {
	pad();						// Pad with nops to current cell boundary
	memory[instr++] = number & 0x80000000 ? 	// if the literal is negative
		-number & 0x7fffffff: 			// compile the positive value
		number & 0x7fffffff;			// otherwise compile the literal value
	if (number&0x80000000) byte(0x95);		// for negative number compile a negate opcode
}

void compile() {
	while(word()) {			// For each word in input
		ident = string();	// Find string identity
		cell opc = lookup();	// Then lookup opcode
		opc ? byte(opc): 	// If opcode compile it otherwise
		find() ? current():	// Find it in the lexicon, and set the current object
		method() ? function():	// else look to see if it is a method, and compile a function call
		literal();		// Otherwise compile literal value
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

void init_strings() {
	for (int i = 0; i < OPCODES; ++i)  {				// For each opcode
		input_slot = input_index = 0;				// reset input buffer
		memset(input,0xffffffff,4*sizeof(cell));		// set all bits high
		for (int j = 0; opcodes[i].name[j]; ++j) {		// and for each character in the name
			input[input_index] <<= 8; 			// shift the input buffer
			input[input_index] |= (0xff & keymap(opcodes[i].name[j])); // translate the character
			++input_slot;					// then shift to the next slot
			if ((input_slot &= 3) == 0) ++input_index;	// and which input cell on overflow
		}
		ops[i].key = string();					// Then initialize the ops table
		ops[i].value = opcodes[i].opcode;			// with the correct targe values
	}
}

void init_lexicon() {

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