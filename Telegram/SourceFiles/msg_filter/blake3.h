/*
BLAKE3 reference implementation (C port by Jack O'Connor).
https://github.com/oconnor663/blake3_reference_impl_c
Public domain / CC0.
*/
#pragma once

#include <stddef.h>
#include <stdint.h>

#define BLAKE3_OUT_LEN 32
#define BLAKE3_KEY_LEN 32
#define BLAKE3_BLOCK_LEN 64
#define BLAKE3_CHUNK_LEN 1024

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _blake3_chunk_state {
	uint32_t chaining_value[8];
	uint64_t chunk_counter;
	uint8_t block[BLAKE3_BLOCK_LEN];
	uint8_t block_len;
	uint8_t blocks_compressed;
	uint32_t flags;
} _blake3_chunk_state;

typedef struct blake3_hasher {
	_blake3_chunk_state chunk_state;
	uint32_t key_words[8];
	uint32_t cv_stack[8 * 54];
	uint8_t cv_stack_len;
	uint32_t flags;
} blake3_hasher;

void blake3_hasher_init(blake3_hasher *self);
void blake3_hasher_update(blake3_hasher *self, const void *input,
	size_t input_len);
void blake3_hasher_finalize(const blake3_hasher *self, void *out,
	size_t out_len);

#ifdef __cplusplus
}
#endif
