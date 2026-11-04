/* Small benchmarks for the on-camera self-test (System > Self-test): how fast the camera's processor runs integer code, copies
 * memory, and runs a dispatch loop shaped like a Game Boy CPU emulator. Portable C so the host can check they run. */
#include "fos.h"
#include "fos_bench.h"
#ifndef CONFIG_EOSM      /* left out of the EOS M build (its boot size limit): only the self-test uses these */

unsigned fos_bench_alu(unsigned n){
 unsigned x=2463534242u,s=0;
 for(unsigned i=0;i<n;i++){x^=x<<13;x^=x>>17;x^=x<<5;s+=x&0xFF;}
 return s;
}
unsigned fos_bench_copy(uint8_t *a,uint8_t *b,int size,int rounds){
 unsigned s=0;
 for(int r=0;r<rounds;r++){memcpy(b,a,size);a[r&255]^=b[(r*97)&255];s+=b[size-1];}
 return s;
}
unsigned fos_bench_stride(const uint8_t *a,int size,int rounds){
 unsigned s=0;
 for(int r=0;r<rounds;r++)for(int i=0;i<size;i+=61)s+=a[i];
 return s;
}
void fos_bench_fill(uint8_t *p,int size){
 unsigned v=12345u;
 for(int i=0;i<size;i++){v=v*1664525u+1013904223u;p[i]=(uint8_t)(v>>24);}
}

/* A stand-in for a Game Boy CPU core: 8 registers, a memory map decoded from the top nibble of the address, loads, 8-bit
 * adds with flags, increments, relative jumps, push and pop. Real emulators also draw the screen and run timers, so what
 * matters is how many of these steps per second the camera manages compared with the Game Boy's ~1 million per second. */
#define GB_ROM 0x8000
#define GB_VRAM 0x2000
#define GB_SRAM 0x2000
#define GB_WRAM 0x2000
#define GB_HRAM 0x200
int fos_bench_gb_mem(void){return GB_ROM+GB_VRAM+GB_SRAM+GB_WRAM+GB_HRAM;}
unsigned fos_bench_gb(uint8_t *mem,int steps){
 uint8_t *rom=mem,*vram=rom+GB_ROM,*sram=vram+GB_VRAM,*wram=sram+GB_SRAM,*hram=wram+GB_WRAM;
 uint8_t r[8]={1,2,3,4,5,6,7,8};            /* B C D E H L (HL) A, as indices 0..7 */
 unsigned pc=0x100,sp=0xFFFE,f=0,ticks=0;
 for(int i=0;i<steps;i++){
  uint8_t op=rom[pc++&0x7FFF];
  switch(op>>6){
   case 1:{                                  /* LD r,r' (and LD r,(HL) / LD (HL),r) */
    int d=(op>>3)&7,s=op&7;unsigned hl=(unsigned)r[4]<<8|r[5];uint8_t v;
    if(s==6){switch(hl>>12){case 0:case 1:case 2:case 3:case 4:case 5:case 6:case 7:v=rom[hl&0x7FFF];break;case 8:case 9:v=vram[hl&0x1FFF];break;
      case 10:case 11:v=sram[hl&0x1FFF];break;case 12:case 13:v=wram[hl&0x1FFF];break;default:v=hram[hl&0x1FF];}}
    else v=r[s];
    if(d==6){switch(hl>>12){case 8:case 9:vram[hl&0x1FFF]=v;break;case 10:case 11:sram[hl&0x1FFF]=v;break;case 12:case 13:wram[hl&0x1FFF]=v;break;case 14:case 15:hram[hl&0x1FF]=v;break;default:break;}}
    else r[d]=v;
    ticks+=4;break;}
   case 2:{                                  /* ADD/SUB/AND/XOR A,r with flags */
    int s=op&7;unsigned a=r[7],v=s==6?rom[((unsigned)r[4]<<8|r[5])&0x7FFF]:r[s],res;
    switch((op>>3)&3){
     case 0:res=a+v;f=(res&0xFF?0:0x80)|(((a&15)+(v&15))>15?0x20:0)|(res>255?0x10:0);break;
     case 1:res=a-v;f=0x40|(res&0xFF?0:0x80)|((a&15)<(v&15)?0x20:0)|(a<v?0x10:0);break;
     case 2:res=a&v;f=(res?0:0x80)|0x20;break;
     default:res=a^v;f=res?0:0x80;break;}
    r[7]=(uint8_t)res;ticks+=4;break;}
   case 3:{
    switch(op&7){
     case 1:{unsigned lo=wram[sp++&0x1FFF],hi=wram[sp++&0x1FFF];r[(op>>4)&1?2:0]=(uint8_t)hi;r[(op>>4)&1?3:1]=(uint8_t)lo;ticks+=12;break;}   /* POP */
     case 5:{wram[--sp&0x1FFF]=r[(op>>4)&1?2:0];wram[--sp&0x1FFF]=r[(op>>4)&1?3:1];ticks+=16;break;}                                            /* PUSH */
     case 2:{if(!(f&0x80))pc+=(int8_t)rom[pc&0x7FFF];pc++;ticks+=8;break;}                                                                     /* JR NZ */
     default:r[7]^=(uint8_t)(op+i);ticks+=4;break;}
    break;}
   default:{                                 /* INC/DEC r, LD r,n */
    int d=(op>>3)&7;
    if((op&7)==4){r[d]=(d==6?r[7]:r[d])+1;f=(f&0x10)|(r[d]?0:0x80);}
    else if((op&7)==5){r[d]=(d==6?r[7]:r[d])-1;f=(f&0x10)|0x40|(r[d]?0:0x80);}
    else if((op&7)==6){r[d==6?7:d]=rom[pc++&0x7FFF];}
    ticks+=4;break;}
  }
 }
 return ticks+r[0]+r[1]+r[2]+r[3]+r[7]+f+pc+sp;
}

#endif
