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

#ifdef _WIN32
#define sleep Sleep
#undef min
#undef max
#endif

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
int short_output = 0;
int allowed_errors = 5;
FILE *file;
int adsb_frame[14];
#define preamble_len		16
#define long_frame		112
#define short_frame		56

void usage(void)
{
	fprintf(stderr,
		"rtl_adsb, a simple ADS-B decoder\n\n"
		"Use:\trtl_adsb [-R] [-g gain] [-p ppm] [output file]\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-R output raw bitstream (default: off)]\n"
		"\t[-S show short frames (default: off)]\n"
		"\t[-e allowed_errors (default: 5)]\n"
		"\t[-g tuner_gain (default: automatic)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\tfilename (a '-' dumps samples to stdout)\n"
		"\t (omitting the filename also uses stdout)\n\n"
		"Streaming with netcat:\n"
		"\trtl_adsb -R | netcat -lp 8080\n"
		"\twhile true; do rtl_adsb -R | nc -lp 8080; done\n"
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

void display(int *frame, int len)
{
	int i;
	if (!short_output && len <= short_frame) {
		return;}
	if (raw_output) {
		fprintf(file, "*");
		for (i=0; i<((len+7)/8); i++) {
			fprintf(file, "%02x", frame[i]);}
		fprintf(file, ";\r\n");
		return;
	}
	fprintf(file, "----------\n");
	fprintf(file, "DF=%x CA=%x\n", (frame[0] >> 3) & 0x1f, frame[0] & 0x07);
	fprintf(file, "ICAO Address=%06x\n", frame[1] << 16 | frame[2] << 8 | frame[3]);
	if (len <= short_frame) {
		return;}
	fprintf(file, "PI=0x%06x\n",  frame[11] << 16 | frame[12] << 8 | frame[13]);
	fprintf(file, "Type Code=%x S.Type/Ant.=%x\n", (frame[4] >> 3) & 0x1f, frame[4] & 0x07);
}

int magnitute(unsigned char *buf, int len)
/* takes i/q, changes buf in place, returns new len */
{
	int i, mag;
	for (i=0; i<len; i+=2) {
		mag = abs((int)buf[i]-128) + abs((int)buf[i+1]-128);
		if (mag > 255) {  // todo, compression
			mag = 255;}
		buf[i/2] = (unsigned char)mag;
	}
	return len/2;
}

inline unsigned char single_manchester(unsigned char a, unsigned char b, unsigned char c, unsigned char d)
/* takes 4 consecutive real samples, return 0 or 1, 255 on error */
{
	int bit, bit_p;
	bit_p = a > b;
	bit   = c > d;
	if ( bit &&  bit_p && c > b && d < a) {
		return 1;}
	if ( bit && !bit_p && c > a && d < b) {
		return 1;}
	if (!bit &&  bit_p && c < a && d > b) {
		return 0;}
	if (!bit && !bit_p && c < b && d > a) {
		return 0;}
	return 255;
}

inline unsigned char min(unsigned char a, unsigned char b)
{
	return a<b ? a : b;
}

inline unsigned char max(unsigned char a, unsigned char b)
{
	return a>b ? a : b;
}

inline int preamble(unsigned char *buf, int len, int i)
/* returns 0/1 for preamble at index i */
{
	int i2;
	unsigned char low  = 0;
	unsigned char high = 255;
	for (i2=0; i2<preamble_len; i2++) {
		switch (i2) {
			case 0:
			case 2:
			case 7:
			case 9:
				high = min(high, buf[i+i2]);
				break;
			default:
				low  = max(low,  buf[i+i2]);
				break;
		}
		if (high <= low) {
			return 0;}
	}
	return 1;
}

void manchester(unsigned char *buf, int len)
/* overwrites magnitude buffer with valid bits (255 on errors) */
{
	/* a and b hold old values to verify local manchester */
	unsigned char a=0, b=0;
	unsigned char bit;
	int i, i2, start, errors;
	// todo, allow wrap across buffers
	i = 0;
	while (i < len) {
		/* find preamble */
		for ( ; i < (len - preamble_len); i++) {
			if (!preamble(buf, len, i)) {
				continue;}
			a = buf[i];
			b = buf[i+1];
			for (i2=0; i2<preamble_len; i2++) {
				buf[i+i2] = 253;}
			i += preamble_len;
			//printf("preamble found\n");
			break;
		}
		i2 = start = i;
		errors = 0;
		/* mark bits until encoding breaks */
		for ( ; i < len; i+=2, i2++) {
			bit = single_manchester(a, b, buf[i], buf[i+1]);
			a = buf[i];
			b = buf[i+1];
			if (bit == 255) {
				errors += 1;
				if (errors > allowed_errors) {
					buf[i2] = 255;
					break;
				} else {
					bit = 0;
					a = 0;
					b = 255;
				}
			}
			buf[i] = buf[i+1] = 254;  /* to be overwritten */
			buf[i2] = bit;
		}
		// todo, nuke short segments
		//printf("%i\n", i2 - start);
	}
}

void messages(unsigned char *buf, int len)
{
	int i, i2, start, preamble_found;
	int data_i, index, shift, frame_len;
	// todo, allow wrap across buffers
	for (i=0; i<len; i++) {
		if (buf[i] > 1) {
			continue;}
		frame_len = long_frame;
		data_i = 0;
		for (index=0; index<14; index++) {
			adsb_frame[index] = 0;}
		for(; i<len && buf[i]<=1 && data_i<frame_len; i++, data_i++) {
			if (buf[i]) {
				index = data_i / 8;
				shift = 7 - (data_i % 8);
				adsb_frame[index] |= (unsigned char)(1<<shift);
			}
			if (data_i == 7) {
				//if (adsb_frame[0] == 0) {
				//    break;}
				if (adsb_frame[0] & 0x80) {
					frame_len = long_frame;}
				else {
					frame_len = short_frame;}
			}
		}
		// todo, check CRC
		if (data_i < (frame_len-1)) {
			continue;}
		//fprintf(file, "bits: %i\n", data_i);
		display(adsb_frame, frame_len);
		fflush(file);
	}
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
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
	int len;
	while (!do_exit) {
		sem_wait(&data_ready);
		len = magnitute(buffer, DEFAULT_BUF_LENGTH);
		manchester(buffer, len);
		messages(buffer, len);
		//fsync(file);
	}
	rtlsdr_cancel_async(dev);
	return 0;
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	char *filename = NULL;
	int n_read, r, opt;
	int i, gain = AUTO_GAIN; // tenths of a dB
	uint32_t dev_index = 0;
	int device_count;
	int ppm_error = 0;
	char vendor[256], product[256], serial[256];
	sem_init(&data_ready, 0, 0);

	while ((opt = getopt(argc, argv, "g:p:e:RS")) != -1)
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
		case 'S':
			short_output = 1;
			break;
		case 'e':
			allowed_errors = atoi(optarg);
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

