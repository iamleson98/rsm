#include "rsm.h"

// Instructions are fixed-size at 32 bits long, little endian.
// PC and jump- & branch destinations are expressed in #instructions rather than bytes.
// There is room for 256 operations and 32+32 (int+float) registers (8 bit OP, 5 bit reg)
//
//        ┌─────────────────┬─────────┬─────────┬─────────┬───────────────┐
//  bit   │3 3 2 2 2 2 2 2 2│2 2 2 1 1│1 1 1 1 1│1 1 1    │               │
//        │1 0 9 8 7 6 5 4 3│2 1 0 9 8│7 6 5 4 3│2 1 0 9 8│7 6 5 4 3 2 1 0│
//        ├─────────────────┼─────────┼─────────┼─────────┼───────────────┤
//  ABCD  │          D (9)  │  C (5)  │  B (5)  │  A (5)  │     OP (8)    │
//        ├─────────────────┴─────────┼─────────┼─────────┼───────────────┤
//  ABCw  │                    C (14) │  B (5)  │  A (5)  │     OP (8)    │
//        ├───────────────────────────┴─────────┼─────────┼───────────────┤
//  ABw   │                              B (19) │  A (5)  │     OP (8)    │
//        ├─────────────────────────────────────┴─────────┼───────────────┤
//  Aw    │                                        A (24) │     OP (8)    │
//        └───────────────────────────────────────────────┴───────────────┘

// size and position of instruction arguments
#define SIZE_OP  8
#define SIZE_A   5
#define SIZE_B   5
#define SIZE_C   5
#define SIZE_D   9
#define SIZE_Cw  (SIZE_C + SIZE_D)
#define SIZE_Bw  (SIZE_B + SIZE_C + SIZE_D)
#define SIZE_Aw  (SIZE_A + SIZE_B + SIZE_C + SIZE_D)
#define POS_A    SIZE_OP
#define POS_B    (POS_A + SIZE_A)
#define POS_C    (POS_B + SIZE_B)
#define POS_D    (POS_C + SIZE_C)

// MASK1 creates a mask with n 1 bits at position p
#define MASK1(n,p)  ( ( ~( (~(rinstr)0) << (n) ) ) << (p) )

// MASK0 creates a mask with n 0 bits at position p
#define MASK0(n,p)  (~MASK1(n,p))

// int GET_ARGN(rinstr, uint pos, uint size)
// rinstr SET_ARGN(rinstr, uint pos, uint size, int val)
#define GET_ARGN(i,pos,size)  ((int)( ((i) >> (pos)) & MASK1(size,0) ))
#define SET_ARGN(i,pos,size,v) \
  ( (i) = ( ((i) & MASK0(size,pos)) | ( (((rinstr)v) << pos) & MASK1(size,pos)) ) )

// rop GET_OP(rinstr) ; rinstr SET_OP(rinstr, rop)
#define GET_OP(i)   ((rop)( (i) & MASK1(SIZE_OP,0) ))
#define SET_OP(i,o) ( (i) = (((i) & MASK0(SIZE_OP,0)) | (((rinstr)o) & MASK1(SIZE_OP,0))) )
// TODO: use {GET,SET}_ARGN for {GET,SET}_OP?

// int GET_{arg}(rinstr) ; rinstr SET_{arg}(rinstr, int val)
#define GET_A(i)    GET_ARGN(i, POS_A, SIZE_A)
#define SET_A(i,v)  SET_ARGN(i, POS_A, SIZE_A, v)
#define GET_Aw(i)   GET_ARGN(i, POS_A, SIZE_Aw)
#define SET_Aw(i,v) SET_ARGN(i, POS_A, SIZE_Aw, v)
#define GET_B(i)    GET_ARGN(i, POS_B, SIZE_B)
#define SET_B(i,v)  SET_ARGN(i, POS_B, SIZE_B, v)
#define GET_Bw(i)   GET_ARGN(i, POS_B, SIZE_Bw)
#define SET_Bw(i,v) SET_ARGN(i, POS_B, SIZE_Bw, v)
#define GET_C(i)    GET_ARGN(i, POS_C, SIZE_C)
#define SET_C(i,v)  SET_ARGN(i, POS_C, SIZE_C, v)
#define GET_Cw(i)   GET_ARGN(i, POS_C, SIZE_Cw)
#define SET_Cw(i,v) SET_ARGN(i, POS_C, SIZE_Cw, v)
#define GET_D(i)    GET_ARGN(i, POS_D, SIZE_D)
#define SET_D(i,v)  SET_ARGN(i, POS_D, SIZE_D, v)

#define MAKE_ABCD(op,a,b,c,d) ( ((rinstr)op) \
      | ( ((rinstr)a)  << POS_A ) \
      | ( ((rinstr)b)  << POS_B ) \
      | ( ((rinstr)c)  << POS_C ) \
      | ( ((rinstr)d)  << POS_D ) )
#define MAKE_ABC(op,a,b,cw) ( ((rinstr)op) \
      | ( ((rinstr)a)  << POS_A ) \
      | ( ((rinstr)b)  << POS_B ) \
      | ( ((rinstr)cw) << POS_C ) )
#define MAKE_AB(op,a,bw) ( ((rinstr)op) \
      | ( ((rinstr)a)  << POS_A  ) \
      | ( ((rinstr)bw) << POS_B ) )
#define MAKE_A(op,aw) ( ((rinstr)op) \
      | ( ((rinstr)aw) << POS_A ) )


int main(int argc, const char** argv) {
  rmem* m = rmem_makevm(4096*1000);

  rinstr* ip = rmem_allocz(m, sizeof(rinstr)*32);
  u32 pc = 0;
  // fun factorial (i32) i32
  //   b0:              //
  //     r8 = r0        // ACC = n (n is in r0, argument 0)
  //     r0 = 1         // RES (return value 0)
  //     ifeq r8 0 end  // if n==0 goto end
  //   b1:              // <- [b0] b1  ("[b]=implicit/fallthrough")
  //     r0 = mul r8 r0 // RES = ACC * RES
  //     r8 = sub r8 1  // ACC = ACC - 1
  //     ifne r8 0 b1   // if n!=0 goto b1
  //   end:             // <- b0 [b1]
  //     ret            // RES is at r0
  // u32 b0 = pc; // b0:
  ip[pc++] = MAKE_AB(rop_MOVE, 8, 0); // r8 = r0
  ip[pc++] = MAKE_AB(rop_LOADI, 0, 1); // r0 = 1
  ip[pc++] = MAKE_ABC(rop_BREQI, 8, 0, 3); // ifeq r8 0 end -- PC+3=end (TODO patch marker)
  u32 b1 = pc; // b1:
  ip[pc++] = MAKE_ABC(rop_MUL, 0, 0, 8); // r0 = mul r8 r0
  ip[pc++] = MAKE_ABC(rop_MUL, 8, 0, 8); // r8 = sub r8 1
  ip[pc] = MAKE_ABC(rop_BRNEI, 8, 0, pc-b1); // ifne r8 0 b1
  pc++;
  // u32 end = pc; // end:
  ip[pc++] = MAKE_A(rop_RET, 0); // ret

  ip = rmem_resize(m, ip, pc*sizeof(rinstr));
  dlog("function size: %lu B", pc*sizeof(rinstr));

  return 0;
}
