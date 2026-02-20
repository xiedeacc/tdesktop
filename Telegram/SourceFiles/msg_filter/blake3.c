/*
BLAKE3 reference implementation (C port by Jack O'Connor).
https://github.com/oconnor663/blake3_reference_impl_c
Public domain / CC0.
*/
#include <assert.h>
#include <string.h>

#include "msg_filter/blake3.h"

#define CHUNK_START (1 << 0)
#define CHUNK_END (1 << 1)
#define PARENT (1 << 2)
#define ROOT (1 << 3)
#define KEYED_HASH (1 << 4)
#define DERIVE_KEY_CONTEXT (1 << 5)
#define DERIVE_KEY_MATERIAL (1 << 6)

static uint32_t IV[8] = {
	0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
	0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
};

static size_t MSG_PERMUTATION[16] = {
	2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8
};

static uint32_t rotate_right(uint32_t x, int n) {
	return (x >> n) | (x << (32 - n));
}

static void g(uint32_t state[16], size_t a, size_t b, size_t c, size_t d,
		uint32_t mx, uint32_t my) {
	state[a] = state[a] + state[b] + mx;
	state[d] = rotate_right(state[d] ^ state[a], 16);
	state[c] = state[c] + state[d];
	state[b] = rotate_right(state[b] ^ state[c], 12);
	state[a] = state[a] + state[b] + my;
	state[d] = rotate_right(state[d] ^ state[a], 8);
	state[c] = state[c] + state[d];
	state[b] = rotate_right(state[b] ^ state[c], 7);
}

static void round_function(uint32_t state[16], uint32_t m[16]) {
	g(state, 0, 4, 8, 12, m[0], m[1]);
	g(state, 1, 5, 9, 13, m[2], m[3]);
	g(state, 2, 6, 10, 14, m[4], m[5]);
	g(state, 3, 7, 11, 15, m[6], m[7]);
	g(state, 0, 5, 10, 15, m[8], m[9]);
	g(state, 1, 6, 11, 12, m[10], m[11]);
	g(state, 2, 7, 8, 13, m[12], m[13]);
	g(state, 3, 4, 9, 14, m[14], m[15]);
}

static void permute(uint32_t m[16]) {
	uint32_t permuted[16];
	size_t i;
	for (i = 0; i < 16; i++) {
		permuted[i] = m[MSG_PERMUTATION[i]];
	}
	memcpy(m, permuted, sizeof(permuted));
}

static void compress(const uint32_t chaining_value[8],
		const uint32_t block_words[16], uint64_t counter,
		uint32_t block_len, uint32_t flags, uint32_t out[16]) {
	uint32_t state[16] = {
		chaining_value[0], chaining_value[1],
		chaining_value[2], chaining_value[3],
		chaining_value[4], chaining_value[5],
		chaining_value[6], chaining_value[7],
		IV[0], IV[1], IV[2], IV[3],
		(uint32_t)counter, (uint32_t)(counter >> 32),
		block_len, flags,
	};
	uint32_t block[16];
	memcpy(block, block_words, sizeof(block));
	round_function(state, block); permute(block);
	round_function(state, block); permute(block);
	round_function(state, block); permute(block);
	round_function(state, block); permute(block);
	round_function(state, block); permute(block);
	round_function(state, block); permute(block);
	round_function(state, block);
	size_t i;
	for (i = 0; i < 8; i++) {
		state[i] ^= state[i + 8];
		state[i + 8] ^= chaining_value[i];
	}
	memcpy(out, state, sizeof(state));
}

static void words_from_little_endian_bytes(const void *bytes,
		size_t bytes_len, uint32_t *out) {
	const uint8_t *u8 = (const uint8_t *)bytes;
	size_t i;
	for (i = 0; i < (bytes_len / 4); i++) {
		out[i] = ((uint32_t)(u8[i*4]))
			| (((uint32_t)(u8[i*4+1])) << 8)
			| (((uint32_t)(u8[i*4+2])) << 16)
			| (((uint32_t)(u8[i*4+3])) << 24);
	}
}

typedef struct b3_output {
	uint32_t input_cv[8];
	uint32_t block_words[16];
	uint64_t counter;
	uint32_t block_len;
	uint32_t flags;
} b3_output;

static void output_cv(const b3_output *self, uint32_t out[8]) {
	uint32_t out16[16];
	compress(self->input_cv, self->block_words, self->counter,
		self->block_len, self->flags, out16);
	memcpy(out, out16, 8 * 4);
}

static void output_root_bytes(const b3_output *self, void *out,
		size_t out_len) {
	uint8_t *out_u8 = (uint8_t *)out;
	uint64_t ctr = 0;
	while (out_len > 0) {
		uint32_t words[16];
		compress(self->input_cv, self->block_words, ctr,
			self->block_len, self->flags | ROOT, words);
		size_t word;
		for (word = 0; word < 16; word++) {
			int byte;
			for (byte = 0; byte < 4; byte++) {
				if (out_len == 0) return;
				*out_u8 = (uint8_t)(words[word] >> (8 * byte));
				out_u8++;
				out_len--;
			}
		}
		ctr++;
	}
}

static void cs_init(_blake3_chunk_state *self, const uint32_t key[8],
		uint64_t chunk_counter, uint32_t flags) {
	memcpy(self->chaining_value, key, sizeof(self->chaining_value));
	self->chunk_counter = chunk_counter;
	memset(self->block, 0, sizeof(self->block));
	self->block_len = 0;
	self->blocks_compressed = 0;
	self->flags = flags;
}

static size_t cs_len(const _blake3_chunk_state *self) {
	return BLAKE3_BLOCK_LEN * (size_t)self->blocks_compressed
		+ (size_t)self->block_len;
}

static uint32_t cs_start_flag(const _blake3_chunk_state *self) {
	return self->blocks_compressed == 0 ? CHUNK_START : 0;
}

static void cs_update(_blake3_chunk_state *self, const void *input,
		size_t input_len) {
	const uint8_t *in = (const uint8_t *)input;
	while (input_len > 0) {
		if (self->block_len == BLAKE3_BLOCK_LEN) {
			uint32_t bw[16];
			words_from_little_endian_bytes(self->block,
				BLAKE3_BLOCK_LEN, bw);
			uint32_t out16[16];
			compress(self->chaining_value, bw, self->chunk_counter,
				BLAKE3_BLOCK_LEN,
				self->flags | cs_start_flag(self), out16);
			memcpy(self->chaining_value, out16,
				sizeof(self->chaining_value));
			self->blocks_compressed++;
			memset(self->block, 0, sizeof(self->block));
			self->block_len = 0;
		}
		size_t want = BLAKE3_BLOCK_LEN - (size_t)self->block_len;
		size_t take = input_len < want ? input_len : want;
		memcpy(&self->block[(size_t)self->block_len], in, take);
		self->block_len += (uint8_t)take;
		in += take;
		input_len -= take;
	}
}

static b3_output cs_output(const _blake3_chunk_state *self) {
	b3_output ret;
	memcpy(ret.input_cv, self->chaining_value, sizeof(ret.input_cv));
	words_from_little_endian_bytes(self->block, sizeof(self->block),
		ret.block_words);
	ret.counter = self->chunk_counter;
	ret.block_len = (uint32_t)self->block_len;
	ret.flags = self->flags | cs_start_flag(self) | CHUNK_END;
	return ret;
}

static b3_output parent_output(const uint32_t left[8],
		const uint32_t right[8], const uint32_t key[8],
		uint32_t flags) {
	b3_output ret;
	memcpy(ret.input_cv, key, sizeof(ret.input_cv));
	memcpy(&ret.block_words[0], left, 8 * 4);
	memcpy(&ret.block_words[8], right, 8 * 4);
	ret.counter = 0;
	ret.block_len = BLAKE3_BLOCK_LEN;
	ret.flags = PARENT | flags;
	return ret;
}

static void parent_cv(const uint32_t left[8], const uint32_t right[8],
		const uint32_t key[8], uint32_t flags, uint32_t out[8]) {
	b3_output o = parent_output(left, right, key, flags);
	output_cv(&o, out);
}

void blake3_hasher_init(blake3_hasher *self) {
	cs_init(&self->chunk_state, IV, 0, 0);
	memcpy(self->key_words, IV, sizeof(self->key_words));
	self->cv_stack_len = 0;
	self->flags = 0;
}

static void push_stack(blake3_hasher *self, const uint32_t cv[8]) {
	memcpy(&self->cv_stack[(size_t)self->cv_stack_len * 8], cv, 8 * 4);
	self->cv_stack_len++;
}

static const uint32_t *pop_stack(blake3_hasher *self) {
	self->cv_stack_len--;
	return &self->cv_stack[(size_t)self->cv_stack_len * 8];
}

static void add_chunk_cv(blake3_hasher *self, uint32_t new_cv[8],
		uint64_t total_chunks) {
	while ((total_chunks & 1) == 0) {
		parent_cv(pop_stack(self), new_cv, self->key_words,
			self->flags, new_cv);
		total_chunks >>= 1;
	}
	push_stack(self, new_cv);
}

void blake3_hasher_update(blake3_hasher *self, const void *input,
		size_t input_len) {
	const uint8_t *in = (const uint8_t *)input;
	while (input_len > 0) {
		if (cs_len(&self->chunk_state) == BLAKE3_CHUNK_LEN) {
			b3_output co = cs_output(&self->chunk_state);
			uint32_t cv[8];
			output_cv(&co, cv);
			uint64_t total = self->chunk_state.chunk_counter + 1;
			add_chunk_cv(self, cv, total);
			cs_init(&self->chunk_state, self->key_words, total,
				self->flags);
		}
		size_t want = BLAKE3_CHUNK_LEN
			- cs_len(&self->chunk_state);
		size_t take = input_len < want ? input_len : want;
		cs_update(&self->chunk_state, in, take);
		in += take;
		input_len -= take;
	}
}

void blake3_hasher_finalize(const blake3_hasher *self, void *out,
		size_t out_len) {
	b3_output current = cs_output(&self->chunk_state);
	size_t remaining = (size_t)self->cv_stack_len;
	while (remaining > 0) {
		remaining--;
		uint32_t cv[8];
		output_cv(&current, cv);
		current = parent_output(
			&self->cv_stack[remaining * 8], cv,
			self->key_words, self->flags);
	}
	output_root_bytes(&current, out, out_len);
}
