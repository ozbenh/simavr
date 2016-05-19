/*
	charlcd.c

	Copyright Luki <humbell@ethz.ch>
	Copyright 2011 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <libgen.h>
#include <assert.h>
#include <getopt.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "sim_avr.h"
#include "avr_ioport.h"
#include "sim_elf.h"
#include "sim_gdb.h"
#include "sim_vcd_file.h"
#include "avr_adc.h"

#if __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif
#include <pthread.h>

#include "hd44780_glut.h"


int window;

avr_t * avr = NULL;
avr_vcd_t vcd_file;
hd44780_t hd44780;
double wave_step;
uint64_t wave_step_ns;
double *wave_data;
uint32_t wave_size;
double wave_mult;
double wave_offset;

int color = 0;
uint32_t colors[][4] = {
		{ 0x00aa00ff, 0x00cc00ff, 0x000000ff, 0x00000055 },	// fluo green
		{ 0xaa0000ff, 0xcc0000ff, 0x000000ff, 0x00000055 },	// red
};

int green_led, red_led;

#define IRQ_WAVE_COUNT 2
#define IRQ_WAVE_TRIGGER_IN 0
#define IRQ_WAVE_VALUE_OUT 1

static const char * irq_wave_names[IRQ_WAVE_COUNT] = {
	[IRQ_WAVE_TRIGGER_IN] = "8<wave.trigger",
	[IRQ_WAVE_VALUE_OUT] = "16>wave.out",
};

avr_irq_t *wirqs;
avr_cycle_count_t wave_start_cycle;
uint32_t wave_adc_mux_number;
bool wave_playing;
bool vcd_started;

static void *
avr_run_thread(
		void * ignore)
{
	while (1) {
		avr_run(avr);
	}
	return NULL;
}

void keyCB(unsigned char key, int x, int y)
{
	switch (key) {
	case 'q':
		if (vcd_started) {
			vcd_started = false;
			avr_vcd_stop(&vcd_file);
		}
		exit(0);
		break;
	case 'r':
		printf("Starting VCD trace; press 's' to stop\n");
		if (!vcd_started) {
			avr_vcd_start(&vcd_file);
			vcd_started = true;
		}
		break;
	case 's':
		printf("Stopping VCD trace\n");
		if (vcd_started) {
			vcd_started = false;
			avr_vcd_stop(&vcd_file);
		}
		break;
	case 'p':
		printf("Starting wave play\n");
		wave_start_cycle = avr->cycle;
		wave_playing = true;
		break;
	}
}


void displayCB(void)		/* function called whenever redisplay needed */
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_MODELVIEW); // Select modelview matrix
	glPushMatrix();
	glLoadIdentity(); // Start with an identity matrix
	glScalef(3, 3, 1);

	hd44780_gl_draw(&hd44780,
			colors[color][0], /* background */
			colors[color][1], /* character background */
			colors[color][2], /* text */
			colors[color][3] /* shadow */ );
	glPopMatrix();

	glPushMatrix();
	glLoadIdentity(); // Start with an identity matrix
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	if (red_led)
		glColor3f(1.0,0.2,0.2);
	else
		glColor3f(0.5,0.0,0.0);
	glTranslatef(10, 70, 0);
	glRectf(0,0,14,14);
	if (green_led)
		glColor3f(0.2,1.0,0.2);
	else
		glColor3f(0.0,0.5,0.0);
	glTranslatef(40, 0, 0);
	glRectf(0,0,14,14);
	glPopMatrix();

	glColor3f(0.0f, 0.0f, 0.0f);
	glutSwapBuffers();
}

// gl timer. if the lcd is dirty, refresh display
void timerCB(int i)
{
	//static int oldstate = -1;
	// restart timer
	glutTimerFunc(1000/64, timerCB, 0);
	glutPostRedisplay();
}

int
initGL(int w, int h)
{
	// Set up projection matrix
	glMatrixMode(GL_PROJECTION); // Select projection matrix
	glLoadIdentity(); // Start with an identity matrix
	glOrtho(0, w, 0, h, 0, 10);
	glScalef(1,-1,1);
	glTranslatef(0, -1 * h, 0);

	glutDisplayFunc(displayCB);		/* set window's display callback */
	glutKeyboardFunc(keyCB);		/* set window's key callback */
	glutTimerFunc(1000 / 24, timerCB, 0);

	glEnable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);

	glClearColor(0.8f, 0.8f, 0.8f, 1.0f);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);

	hd44780_gl_init();

	return 1;
}

static void led_changed_hook(struct avr_irq_t * irq,
			     uint32_t value, void *param)
{
	int *led = param;

	*led = !value;
}

static void wave_trigger_hook(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_cycle_count_t cycle_now;
	uint32_t index, millivolts;
	double sample;

	sample = 0.0;

	if (wave_playing) {
		/* Calculate wave index */
		cycle_now = avr->cycle - wave_start_cycle;

		/* For now assume 1 cycle is 62.5ns */
		index = (cycle_now * 625u) / (wave_step_ns * 10);
		if (index < wave_size) {
			sample = wave_data[index];
			//printf("wave trigger cycle %lld index %d sample=%f\n",
			//	(unsigned long long)cycle_now, index, sample);
		} else {
			wave_playing = false;
			printf("End of wave !\n");
		}
	}

	millivolts = (sample * wave_mult) + wave_offset;
	avr_raise_irq(wirqs + IRQ_WAVE_VALUE_OUT, millivolts);
}

static void setup_wave_sensor(int adc_mux_number)
{
	wave_adc_mux_number = adc_mux_number;
	wirqs = avr_alloc_irq(&avr->irq_pool, 0, IRQ_WAVE_COUNT, irq_wave_names);
	avr_irq_register_notify(wirqs + IRQ_WAVE_TRIGGER_IN, wave_trigger_hook, NULL);

	avr_irq_t * src = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_OUT_TRIGGER);
	avr_irq_t * dst = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, adc_mux_number);
	if (src && dst) {
		avr_connect_irq(src, wirqs + IRQ_WAVE_TRIGGER_IN);
		avr_connect_irq(wirqs + IRQ_WAVE_VALUE_OUT, dst);
	}
}


static void load_wave(const char *filename)
{
	FILE *w = fopen(filename, "r");
	double t0,t1,v0,v1;
	uint32_t asize;

	printf("Loading wave file %s\n", filename);
	if (!w)  {
		perror("Failed to open wave file");
		exit(1);
	}

	/* Read first two entries */
	fscanf(w, "%lf,%lf\n", &t0, &v0);
	fscanf(w, "%lf,%lf\n", &t1, &v1);
	wave_step = t1 - t0;
	asize = 30 * (1.0/wave_step) + 10 /* some margin */;

	wave_step_ns = wave_step * 1000000000.0;
	printf("wave step: %fs (%d samples, %lldns)\n",
	       wave_step, asize, (unsigned long long)wave_step_ns);

	/* Allocating for 30s ... */
	wave_data = malloc(asize * sizeof(double));
	assert(wave_data);
	wave_data[wave_size++] = v0;
	wave_data[wave_size++] = v1;

	for (;;) {
		if (fscanf(w, "%lf,%lf\n", &t0, &v0) == EOF)
			break;
		t1 += wave_step;
		if (wave_size >= asize) {
			printf("too many samples !\n");
			break;
		}
		wave_data[wave_size++] = v0;
	}
	printf("loaded %d samples\n", wave_size);
	fclose(w);
}


static void create_avr(const char *fname, bool use_gdb, uint16_t gdb_port)
{
	elf_firmware_t f;

	elf_read_firmware(fname, &f);

	printf("Frequency=%fMhz mmcu=%s\n", ((double)f.frequency)/1000000, f.mmcu);

	avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		fprintf(stderr, "AVR '%s' not known\n", f.mmcu);
		exit(1);
	}

	avr_init(avr);

	avr_load_firmware(avr, &f);

	if (use_gdb) {
		avr->gdb_port = gdb_port;
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
	}

	avr->log = LOG_TRACE;
}

static void setup_lcd(void)
{
	unsigned int i;

	hd44780_init(avr, &hd44780, 16, 2);

	/* Connect Data Lines to Port D, 7-0, output from AVR only */
	for (i = 0; i < 8; i++) {
		avr_irq_t * iavr = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), i);
		avr_irq_t * ilcd = hd44780.irq + IRQ_HD44780_D7 - i;
		avr_connect_irq(iavr, ilcd);
	}
	/* Connect RS and E */
	avr_connect_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), 4),
			hd44780.irq + IRQ_HD44780_RS);
	avr_connect_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), 5),
			hd44780.irq + IRQ_HD44780_E);

	/* RW is forced to 0 */
	avr_raise_irq(hd44780.irq + IRQ_HD44780_RW, 0);

	/* Register LED changed hooks */
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 0),
				led_changed_hook, &red_led);
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 1),
				led_changed_hook, &green_led);
}

static void setup_vcd(void)
{
	avr_vcd_init(avr, "gtkwave_output.vcd", &vcd_file, 1 /* usec */);
	avr_vcd_add_signal(&vcd_file,
			avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), IOPORT_IRQ_PIN_ALL),
			8 /* bits */, "D7-D0");
	avr_vcd_add_signal(&vcd_file,
			avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), 4),
			1 /* bits */, "RS");
	avr_vcd_add_signal(&vcd_file,
			avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), 5),
			1 /* bits */, "E");
	avr_vcd_add_signal(&vcd_file,
			hd44780.irq + IRQ_HD44780_BUSY,
			1 /* bits */, "LCD_BUSY");
	avr_vcd_add_signal(&vcd_file,
			hd44780.irq + IRQ_HD44780_ADDR,
			7 /* bits */, "LCD_ADDR");
	avr_vcd_add_signal(&vcd_file,
			hd44780.irq + IRQ_HD44780_DATA_IN,
			8 /* bits */, "LCD_DATA_IN");
	avr_vcd_add_signal(&vcd_file,
			hd44780.irq + IRQ_HD44780_DATA_OUT,
			8 /* bits */, "LCD_DATA_OUT");
	avr_vcd_add_signal(&vcd_file,
			avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 0),
			1 /* bits */, "RED_LED");
	avr_vcd_add_signal(&vcd_file,
			avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 1),
			1 /* bits */, "GREEN_LED");
	avr_vcd_add_signal(&vcd_file,
			avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 2),
			1 /* bits */, "PB2");
	avr_vcd_start(&vcd_file);
	vcd_started = true;
}

static void setup_display(void)
{
	int w = 5 + hd44780.w * 6;
	int h = 5 + hd44780.h * 8 + 10;
	int pixsize = 3;

	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
	glutInitWindowSize(w * pixsize, h * pixsize);		/* width=400pixels height=500pixels */
	window = glutCreateWindow("Press 'q' to quit");	/* create window */

	initGL(w * pixsize, h * pixsize);
}

int main(int argc, char *argv[])
{
	const char * fname = "atmega8_firmware.axf";
	const char * wname = "wave1.txt";
	uint16_t gdb_port = 1234;
	bool use_gdb = false;
	pthread_t run;

	/*
	 * OpenGL init, happens before getopt as it can remove GL specific
	 * arguments from the opt strings
	 */
	glutInit(&argc, argv);

	/* Default scale: 2200mV */
	wave_mult = 2200.0;
	/* Defaukt offset: 2000mV */
	wave_offset = 2000.0;


	while(1) {
		static struct option long_opts[] = {
			{"wave",	required_argument,	NULL,	'w'},
			{"firmware",	required_argument,	NULL,	'f'},
			{"offset",	required_argument,	NULL,	'o'},
			{"scale",	required_argument,	NULL,	's'},
			{"gdb",		optional_argument,	NULL,	'g'},
			{"help",	no_argument,		NULL,	'h'},
		};
		int c, oidx = 0;

		c = getopt_long(argc, argv, "w:f:o:s:g::h", long_opts, &oidx);
		if (c == EOF)
			break;
		switch(c) {
		case 'w':
			wname = optarg;
			break;
		case 'f':
			fname = optarg;
			break;
		case 'o':
			wave_offset = (double)atoi(optarg);
			break;
		case 's':
			wave_mult =  (double)atoi(optarg);
			break;
		case 'g':
			use_gdb = true;
			if (optarg)
				gdb_port = strtoul(optarg, NULL, 0);
			break;
		case 'h':
			printf("Usage: %s [OPTIONS]...\n\n", argv[0]);
			printf("Run the lab board simulation\n\n");
			printf("Options:\n");
			printf("  -w, --wave=file.txt     specify wave text file\n");
			printf("  -f, --firmware=file.axf specify firmware file\n");
			printf("  -o, --offset=<value>    specify voltage offset in mV (default 2000)\n");
			printf("  -s, --scale=<value>     specify voltage scale in mV (default 2200)\n");
			printf("  -g, --gdb[=port]        enable GDB support on port \"port\" (default to 1234)\n");
			printf("  -h, --help              this help\n\n");
			printf("Additional standard GL/X11 options are supported\n\n");
		default:
			exit(1);
		}
	}

	printf("Wave adjustement: offset=%fmV, scale=%fmV\n", wave_offset, wave_mult);

	/* Init and setup AVR */
	create_avr(fname, use_gdb, gdb_port);

	/* Load wave */
	load_wave(wname);

	/* Setup LCD screen wiring */
	setup_lcd();

	/* Initialize wave sensor */
	setup_wave_sensor(0);

	/* Setup VCD recording of the signals */
	setup_vcd();

	printf( "Demo : This is the lab board simulation\n"
			"   You can configure the width&height of the LCD in the code\n"
			"   Press 'r' to start recording a 'vcd' file - with a LOT of data\n"
			"   Press 's' to stop recording\n"
			"   Press 'p' to play the wave file\n"
			);

	setup_display();

	if (use_gdb)
		printf("Program stopped, connect with GDB to port %d...\n",
		       gdb_port);

	pthread_create(&run, NULL, avr_run_thread, NULL);

	wave_start_cycle = avr->cycle;
	wave_playing = true;

	glutMainLoop();
}
