/* Virtual Machine with I/O, persistence and indexing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int debug = 0;

struct PU {
	unsigned pc; 	// counter
	unsigned fr; 	// flags register
	unsigned r[8];  // registers

	unsigned char *pmem; 		 // points to RAM
	unsigned pmemsize;	 // RAM size in bytes
	void (*op[32])(struct PU *pcore); // execution functions
};

enum        Ops         { SUS,   MOV ,  ADD ,  SUB ,  JIF ,  JMR,   MPC,   IN,   OUT ,  PUT ,  ATP };
const char *ops_str[] = {"SUS", "MOV", "ADD", "SUB", "JIF", "JMR", "MPC", "IN", "OUT", "PUT", "ATP"};

#define FL_ZERO		0x1
#define FL_CARRY	0x2

#define isflag(pu, f)		(((pu)->fr) & (f))
#define setflag(pu, e, f)	((e) ? ((pu)->fr |= (f)) : ((pu)->fr &= ~(f)))

#define setzf(pu, e)	setflag(pu, e, FL_ZERO)
#define setcf(pu, e)	setflag(pu, e, FL_CARRY)

#define num_(n)		((n)*2 + 1)
#define addr_(p)	((p)*2)
#define reg_(p)		addr_(p)
#define tonum(e)	((e)>>1)
#define isnum(e)	((e)&1)

// convenience macros
#define R0		reg_(0)
#define R1		reg_(1)
#define R2		reg_(2)
#define R3		reg_(3)
#define R4		reg_(4)
#define R5		reg_(5)
#define R6		reg_(6)
#define R7		reg_(7)
#define N0		num_(0)
#define N1		num_(1)

// an rval can be a number, memory/register index
// an lval can only be a memory/register index
static unsigned obj_read(struct PU *pcore, unsigned rval)
{
	if (isnum(rval))
		return tonum(rval);
	rval /= 2;
	return rval < 16 ? pcore->r[rval] : pcore->pmem[rval];
}

static void obj_write(struct PU *pcore, unsigned lval, unsigned rval)
{
	assert(!isnum(lval));
	lval /= 2;
	if (lval < 16)
		pcore->r[lval] = obj_read(pcore, rval);
	else
		pcore->pmem[lval] = obj_read(pcore, rval);
}

#define IO_CHR	0	// character
#define IO_NUM  1	// numbers
#define IO_STR  2	// strings

static unsigned getinp(struct PU *pcore, unsigned ch)
{
	int n = 0;

	if (ch == IO_CHR)
		return getchar();
	else if (ch == IO_NUM) {
		//if (isatty(0))// putchar('?');//is atty:file descriptor referring to a terminal
		scanf("%d", &n);
		return n;
	}
	return 0;
}

static void putout(struct PU *pcore, unsigned ch, unsigned n)
{
	if (ch == IO_CHR)
		putchar(n);
	else if (ch == IO_NUM)
		printf("%d\n", n);
	return;
}

static void op_SUS(struct PU *pcore)
{
}

static void op_MOV(struct PU *pcore)
{
	// MOV rval, lval
	unsigned rval = pcore->pmem[pcore->pc++];
	unsigned lval = pcore->pmem[pcore->pc++];

	obj_write(pcore, lval, rval);
}

static void op_PUT(struct PU *pcore)//indirect addressing
{
	// PUT rval, [lval]
	unsigned rval = pcore->pmem[pcore->pc++];
	unsigned ixreg = pcore->pmem[pcore->pc++];
	unsigned lval;

	assert(!isnum(ixreg));
	lval = obj_read(pcore, ixreg);//read the contents of register and use that as an index
	obj_write(pcore, addr_(lval), rval);//have to encode as an address
}

static void op_ATP(struct PU *pcore)//indirect address is the src
{
	// ATP [lval], lval2
	unsigned ixreg = pcore->pmem[pcore->pc++];
	unsigned lval2 = pcore->pmem[pcore->pc++];
	unsigned lval;

	assert(!isnum(ixreg));//ensure that it is not a number
	lval = obj_read(pcore, ixreg);//read the first operand which is the address
	obj_write(pcore, lval2, addr_(lval));//encode it as an address
}

static void op_ADD(struct PU *pcore)
{
	// ADD expr addr
	unsigned expr = pcore->pmem[pcore->pc++];
	unsigned addr = pcore->pmem[pcore->pc++];
	unsigned old, new;

	old = obj_read(pcore, addr);
	new = old + obj_read(pcore, expr);
	setcf(pcore, (new < old));
	setzf(pcore, (new == 0));
	obj_write(pcore, addr, num_(new));
}

static void op_SUB(struct PU *pcore)
{
	// SUB expr addr
	unsigned expr = pcore->pmem[pcore->pc++];
	unsigned addr = pcore->pmem[pcore->pc++];
	unsigned old, new;

	old = obj_read(pcore, addr);
	new = old - obj_read(pcore, expr);
	setcf(pcore, (new > old));
	setzf(pcore, (new == 0));
	obj_write(pcore, addr, num_(new));
}

static void op_JIF(struct PU *pcore)
{
	// JIF flag, addr - set PC to addr if flag is false
	unsigned flag = pcore->pmem[pcore->pc++];
	unsigned addr = pcore->pmem[pcore->pc++];

	if (!isflag(pcore, flag))
		pcore->pc = obj_read(pcore, addr);
}

static void op_JMR(struct PU *pcore)
{
	// JMR reg - jump to address in a register
	unsigned addr = pcore->pmem[pcore->pc++];

	pcore->pc = obj_read(pcore, addr);
}

static void op_MPC(struct PU *pcore)
{
	// MPC reg - save pc contents in a register
	unsigned addr = pcore->pmem[pcore->pc++];

	obj_write(pcore, addr, num_(pcore->pc));
}

static void op_IN(struct PU *pcore)
{
	// IN ch, addr
	unsigned ch = pcore->pmem[pcore->pc++];
	unsigned addr = pcore->pmem[pcore->pc++];
	unsigned in = getinp(pcore, ch);//returns character or number depending on ch

	obj_write(pcore, addr, num_(in));
}

static void op_OUT(struct PU *pcore)
{
	// OUT ch, addr
	unsigned ch = pcore->pmem[pcore->pc++];
	unsigned addr = pcore->pmem[pcore->pc++];

	putout(pcore, ch, obj_read(pcore, addr));//write to screen the contents of the address as a character or number depending on ch
}

struct PU Core1 = {
	.op[SUS] = op_SUS,
	.op[MOV] = op_MOV,
	.op[ADD] = op_ADD,
	.op[SUB] = op_SUB,
	.op[JIF] = op_JIF,
	.op[JMR] = op_JMR,
	.op[MPC] = op_MPC,
	.op[IN]  = op_IN,
	.op[OUT] = op_OUT,
	.op[PUT] = op_PUT,
	.op[ATP] = op_ATP,
};

#define MEM_SIZE	128 // cannot exceed 128

#define NVARS		4
#define VARS		(MEM_SIZE-NVARS)

#if (MEM_SIZE > 128)
#error "mem size too large"
#endif

unsigned char MEM[MEM_SIZE] = {
	IN,IO_NUM,R6,//read start addr into R6
	MOV,R6,addr_(127),//store start address into addr 127

	IN,IO_NUM,R2,//read no of bytes into R2

	//loop to start reading each number
	MPC,R3,
	IN,IO_NUM,R1,//read one byte
	PUT,R1,R6,//save at start address
	ADD,N1,R6,//increment the pointer to program memory
	SUB,N1,R2,//keep looping till all bytes are read
	JIF,FL_ZERO,R3,

	JMR, addr_(127),//jump to start address to begin executing the instructions that were read

[VARS] = 0, 0, 0, 

};

#define array_size(a)	(sizeof(a)/sizeof(a[0]))

extern void dump_state(struct PU *);

static void resume_core(struct PU *pcore)
{
	unsigned char icode;

	dump_state(pcore);
	do {
		icode = pcore->pmem[pcore->pc++];
		if (pcore->op[icode])
			(pcore->op[icode])(pcore);
		else
			break;
		dump_state(pcore);
	} while (icode != SUS);
}

struct imageheader {
	unsigned magic;
	unsigned size;		// size of image excluding header
	unsigned memsize;	// pool memory size
	unsigned pc;		// PC status saved here
};

#define MAGIC 	0x2017

static void save_image(struct PU *pcore, char *filename)
{
	int f;
	struct imageheader h;
	int n, len;

	if (!filename) return;

	if (debug) printf("saving image into new file %s\n", filename);
	if ((f = open(filename, O_RDWR|O_CREAT|O_EXCL,S_IRWXU)) < 0) {
		perror(filename);
		return;
	}
	h.magic = MAGIC;
	h.pc = pcore->pc;
	h.memsize = pcore->pmemsize;
	h.size = sizeof(h)+h.memsize;
	n = write(f, &h, sizeof(h));
	for (len = h.memsize; len > 0; len -= n) {
		n = write(f, pcore->pmem, h.memsize);
		if (n <= 0) {
			perror("write");
			break;
		}
	}
	close(f);
}

static int load_image(struct PU *pcore, char *filename)
{
	int f;
	struct imageheader h;
	int n, len;

	if (!filename) {
		// use built-in bootstrap
		pcore->pc = 0;
		pcore->pmem = MEM;
		pcore->pmemsize = array_size(MEM);
		return 0;
	}
	if (debug) printf("loading image from %s\n", filename);
	if ((f = open(filename, O_RDONLY)) < 0) {
		perror(filename);
		return -1;
	}
	if (read(f, &h, sizeof(h)) < sizeof(h)) {
		close(f);
		perror(filename);
		return -1;
	}
	if (h.magic != MAGIC) {
		printf("error: bad magic %x\n", h.magic);
		close(f);
		return -1;
	}
	assert(0 < h.memsize);
	if (h.memsize != MEM_SIZE) {
		printf("error: memory size mismatch %d\n", h.memsize);
		close(f);
		return -1;
	}
	if ((pcore->pmem = malloc(h.memsize)) == 0) {
		perror("no memory");
		close(f);
		return -1;
	}
	pcore->pmemsize = h.memsize;
	pcore->pc = h.pc;
	if (h.pc >= pcore->pmemsize) {
		printf("error: pc %d beyond memory %d\n", h.pc, h.memsize);
		close(f);
		return -1;
	}
	for (len = pcore->pmemsize; len > 0 ; len -= n) {
		n = read(f, pcore->pmem, len);
		if (n <= 0) {
			close(f);
			perror(filename);
			return -1;
		}
	}
	close(f);
	return 0;
}

int main(int argc, char *argv[])
{
	char *arg;
	char *loadfile = 0;
	char *savefile = 0;
	/* simple board with just one PU and one MEM */
	
	while (--argc > 0) {
		//printf("%d%s\n",argc,argv[1]);
		arg = *++argv;
		if (!strcmp(arg, "-s")) {
			--argc;
			savefile = *++argv;
		}
		else if (!strcmp(arg, "-l")) {
			--argc;
			loadfile = *++argv;
		}
		else if (!strcmp(arg, "-d")) {
			debug = 1;
		}
		else
			loadfile = arg;
	}
	if (load_image(&Core1, loadfile) < 0) {
		exit(1);
	}
	
	if (debug) printf("resuming core from %d\n", Core1.pc);
	resume_core(&Core1);
	save_image(&Core1, savefile);
	return 0;
}


void dump_state(struct PU *pcore)
{
	int i;

	if (debug == 0) return;

	printf(" %2s %2s %4s", "PC", "FR", "INST");
	for (i = 0; i < array_size(pcore->r); i++) printf(" R%d", i);
	putchar('\n');
	printf(" %02hhx %02hhx %4s", pcore->pc, pcore->fr, ops_str[pcore->pmem[pcore->pc]]);
	for (i = 0; i < array_size(pcore->r); i++) printf(" %02hhx", pcore->r[i]);
	printf("\n------------------------- MEM ------------------------\n");
	for(i = 0; i < array_size(MEM);) {
		printf("%04x  ", i);
		for (int col = 0; col < 16; col++, i++) {
			if (col == 8) putchar(' ');
			printf("%02hhx%c", pcore->pmem[i], i == pcore->pc ? '<': ' ');
		}
		putchar('\n');
	}
	if (isatty(0)) {
		printf("Press enter to continue...\n"); getchar();
	}
}
