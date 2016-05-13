/*
 * sample LCD screen code
 *
 * Created: 22/05/2013 10:47:02
 *  Author: Nicolo
 *
 * PLEASE NOTE: the delays in this file are overlong.
 * You may need to adjust them in the code
 * you write for your application so as not to slow your system
 * down unnecessarily.
 */

#undef F_CPU
#define F_CPU 16000000UL

#include <stdint.h>
#include <stdio.h>
#include <avr/io.h>
#include <util/delay.h>

/* These bits are specific to building with simavr */
#ifdef SIMAVR_BUILD
#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega8");
AVR_MCU_VOLTAGES(5000,5000,5000);
#endif

int main(void)
{
	//setting up port direction on microprocessor
	DDRB=0x3;
	DDRC=0b00110000; //portc has RS on bit 4, and enable on bit 5
	DDRD=0xFF;
	_delay_ms(100); //wait a little bit of time for the LCD to power up properly.

	//initialisation of LCD screen
	PORTC=0b00100000; //enable is high, RS is low (I'm going to send a command)
	_delay_ms(2);
	PORTD=0b00011100; //command1: function set (5x7 dot format, 2 line mode, 8-bit data)
	_delay_ms(2);
	PORTC=0b00000000; //enable is low, RS stays low (it will execute the command now)
	_delay_ms(2);
	PORTC=0b00100000; //enable is high, RS is low (I'm going to send another command)
	_delay_ms(2);
	PORTD=0b11110000; //command2: display on / cursor (Blink ON, underline ON, Display ON - you can use different settings if you like)
	_delay_ms(2);
	PORTC=0b00000000; //enable is low,  RS stays low (it will execute the command now)
	_delay_ms(2);
	PORTC=0b00100000; //enable is high, RS is low (I'm going to send another command)
	_delay_ms(2);
	PORTD=0b01100000; //command 3: character entry mode with increment and display shift OFF
	_delay_ms(2);
	PORTC=0b00000000; //enable is low, RS stays low (it will execute the command now)
	_delay_ms(2);
	PORTC=0b00100000; //enable is high, RS is low (I'm going to send another command)
	_delay_ms(2);
	PORTD=0b10000000; //command 4: clear screen
	_delay_ms(2);
	PORTC=0b00000000; //enable is low
	_delay_ms(2);
	PORTC=0b00100000; //enable is high
	_delay_ms(2);
	PORTD=0b01000000; //command 5 (take display cursor home)
	_delay_ms(2);
	PORTC=0b00000000; //enable is low
	_delay_ms(2);
	PORTC=0b00100000; //enable is high
	
	_delay_ms(100); //wait a little longer after initialisation (may not be required, but it seems to help)
	
	//now i am going to enter a real character
	PORTC=0b00110000; //enable is high, with RS high (I'm going to send data)
	_delay_ms(5);
	PORTD=0b00010010; //Send character 'H' (0b01001000)
	_delay_ms(5);
	PORTC=0b00010000; //enable is low, with RS high
	_delay_ms(5);
	PORTC=0b00110000; //enable is high, with RS high (ready to send more data)
	//and another one
	_delay_ms(5);
	PORTD=0b10010110; //Send character 'i' (0b01101001)
	_delay_ms(5);
	PORTC=0b00010000; //enable is low, with RS high
	_delay_ms(5);
	PORTC=0b00110000; //enable is high, with RS high (ready to send more data)
	//and another one
	_delay_ms(5);
	PORTD=0b10000100; //Send character '!' (0b00100001)
	_delay_ms(5);
	PORTC=0b00010000; //enable is low, with RS high
	_delay_ms(5);
	PORTC=0b00110000; //enable is high, with RS high (ready to send more data)
	//LCD should say "Hi!"

	PORTB=0;
	while(1) {
		PORTB=1;
		_delay_ms(1000);
		PORTB=2;
		_delay_ms(1000);
	}
}
