#pragma once
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
typedef uint8_t mz_uint8;
typedef int16_t mz_int16;
typedef uint16_t mz_uint16;
typedef uint32_t mz_uint32;
typedef uint32_t mz_uint;
typedef int64_t mz_int64;
typedef uint64_t mz_uint64;
typedef int mz_bool;
#define MZ_FALSE 0
#define MZ_TRUE 1
#define MZ_ASSERT(x) assert(x)

/* Minimal forward declaration so d2dview.c compiles */
typedef struct mz_zip_archive mz_zip_archive;