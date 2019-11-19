#ifndef _CYCLIC_SHIFT_H_
#define _CYCLIC_SHIFT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdexcept>
#include <math.h>
#include <immintrin.h> 

inline __m256i cycle_bit_shift_2to64(__m256i data, int16_t cyc_shift, int16_t zc);
inline __m256i cycle_bit_shift_72to128(__m256i data, int16_t cyc_shift, int16_t zc);
inline __m256i cycle_bit_shift_144to256(__m256i data, int16_t cyc_shift, int16_t zc);

typedef __m256i (* CYCLIC_BIT_SHIFT)(__m256i, int16_t, int16_t);
CYCLIC_BIT_SHIFT ldpc_select_shift_func(int16_t zcSize);

#endif