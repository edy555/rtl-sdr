/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Hoernchen <la@tfc-server.de>
 * Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 * Copyright (C) 2012 by Youssef Touil <youssef@sdrsharp.com>
 * Copyright (C) 2012 by Ian Gilmour <ian@sdrsharp.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include "getopt/getopt.h"
#endif

#include <semaphore.h>
#include <pthread.h>
#include <libusb.h>

#include "rtl-sdr.h"

#define ADSB_RATE			2000000
#define ADSB_FREQ			1090000000
#define DEFAULT_ASYNC_BUF_NUMBER	32
#define DEFAULT_BUF_LENGTH		(128 * 16384)
#define AUTO_GAIN			-100

static pthread_t demod_thread;
static sem_t data_ready;
static volatile int do_exit = 0;
static rtlsdr_dev_t *dev = NULL;

/* todo, bundle these up in a struct */
uint8_t *buffer;
int raw_output = 0;
char adsb_preamble[] = {1,0,1,0, 0,0,0,1, 0,1,0,0, 0,0,0,0};
#define preamble_len		16
#define long_frame		112
#define short_frame		56
int adsb_frame[14];
unsigned char manch_frame[long_frame*2];

void usage(void)
{
	fprintf(stderr,
		"rtl_adsb, a simple ADS-B decoder\n\n"
		"Use:\trtl_adsb [-R] [-g gain] [-p ppm] [output file]\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-R output raw bitstream (default: off)]\n"
		"\t[-g tuner_gain (default: automatic)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\tfilename (a '-' dumps samples to stdout)\n"
		"\t (omitting the filename also uses stdout)\n"
		"\n");
	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
	rtlsdr_cancel_async(dev);
}
#endif

void display(int *frame)
{
	printf("DF=%x CA=%x\n", (frame[0] >> 3) & 0x1f, frame[0] & 0x07);
	printf("ICAO Address=%06x\n", frame[1] << 16 | frame[2] << 8 | frame[3]);
	printf("PI=0x%06x\n",  frame[11] << 16 | frame[12] << 8 | frame[13]);
	printf("Type Code=%x S.Type/Ant.=%x\n", (frame[4] >> 3) & 0x1f, frame[4] & 0x07);
	printf("----------\n");
}

void threshold_bits(unsigned char *buf, int len)
/* changes in place */
{
	#define scale 128
	//static int avg;
	static double avg;
	int i, mag, threshold;
	for (i=0; i<len; i+=2) {
		mag = abs((int)buf[i] - 128) + abs((int)buf[i+1] - 128);
		// scaled to 15 bits, might need more
		//avg = avg - (avg+scale/2)/scale + mag;
		//threshold = avg + 80*scale;
		avg = 0.99 * avg + 0.01 * (float)mag;
		//threshold = (int)avg + 80;
		//threshold = (int)(avg+0.5) + 8;
		//threshold = (int)avg;
		//buf[i] = (mag*scale) > threshold;
		//buf[i] = mag > threshold;
		buf[i] = (double)mag > (avg * 2.0);
		buf[i+1] = buf[i];
		//printf("%f\n", avg);
	}
}

void demanchester(void)
{
    	int i, index, shift;
	unsigned char bit = 0;
	for (i=0; i<14; i++) {
		adsb_frame[i] = 0;}
	for (i=0; i<long_frame*2; i+=2) {
		if (manch_frame[i] == 0 && manch_frame[i+1] == 1) {
			bit = 0;}
		else if (manch_frame[i] == 1 && manch_frame[i+1] == 0) {
			bit = 1;}
		else {
			return;}
		if (bit) {
			index = i / 2 / 8;
			shift = 7 - ((i / 2) % 8);
			// needs oob check?
			adsb_frame[index] += (1 << shift);
		}
	}
	//if (raw_output) {
		printf("* ");
		for (i=0; i<14; i++) {
			printf("%02x", adsb_frame[i]);}
		printf(";\n"); //return;}
	// bail on short frame
	if (((adsb_frame[0] >> 3) & 0x1f) < 16) {
		return;}
	// long frame
	display(adsb_frame);
	
}

void state_engine(unsigned char* buf, int len)
// todo, organize this with inline functions
{
	static int preamble_i, manch_i;
	int i = 0;  /* always inc by 2 */
	while (i<len) {
		/* preamble detection */
		for (; manch_i<=0 && i<len; i+=2) {
			manch_i = -1;
			if (buf[i] == adsb_preamble[preamble_i]) {
				preamble_i++;}
			else {
				preamble_i = 0;}
			if (preamble_i >= preamble_len) {
				manch_i = 0;
				preamble_i = 0;
				break;
			}
		}
		i+=2;
		/* load manchester frame */
		for (; manch_i<(long_frame*2) && i<len; i+=2, manch_i++) {
			manch_frame[manch_i] = buf[i];}
		i+=2;
		if (i<len) {
			demanchester();
			manch_i = 0;
		}
	}
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	struct fm_state *fm2 = ctx;
	int dr_val;
	if (do_exit) {
		return;}
	memcpy(buffer, buf, len);
	sem_getvalue(&data_ready, &dr_val);
	if (dr_val <= 0) {
		sem_post(&data_ready);}
}

static void *demod_thread_fn(void *arg)
{
	while (!do_exit) {
		sem_wait(&data_ready);
		threshold_bits(buffer, DEFAULT_BUF_LENGTH);
		state_engine(buffer, DEFAULT_BUF_LENGTH);
		if (do_exit) {
			rtlsdr_cancel_async(dev);}
	}
	return 0;
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	char *filename = NULL;
	FILE *file;
	int n_read, r, opt;
	int i, gain = AUTO_GAIN; // tenths of a dB
	uint32_t dev_index = 0;
	int device_count;
	int ppm_error = 0;
	char vendor[256], product[256], serial[256];
	sem_init(&data_ready, 0, 0);

	while ((opt = getopt(argc, argv, "g:p:R")) != -1)
	{
		switch (opt) {
		case 'd':
			dev_index = atoi(optarg);
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10);
			break;
		case 'p':
			ppm_error = atoi(optarg);
			break;
		case 'R':
			raw_output = 1;
			break;
		default:
			usage();
			return 0;
		}
	}

	if (argc <= optind) {
		//usage();
		filename = "-";
	} else {
		filename = argv[optind];
	}

	buffer = malloc(DEFAULT_BUF_LENGTH * sizeof(uint8_t));

	device_count = rtlsdr_get_device_count();
	if (!device_count) {
		fprintf(stderr, "No supported devices found.\n");
		exit(1);
	}

	fprintf(stderr, "Found %d device(s):\n", device_count);
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
	}
	fprintf(stderr, "\n");

	fprintf(stderr, "Using device %d: %s\n",
		dev_index, rtlsdr_get_device_name(dev_index));

	r = rtlsdr_open(&dev, dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		exit(1);
	}
#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	if (strcmp(filename, "-") == 0) { /* Write samples to stdout */
		file = stdout;
#ifdef _WIN32
		_setmode(_fileno(file), _O_BINARY);
#endif
	} else {
		file = fopen(filename, "wb");
		if (!file) {
			fprintf(stderr, "Failed to open %s\n", filename);
			exit(1);
		}
	}

	/* Set the tuner gain */
	if (gain == AUTO_GAIN) {
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
	} else {
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		r = rtlsdr_set_tuner_gain(dev, gain);
	}
	if (r != 0) {
		fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
	} else if (gain == AUTO_GAIN) {
		fprintf(stderr, "Tuner gain set to automatic.\n");
	} else {
		fprintf(stderr, "Tuner gain set to %0.2f dB.\n", gain/10.0);
	}

	r = rtlsdr_set_freq_correction(dev, ppm_error);
	r = rtlsdr_set_agc_mode(dev, 1);

	/* Set the tuner frequency */
	r = rtlsdr_set_center_freq(dev, ADSB_FREQ);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set center freq.\n");}
	else {
		fprintf(stderr, "Tuned to %u Hz.\n", ADSB_FREQ);}

	/* Set the sample rate */
	fprintf(stderr, "Sampling at %u Hz.\n", ADSB_RATE);
	r = rtlsdr_set_sample_rate(dev, ADSB_RATE);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");}

	/* Reset endpoint before we start reading from it (mandatory) */
	r = rtlsdr_reset_buffer(dev);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");}

	/* flush old junk */
	sleep(1);
	rtlsdr_read_sync(dev, NULL, 4096, NULL);

	pthread_create(&demod_thread, NULL, demod_thread_fn, (void *)(NULL));
	rtlsdr_read_async(dev, rtlsdr_callback, (void *)(NULL),
			      DEFAULT_ASYNC_BUF_NUMBER,
			      DEFAULT_BUF_LENGTH);


	if (do_exit) {
		fprintf(stderr, "\nUser cancel, exiting...\n");}
	else {
		fprintf(stderr, "\nLibrary error %d, exiting...\n", r);}
	rtlsdr_cancel_async(dev);

	if (file != stdout) {
		fclose(file);}

	rtlsdr_close(dev);
	free(buffer);
	return r >= 0 ? r : -r;
}

