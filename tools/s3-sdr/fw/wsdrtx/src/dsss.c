/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 802.11b DSSS frames for the S3 transmitter (dsss.h). IEEE 802.11-2020 clause 16: the long
 * PLCP preamble (128 scrambled ones, SFD 0xF3A0), the PLCP header (SIGNAL, SERVICE, LENGTH in us,
 * CRC-16), the scrambler x^7 + x^4 + 1 over everything, DBPSK for the preamble, the header and a
 * 1 Mb/s payload, DQPSK (Gray: 00 0, 01 +90, 11 +180, 10 +270 degrees) for a 2 Mb/s one, and the
 * 11-chip Barker code, here shaped with a raised-cosine pulse.
 */
#include "dsss.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int8_t barker[11] = {1, -1, 1, 1, -1, 1, 1, 1, -1, -1, -1};

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;

	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int b = 0; b < 8; b++) {
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
		}
	}
	return ~c;
}

struct bits {
	uint8_t v[DSSS_SYMS_MAX * 2];
	size_t n;
};

static void put(struct bits *b, uint32_t value, int count) /* LSB first */
{
	for (int i = 0; i < count; i++) {
		b->v[b->n++] = (uint8_t)((value >> i) & 1u);
	}
}

int dsss_espnow(struct dsss_frame *f, int rate_mbps, const uint8_t src[6], uint16_t seq,
		const uint8_t *payload, size_t len, uint32_t rnd, uint8_t scr_seed)
{
	static struct bits b; /* (big: not on a thread stack) */
	uint8_t m[24 + 8 + 7 + DSSS_PAYLOAD_MAX + 4];
	size_t n = 0;

	if ((rate_mbps != 1 && rate_mbps != 2) || len > DSSS_PAYLOAD_MAX) {
		return -1;
	}
	/* MAC header: an Action frame (management, subtype 13), broadcast, no DS bits. */
	m[n++] = 0xD0;
	m[n++] = 0x00;
	m[n++] = 0x00; /* duration */
	m[n++] = 0x00;
	memset(&m[n], 0xFF, 6); /* addr1: everyone */
	n += 6;
	memcpy(&m[n], src, 6); /* addr2: the sender */
	n += 6;
	memset(&m[n], 0xFF, 6); /* addr3: the broadcast BSSID, as ESP-NOW */
	n += 6;
	m[n++] = (uint8_t)(seq << 4);
	m[n++] = (uint8_t)(seq >> 4);
	/* Body: category 127 (vendor), Espressif's OUI, 4 random bytes, one ESP-NOW element (v1). */
	m[n++] = 127;
	m[n++] = 0x18;
	m[n++] = 0xFE;
	m[n++] = 0x34;
	for (int i = 0; i < 4; i++) {
		m[n++] = (uint8_t)(rnd >> (8 * i));
	}
	m[n++] = 221;
	m[n++] = (uint8_t)(5 + len);
	m[n++] = 0x18;
	m[n++] = 0xFE;
	m[n++] = 0x34;
	m[n++] = 4; /* type: ESP-NOW */
	m[n++] = 1; /* version 1 */
	memcpy(&m[n], payload, len);
	n += len;
	const uint32_t fcs = crc32_ieee(m, n);

	for (int i = 0; i < 4; i++) {
		m[n++] = (uint8_t)(fcs >> (8 * i));
	}

	/* PLCP preamble and header, then the MPDU, all LSB first but the header CRC. */
	b.n = 0;
	put(&b, 0xFFFFFFFFu, 32);
	put(&b, 0xFFFFFFFFu, 32);
	put(&b, 0xFFFFFFFFu, 32);
	put(&b, 0xFFFFFFFFu, 32);
	put(&b, 0xF3A0u, 16);
	const size_t h0 = b.n;
	const unsigned length_us = (unsigned)(n * 8 / (size_t)rate_mbps);

	put(&b, rate_mbps == 1 ? 0x0Au : 0x14u, 8);
	put(&b, 0x00u, 8); /* SERVICE */
	put(&b, length_us, 16);
	unsigned reg = 0xFFFFu; /* CRC-16 CCITT, preset ones, ones' complement, MSB first */

	for (size_t j = h0; j < h0 + 32; j++) {
		const unsigned fb = ((reg >> 15) & 1u) ^ b.v[j];

		reg = (reg << 1) & 0xFFFFu;
		if (fb) {
			reg ^= 0x1021u;
		}
	}
	reg ^= 0xFFFFu;
	for (int j = 15; j >= 0; j--) {
		b.v[b.n++] = (uint8_t)((reg >> j) & 1u);
	}
	const size_t p0 = b.n; /* the payload's first bit */

	for (size_t i = 0; i < n; i++) {
		put(&b, m[i], 8);
	}

	/* Scramble: s = b ^ s[-4] ^ s[-7]. */
	unsigned sr = scr_seed & 0x7Fu;

	for (size_t i = 0; i < b.n; i++) {
		const unsigned s = b.v[i] ^ (((sr >> 6) ^ (sr >> 3)) & 1u);

		b.v[i] = (uint8_t)s;
		sr = ((sr << 1) | s) & 0x7Fu;
	}

	/* Phases: DBPSK (a 1 turns by 180 degrees) up to the payload, then DBPSK or DQPSK. */
	size_t k = 0;
	unsigned ph = 0;

	for (int i = 0; i < DSSS_LEAD; i++) {
		f->ph[k++] = DSSS_SILENT;
	}
	for (size_t i = 0; i < p0; i++) {
		ph = (ph + 2u * b.v[i]) & 3u;
		f->ph[k++] = (uint8_t)ph;
	}
	if (rate_mbps == 1) {
		for (size_t i = p0; i < b.n; i++) {
			ph = (ph + 2u * b.v[i]) & 3u;
			f->ph[k++] = (uint8_t)ph;
		}
	} else {
		static const uint8_t step[4] = {0, 1, 3, 2}; /* (d0 << 1 | d1): 00 0, 01 1, 10 3, 11 2 */

		for (size_t i = p0; i + 1 < b.n; i += 2) {
			ph = (ph + step[(b.v[i] << 1) | b.v[i + 1]]) & 3u;
			f->ph[k++] = (uint8_t)ph;
		}
	}
	for (int i = 0; i < DSSS_LEAD; i++) {
		f->ph[k++] = DSSS_SILENT;
	}
	f->n = (uint16_t)k;
	f->mpdu_len = (uint16_t)n;
	return 0;
}

/* Raised-cosine pulse, roll-off 0.5, at the chip time u (in chips), cut at +-3 chips. */
static double pulse(double u)
{
	const double beta = 0.5;

	if (fabs(u) >= 3.0) {
		return 0.0;
	}
	const double s = fabs(u) < 1e-9 ? 1.0 : sin(M_PI * u) / (M_PI * u);
	const double d = 1.0 - (2.0 * beta * u) * (2.0 * beta * u);

	if (fabs(d) < 1e-9) {
		return M_PI / 4.0 * sin(M_PI / (2.0 * beta)) / (M_PI / (2.0 * beta));
	}
	return s * cos(M_PI * beta * u) / d;
}

void dsss_table(int16_t *iq, int amp)
{
	static const int re[DSSS_STATES] = {1, 0, -1, 0, 0}, im[DSSS_STATES] = {0, 1, 0, -1, 0};
	double h[3][DSSS_SPS]; /* the previous, current and next symbol's chips at each sample */
	double peak = 0.0;

	for (int x = 0; x < 3; x++) {
		for (int s = 0; s < DSSS_SPS; s++) {
			double acc = 0.0;
			const double t = (double)s / DSSS_SPS; /* in symbols */

			for (int j = 0; j < 11; j++) {
				const double centre = (double)(x - 1) + (j + 0.5) / 11.0;

				acc += barker[j] * pulse((t - centre) * 11.0);
			}
			h[x][s] = acc;
		}
	}
	for (int pass = 0; pass < 2; pass++) {
		const double g = pass == 0 ? 1.0 : (double)amp / peak;

		for (int p = 0; p < DSSS_STATES; p++) {
			for (int c = 0; c < DSSS_STATES; c++) {
				for (int nx = 0; nx < DSSS_STATES; nx++) {
					const int st[3] = {p, c, nx};

					for (int s = 0; s < DSSS_SPS; s++) {
						double vi = 0.0, vq = 0.0;

						for (int x = 0; x < 3; x++) {
							vi += re[st[x]] * h[x][s];
							vq += im[st[x]] * h[x][s];
						}
						if (pass == 0) {
							peak = fmax(peak, fmax(fabs(vi), fabs(vq)));
						} else {
							const size_t at = (size_t)((p * 5 + c) * 5 + nx) * DSSS_SPS + s;

							iq[2 * at] = (int16_t)lround(vi * g);
							iq[2 * at + 1] = (int16_t)lround(vq * g);
						}
					}
				}
			}
		}
	}
}

void dsss_render(const struct dsss_frame *f, const int16_t *iq, uint32_t index, uint32_t n, int16_t *out)
{
	for (uint32_t j = 0; j < n; j++) {
		const uint32_t at = dsss_entry(f, index + j);

		out[2 * j] = iq[2 * at];
		out[2 * j + 1] = iq[2 * at + 1];
	}
}
