/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PC check of dsss.c: the frame train the S3 app sends (the same payload, counters, table and
 * rendering), written as 40 MS/s int16 I/Q pairs for frames.py to turn into a recording.
 *
 *   frames <rate 1|2> <count> <period us> <amp> <size> <out.raw>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dsss.h"

int main(int argc, char **argv)
{
	if (argc != 7) {
		fprintf(stderr, "usage: frames <rate 1|2> <count> <period us> <amp> <size> <out.raw>\n");
		return 2;
	}
	const int rate = atoi(argv[1]), count = atoi(argv[2]), period = atoi(argv[3]),
		  amp = atoi(argv[4]), size = atoi(argv[5]);
	static int16_t tab[2 * DSSS_TABLE];
	static struct dsss_frame f;
	static const uint8_t src[6] = {0x02, 0x57, 0x53, 0x44, 0x52, 0x01};
	uint8_t pl[DSSS_PAYLOAD_MAX];
	FILE *o = fopen(argv[6], "wb");

	if (!o || size < 10 || size > DSSS_PAYLOAD_MAX) {
		return 1;
	}
	dsss_table(tab, amp);
	const size_t span = (size_t)period * DSSS_SPS;
	int16_t *iq = malloc(span * 4);

	for (int c = 0; c < count; c++) {
		memcpy(pl, "WSDR", 4);
		memcpy(pl + 4, &c, 4); /* (little-endian host, as the S3) */
		pl[8] = 0;
		pl[9] = (uint8_t)rate;
		for (int k = 10; k < size; k++) {
			pl[k] = (uint8_t)k;
		}
		if (dsss_espnow(&f, rate, src, (uint16_t)c, pl, (size_t)size, 0x12345678u * (uint32_t)(c + 1),
				(uint8_t)(0x6C ^ c)) != 0 ||
		    (size_t)f.n * DSSS_SPS > span) {
			fprintf(stderr, "frame does not fit the period\n");
			return 1;
		}
		dsss_render(&f, tab, 0, (uint32_t)span, iq);
		fwrite(iq, 4, span, o);
		if (c == 0) {
			fprintf(stderr, "frame: %u symbols (%u us), MPDU %u bytes\n", f.n, f.n, f.mpdu_len);
		}
	}
	fclose(o);
	return 0;
}
