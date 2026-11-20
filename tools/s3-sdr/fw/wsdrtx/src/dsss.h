/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 802.11b DSSS frames (1 Mb/s DBPSK, 2 Mb/s DQPSK, long preamble) as one phase per 1 us symbol,
 * and the waveform table that turns them into samples at 40 MS/s: one symbol is exactly 40
 * samples, so each output sample is a table entry picked by the previous, current and next symbol.
 * Plain C, no Zephyr: the S3 app and the PC test (host/) build the same file.
 */
#ifndef WSDRTX_DSSS_H_
#define WSDRTX_DSSS_H_

#include <stddef.h>
#include <stdint.h>

#define DSSS_SPS      40   /* samples per symbol (1 us) at 40 MS/s */
#define DSSS_LEAD     2    /* silent symbols before and after a frame */
#define DSSS_SILENT   4    /* the phase value of a silent symbol */
#define DSSS_STATES   5    /* phases 0..3 (quarter turns) and silence */
#define DSSS_TABLE    (DSSS_STATES * DSSS_STATES * DSSS_STATES * DSSS_SPS)
#define DSSS_PAYLOAD_MAX 100 /* (the S3 app keeps a frame in 24 KiB of internal RAM with the table) */
/* The preamble and header (192 us) and the longest ESP-NOW frame at 1 Mb/s, plus the silences. */
#define DSSS_SYMS_MAX (192 + 8 * (24 + 8 + 7 + DSSS_PAYLOAD_MAX + 4) + 2 * DSSS_LEAD)

struct dsss_frame {
	uint16_t n;                 /* symbols, the silences included */
	uint16_t mpdu_len;          /* bytes, FCS included */
	uint8_t ph[DSSS_SYMS_MAX];  /* per symbol: quarter turns 0..3, or DSSS_SILENT */
};

/*
 * An ESP-NOW v1 broadcast (an Action frame, as ESP-IDF sends it) from @p src with sequence number
 * @p seq, carrying @p len bytes of @p payload, at @p rate_mbps 1 or 2. @p scr_seed: the scrambler's
 * start (7 bits). Returns 0, or -1 for a bad rate or length.
 */
int dsss_espnow(struct dsss_frame *f, int rate_mbps, const uint8_t src[6], uint16_t seq,
		const uint8_t *payload, size_t len, uint32_t rnd, uint8_t scr_seed);

/* The state of symbol k of @p f (k may be out of range: silence). */
static inline int dsss_state(const struct dsss_frame *f, long k)
{
	return k < 0 || k >= f->n ? DSSS_SILENT : f->ph[k];
}

/*
 * The waveform table: for (prev, cur, next) states and sample s of the symbol, entry
 * ((prev * 5 + cur) * 5 + next) * 40 + s holds I and Q at iq[2 * entry] and iq[2 * entry + 1]
 * (so an entry is one 32-bit word: it can be turned into a DAC word in place), with a
 * raised-cosine chip pulse (roll-off 0.5, +-3 chips: +-8.25 MHz), scaled so the largest |I| or
 * |Q| is @p amp.
 */
void dsss_table(int16_t *iq, int amp);

/* The table entry of output sample @p idx of @p f. */
static inline uint32_t dsss_entry(const struct dsss_frame *f, uint32_t idx)
{
	const long k = (long)(idx / DSSS_SPS);

	return ((uint32_t)(dsss_state(f, k - 1) * 5 + dsss_state(f, k)) * 5 + (uint32_t)dsss_state(f, k + 1)) *
		       DSSS_SPS + idx % DSSS_SPS;
}

/* Samples [@p index, @p index + @p n) of @p f as I/Q pairs, from the table (the PC test's path). */
void dsss_render(const struct dsss_frame *f, const int16_t *iq, uint32_t index, uint32_t n, int16_t *out);

#endif /* WSDRTX_DSSS_H_ */
