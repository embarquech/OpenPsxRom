// Minimal subset of PSn00bSDK psxpress.h (MPL-2.0, (C) 2022-2023 spicyjpeg) for the STR player
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Structure definitions */

typedef struct {
	uint8_t iq_y[64];	// Luma quantization table, stored in zigzag order
	uint8_t iq_c[64];	// Chroma quantization table, stored in zigzag order
	int16_t dct[64];	// Inverse DCT matrix (2.14 fixed-point)
} DECDCTENV;

typedef struct {
	uint16_t ac0[2];
	uint32_t ac2[8], ac3[64];
	uint16_t ac4[8], ac5[8], ac7[16], ac8[32];
	uint16_t ac9[32], ac10[32], ac11[32], ac12[32];
} VLC_TableV2;

typedef struct {
	uint16_t ac0[2];
	uint32_t ac2[8], ac3[64];
	uint16_t ac4[8], ac5[8], ac7[16], ac8[32];
	uint16_t ac9[32], ac10[32], ac11[32], ac12[32];
	uint8_t  dc[128], dc_len[9];
	uint8_t  _reserved[3];
} VLC_TableV3;

typedef struct {
	uint32_t ac[8192], ac00[512];
} DECDCTTAB;

typedef enum {
	DECDCT_MODE_24BPP		= 1,
	DECDCT_MODE_16BPP		= 0,
	DECDCT_MODE_16BPP_BIT15	= 2,
	DECDCT_MODE_RAW			= -1
} DECDCTMODE;

typedef struct {
	const uint32_t	*input;
	uint32_t		window, next_window, remaining;
	int8_t			is_v3, bit_offset, block_index, coeff_index;
	uint16_t		quant_scale;
	int16_t			last_y, last_cr, last_cb;
} VLC_Context;

typedef struct {
	uint32_t mdec0_header;
	uint16_t quant_scale;
	uint16_t version;
} BS_Header;


#ifndef OPENROM_VERSION
void *memcpy(void *d, const void *s, unsigned int n);
#endif
int DecDCTvlcStart(VLC_Context *ctx, uint32_t *buf, size_t max_size, const uint32_t *bs);
int DecDCTvlcContinue(VLC_Context *ctx, uint32_t *buf, size_t max_size);
void DecDCTvlcCopyTableV3(VLC_TableV3 *addr);
