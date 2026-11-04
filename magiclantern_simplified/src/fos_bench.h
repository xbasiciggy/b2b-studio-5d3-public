#ifndef FOS_BENCH_H
#define FOS_BENCH_H
#include <stdint.h>
unsigned fos_bench_alu(unsigned n);
unsigned fos_bench_copy(uint8_t *a, uint8_t *b, int size, int rounds);
unsigned fos_bench_stride(const uint8_t *a, int size, int rounds);
void fos_bench_fill(uint8_t *p, int size);
int fos_bench_gb_mem(void);                       /* bytes the Game Boy-shaped loop needs */
unsigned fos_bench_gb(uint8_t *mem, int steps);   /* mem filled with random bytes; returns a checksum */
#endif
