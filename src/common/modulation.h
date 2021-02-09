#ifndef MOD_COMMON
#define MOD_COMMON

#include <emmintrin.h>
#include <immintrin.h>

#include <cmath>
#include <iostream>

#include "buffer.inc"
#include "gettime.inc"
#include "memory_manage.h"
#include "symbols.h"

#define BPSK_LEVEL M_SQRT1_2
#define QPSK_LEVEL M_SQRT1_2

#define SCALE_BYTE_CONV_QPSK 20
#define SCALE_BYTE_CONV_QAM16 100
#define SCALE_BYTE_CONV_QAM64 100
#define QAM16_THRESHOLD 2 / sqrt(10)
#define QAM64_THRESHOLD_1 2 / sqrt(42)
#define QAM64_THRESHOLD_2 4 / sqrt(42)
#define QAM64_THRESHOLD_3 6 / sqrt(42)

void InitModulationTable(Table<complex_float>& table, size_t mod_order);
void InitQpskTable(Table<complex_float>& table);
void InitQam16Table(Table<complex_float>& table);
void InitQam64Table(Table<complex_float>& table);

complex_float ModSingle(int x, Table<complex_float>& mod_table);
complex_float ModSingleUint8(uint8_t x, Table<complex_float>& mod_table);
void ModSimd(uint8_t* in, complex_float*& out, size_t len,
             Table<complex_float>& mod_table);

void DemodQpskSoftSse(float* x, int8_t* z, int len);

void Demod16qamHardLoop(const float* vec_in, uint8_t* vec_out, int num);
void Demod16qamHardSse(float* vec_in, uint8_t* vec_out, int num);
void Demod16qamHardAvx2(float* vec_in, uint8_t* vec_out, int num);

void Demod16qamSoftLoop(const float* vec_in, int8_t* llr, int num);
void Demod16qamSoftSse(float* vec_in, int8_t* llr, int num);
void Demod16qamSoftAvx2(float* vec_in, int8_t* llr, int num);

void Demod64qamHardLoop(const float* vec_in, uint8_t* vec_out, int num);
void Demod64qamHardSse(float* vec_in, uint8_t* vec_out, int num);
void Demod64qamHardAvx2(float* vec_in, uint8_t* vec_out, int num);

void Demod64qamSoftLoop(const float* vec_in, int8_t* llr, int num);
void Demod64qamSoftSse(float* vec_in, int8_t* llr, int num);
void Demod64qamSoftAvx2(float* vec_in, int8_t* llr, int num);

void Print256Epi8(__m256i var);

#endif