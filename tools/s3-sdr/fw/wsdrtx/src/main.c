/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wsdrtx: the S3 as a known 2.4 GHz source for wsdr receiver tests. A multiapp extension with
 * rfprobe's root command, "probe", and its tune, gain and tone commands (adapted from
 * esp-sdr-zephyr's apps/rfprobe, GPL-3.0-or-later), plus "probe frames": numbered ESP-NOW frames
 * (802.11b DSSS, dsss.c) played through the DAC by esp_sdr_tx_dac_play_gen(), one call per frame.
 *
 * Bench limits, enforced here: transmit step 0, amplitude up to 400; "probe frames" alone may use up
 * to step FRAMES_STEP_MAX (the owner's OK for the decode test, 2026-11-20), back to 0 after each burst.
 */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
#include "app_usb.h"
#endif

#include "dsss.h"

#define TX_STEP_MAX 0
#define FRAMES_STEP_MAX 8
#define AMP_MAX     400
#define RUN_MAX_MS  60000

/* The waveform table (as DAC words) and the frame the generator reads, in internal RAM. */
ESP_SDR_HIGH_BUF_DEFINE(uint32_t, wave, [DSSS_TABLE], 4);
ESP_SDR_HIGH_BUF_DEFINE(struct dsss_frame, frame, [1], 4);

static bool tone_active;
static int wave_amp = -1;
static uint32_t counter; /* numbers every frame of this boot (one recording never repeats one) */
static const uint8_t src_mac[6] = {0x02, 0x57, 0x53, 0x44, 0x52, 0x01}; /* locally administered */

static struct {
	uint32_t sent, errors, period_ms, airtime_us;
	int last_ret, rate, size, amp;
} st;

static long arg(char **argv, size_t argc, size_t i, long def)
{
	return argc > i ? strtol(argv[i], NULL, 0) : def;
}

static bool busy(const struct shell *sh)
{
	if (tone_active) {
		shell_error(sh, "tone on: probe tone off first");
		return true;
	}
	return false;
}

/* f <MHz>: tune through the library's plan. */
static int cmd_f(const struct shell *sh, size_t argc, char **argv)
{
	long mhz = arg(argv, argc, 1, 2437);
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	ret = mhz > 0 ? esp_sdr_set_freq((uint32_t)mhz) : -EINVAL;
	shell_print(sh, "f %ld ret %d", mhz, ret);
	return 0;
}

/* gain <rx index|-1> [tx step]: as rfprobe's, the transmit step limited to TX_STEP_MAX. */
static int cmd_gain(const struct shell *sh, size_t argc, char **argv)
{
	int ret = esp_sdr_rx_set_gain((int)arg(argv, argc, 1, ESP_SDR_RX_GAIN_AUTO));

	if (argc > 2) {
		long step = arg(argv, argc, 2, 0);
		int tret = step < 0 || step > TX_STEP_MAX ? -EPERM : esp_sdr_tx_set_gain((int)step);

		shell_print(sh, "txgain %d ret %d max %d", esp_sdr_tx_get_gain(), tret,
			    esp_sdr_tx_gain_max());
	}
	shell_print(sh, "rxgain %d ret %d max %d", esp_sdr_rx_get_gain(), ret,
		    esp_sdr_rx_gain_max());
	return 0;
}

/* tone on <Hz> [amp] [rate 0|1]: a whole number of periods in the bank, looped (rfprobe's). */
static int cmd_tone_on(const struct shell *sh, size_t argc, char **argv)
{
	long hz = arg(argv, argc, 1, 1000000);
	long amp = arg(argv, argc, 2, 400);
	enum esp_sdr_rate rate = (enum esp_sdr_rate)arg(argv, argc, 3, ESP_SDR_RATE_80MSPS);
	double fs = (double)esp_sdr_tx_rate_hz(rate);
	size_t words, n;
	long cycles;
	uint32_t *bank;
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	if (fs == 0.0 || fabs((double)hz) >= fs / 2.0 || amp < 1 || amp > AMP_MAX ||
	    esp_sdr_tx_get_gain() > TX_STEP_MAX) {
		shell_error(sh, "tone: rate 0 or 1, |Hz| below rate / 2, amp 1..%d, tx step 0",
			    AMP_MAX);
		return -EINVAL;
	}
	ret = esp_sdr_tx_loop_begin(rate);
	if (ret != 0) {
		shell_error(sh, "tone: begin %d", ret);
		return ret;
	}
	bank = esp_sdr_tx_loop_buf(0, &words);
	n = esp_sdr_tone_period((double)hz, fs, ESP_SDR_SAMPLES_MIN,
				MIN(words, ESP_SDR_SAMPLES_MAX), &cycles);
	for (size_t k = 0; k < n; k++) {
		double ph = esp_sdr_tone_phase(cycles, k, n);

		bank[k] = esp_sdr_tx_word((int16_t)lround((double)amp * cos(ph)),
					  (int16_t)lround((double)amp * sin(ph)));
	}
	ret = esp_sdr_tx_loop_start(0, n);
	if (ret != 0) {
		esp_sdr_tx_loop_end();
		shell_error(sh, "tone: start %d", ret);
		return ret;
	}
	tone_active = true;
	shell_print(sh, "tone on %u MHz %+ld Hz period %u amp %ld txgain %d",
		    esp_sdr_get_freq(), lround((double)cycles * fs / (double)n), (unsigned int)n,
		    amp, esp_sdr_tx_get_gain());
	return 0;
}

static int cmd_tone_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (tone_active) {
		esp_sdr_tx_loop_end();
		tone_active = false;
	}
	shell_print(sh, "tone off");
	return 0;
}

/*
 * Runs in the DAC filler, interrupts locked, against the DAC clock (a 408 us bank): internal RAM
 * code (PACED_SAMPLE, as osmosdr's) and data, whole symbols copied 40 words at a time.
 */
ESP_SDR_CODE(PACED_SAMPLE, frame_gen)
static __attribute__((optimize("O3", "no-tree-loop-distribute-patterns"))) void
frame_gen(void *user, uint32_t *dst, uint64_t index, uint32_t n)
{
	const struct dsss_frame *f = user;
	uint32_t idx = (uint32_t)index;
	uint32_t *end = dst + n;

	while (dst < end) {
		const uint32_t s = idx % DSSS_SPS;
		const uint32_t *row = &wave[dsss_entry(f, idx)];

		if (s == 0U && end - dst >= DSSS_SPS) {
			for (int m = 0; m < DSSS_SPS; m += 8) {
				dst[m] = row[m];
				dst[m + 1] = row[m + 1];
				dst[m + 2] = row[m + 2];
				dst[m + 3] = row[m + 3];
				dst[m + 4] = row[m + 4];
				dst[m + 5] = row[m + 5];
				dst[m + 6] = row[m + 6];
				dst[m + 7] = row[m + 7];
			}
			dst += DSSS_SPS;
			idx += DSSS_SPS;
		} else {
			*dst++ = *row;
			idx++;
		}
	}
}

/* The DAC words of the table for amplitude @p amp: built as I/Q pairs, packed in place. */
static void wave_build(int amp)
{
	int16_t *iq = (int16_t *)wave;

	if (amp == wave_amp) {
		return;
	}
	dsss_table(iq, amp);
	for (size_t k = 0; k < DSSS_TABLE; k++) {
		const int16_t i = iq[2 * k], q = iq[2 * k + 1];

		wave[k] = esp_sdr_tx_word(i, q);
	}
	wave_amp = amp;
}

/*
 * frames <count> [period ms] [amp] [rate 1|2] [size] [tx step]: <count> numbered ESP-NOW broadcasts at the
 * LO, one every period, then back to receive. The payload is the ESP-NOW bench's ("WSDR", u32
 * counter, mode 0, the rate, then bytes 10.. up to size), which `test_core --wifi` tallies.
 */
static int cmd_frames(const struct shell *sh, size_t argc, char **argv)
{
	const long count = arg(argv, argc, 1, 10), period = arg(argv, argc, 2, 10);
	const long amp = arg(argv, argc, 3, 400), rate = arg(argv, argc, 4, 1);
	const long size = arg(argv, argc, 5, 10), step = arg(argv, argc, 6, 0);
	struct dsss_frame *f = &frame[0];
	uint8_t pl[DSSS_PAYLOAD_MAX];

	if (busy(sh)) {
		return -EBUSY;
	}
	if (count < 1 || period < 3 || count * period > RUN_MAX_MS || amp < 1 || amp > AMP_MAX ||
	    (rate != 1 && rate != 2) || size < 10 || size > DSSS_PAYLOAD_MAX || step < 0 ||
	    step > FRAMES_STEP_MAX) {
		shell_error(sh, "frames: count >= 1, period >= 3 ms, count * period <= %d ms, amp 1..%d, "
				"rate 1|2, size 10..%d, tx step 0..%d",
			    RUN_MAX_MS, AMP_MAX, DSSS_PAYLOAD_MAX, FRAMES_STEP_MAX);
		return -EINVAL;
	}
	if (esp_sdr_tx_set_gain((int)step) != 0 || esp_sdr_tx_get_gain() != step) {
		(void)esp_sdr_tx_set_gain(0);
		shell_error(sh, "frames: tx step %ld not set", step);
		return -EIO;
	}
	wave_build((int)amp);
	st = (typeof(st)){.period_ms = (uint32_t)period, .rate = (int)rate, .size = (int)size,
			  .amp = (int)amp};
	int64_t next = k_uptime_get();

	for (long c = 0; c < count; c++) {
		const uint32_t n = counter++;

		memcpy(pl, "WSDR", 4);
		memcpy(pl + 4, &n, 4);
		pl[8] = 0;
		pl[9] = (uint8_t)rate;
		for (long k = 10; k < size; k++) {
			pl[k] = (uint8_t)k;
		}
		if (dsss_espnow(f, (int)rate, src_mac, (uint16_t)n, pl, (size_t)size,
				n * 2654435761u, (uint8_t)(0x6C ^ n)) != 0) {
			(void)esp_sdr_tx_set_gain(0);
			return -EINVAL;
		}
		st.airtime_us = f->n - 2U * DSSS_LEAD;
		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += period;
		st.last_ret = esp_sdr_tx_dac_play_gen(frame_gen, f, (uint64_t)f->n * DSSS_SPS);
		if (st.last_ret == 0) {
			st.sent++;
		} else {
			st.errors++;
		}
	}
	struct esp_sdr_tx_dac_stats ds;

	esp_sdr_tx_dac_get_stats(&ds);
	shell_print(sh,
		    "frames %u sent, %u errors (last %d); counters %u..%u; %u MHz, %d Mb/s, %d bytes "
		    "(%u us), every %u ms, amp %d, txgain %d",
		    st.sent, st.errors, st.last_ret, counter - (uint32_t)count, counter - 1U,
		    esp_sdr_get_freq(), st.rate, st.size, st.airtime_us, st.period_ms, st.amp,
		    esp_sdr_tx_get_gain());
	(void)esp_sdr_tx_set_gain(0); /* (the raised step is for this burst only) */
	shell_print(sh,
		    "last frame's DAC session: restarts %u, errors %u, fill max %u us, "
		    "slack min %d us, start margin %d us, simd %d",
		    ds.restarts, ds.errors, ds.fill_us_max,
		    ds.slack_us_min, ds.start_margin_us, ds.simd);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(tone_cmds,
	SHELL_CMD_ARG(on, NULL, "<Hz> [amp] [rate 0|1]: loop a tone at the LO + Hz", cmd_tone_on,
		      1, 3),
	SHELL_CMD(off, NULL, "stop the tone", cmd_tone_off),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(probe_cmds,
	SHELL_CMD_ARG(f, NULL, "<MHz>: tune through the library's plan", cmd_f, 2, 0),
	SHELL_CMD_ARG(gain, NULL, "<rx index|-1> [tx step, 0 only]", cmd_gain, 2, 1),
	SHELL_CMD(tone, &tone_cmds, "loop a tone", NULL),
	SHELL_CMD_ARG(frames, NULL,
		      "<count> [period ms] [amp] [rate 1|2] [size] [tx step 0..8]: numbered ESP-NOW frames",
		      cmd_frames, 2, 5),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(probe, &probe_cmds, "wsdrtx: tones and ESP-NOW frames at TX step 0", NULL);

int main(void)
{
	int ret;

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	if (app_usb_init() != 0) {
		printk("wsdrtx: usb init failed\n");
	}
#endif
	ret = esp_sdr_init();
	if (ret != 0) {
		printk("wsdrtx: init failed (%d)\n", ret);
		return 0;
	}
	(void)esp_sdr_rx_set_lpf(0);
	(void)esp_sdr_tx_set_lpf(0, 0);
	(void)esp_sdr_tx_set_gain(0);
	printk("wsdrtx: ready\n");
	return 0;
}
