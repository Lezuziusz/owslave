/*
 *  Copyright © 2014-2015, Matthias Urlichs <matthias@urlichs.de>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License (included; see the file LICENSE)
 *  for more details.
 */

/* This code implements the main code of MoaT slaves.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <string.h>

#include "onewire.h"
#include "features.h"
#include "moat.h"
#include "dev_data.h"
#include "debug.h"
#include "moat_internal.h"
#include "port.h"
#include "pwm.h"
#include "count.h"
#include "console.h"
#include "timer.h"

#define _1W_READ_GENERIC  0xF2
#define _1W_WRITE_GENERIC 0xF4

const uint8_t moat_sizes[] __attribute__ ((progmem)) = {
#include "_nums.h"
};

uint8_t dummy_read_len_fn(uint8_t chan) { next_idle('y'); return 0; }
void dummy_read_fn(uint8_t chan, uint8_t *buf) { next_idle('y'); }
void dummy_read_done_fn(uint8_t chan) {}
void dummy_write_fn(uint16_t crc) { next_idle('y'); }
char dummy_alert_check_fn(void) { return 0; }
void dummy_alert_fill_fn(uint8_t *buf) { next_idle('y'); }

#define TC_DEFINE(_s) \
    read_len_fn read_ ## _s ## _len __attribute__((weak,alias("dummy_read_len_fn"))); \
    read_fn read_ ## _s __attribute__((weak,alias("dummy_read_fn"))); \
    read_done_fn read_ ## _s ## _done __attribute__((weak,alias("dummy_read_done_fn"))); \
    write_fn write_ ## _s __attribute__((weak,alias("dummy_write_fn")));  \
	ALERT_DEF(_s)
#ifdef CONDITIONAL_SEARCH
#define ALERT_DEF(_s) \
    alert_check_fn alert_ ## _s ## _check __attribute__((weak,alias("dummy_alert_check_fn"))); \
    alert_fill_fn alert_ ## _s ## _fill __attribute__((weak,alias("dummy_alert_fill_fn")));
#else
#define ALERT_DEF(x) // nothing
#endif
#include "_def.h"
#undef ALERT_DEF
#undef TC_DEFINE

#define TC_DEFINE(_s) \
{ \
    &read_ ## _s ## _len, \
    &read_ ## _s, \
    &read_ ## _s ## _done, \
    &write_ ## _s, \
	ALERT_DEF(_s) \
},
#ifdef CONDITIONAL_SEARCH
#define ALERT_DEF(_s) \
    &alert_ ## _s ## _check, \
    &alert_ ## _s ## _fill, 
#else
#define ALERT_DEF(x) // nothing
#endif
const moat_call_t moat_calls[TC_MAX] __attribute__((progmem)) = {
#include "_def.h"
};
#undef ALERT_DEF
#undef TC_DEFINE

void end_transmission(uint16_t crc)
{
	crc = ~crc;
	xmit_byte(crc);
	xmit_byte(crc >> 8);
	{
		uint16_t icrc;
		recv_byte();
		icrc = recv_byte_in();
		recv_byte();
		icrc |= recv_byte_in() << 8;
		if (icrc != ~crc) {
			DBG_P(" crc=");
			DBG_W(crc);
			DBG_P(" icrc=");
			DBG_W(icrc);
			DBG_C(' ');
			next_idle('c');
		}
		// DBG_P("CRC OK ");
	}
}

static void moat_read(void)
{
	uint16_t crc = 0;
	uint8_t dtype,chan;
	static uint8_t buf[MAXBUF];
	uint8_t len;
	uint8_t *bp=buf;

	/*
	 Implement reading data. We read whatever necessary, write the length,
	 write the data, write the CRC, read the inverted CRC back, and then do
	 whatever necessary to effect the read (e.g. clear a flag, update a
	 stored value, whatever).

	 Typically there's some free time between reading, writing the length,
	 and writing the data respectively. CRC read is immediate, so we
	 pre-calculate as much as possible and add the rest on the go.

	 Do not forget that bus errors and whatnot can abort this code in any
	 recv/xmit call.
	 */
	
	recv_byte();
	crc = crc16(crc,_1W_READ_GENERIC);
	dtype = recv_byte_in();
	recv_byte();
	crc = crc16(crc,dtype);
	chan = recv_byte_in();

	switch(dtype) {
	case TC_CONFIG:  len = read_config_len(chan); break;
	case TC_CONSOLE: len = read_console_len(chan); break;
	case TC_PORT:    len = read_port_len(chan); break;
	case TC_PWM:     len = read_pwm_len(chan); break;
	case TC_COUNT:   len = read_count_len(chan); break;
	case TC_ADC:     len = read_adc_len(chan); break;
	default: DBG_C('?');DBG_C('-'); DBG_X(dtype); next_idle('-');
	}
	xmit_byte(len);

	crc = crc16(crc,chan);
	crc = crc16(crc,len);

	switch(dtype) {
	case TC_CONFIG:  read_config(chan, buf); break;
	case TC_CONSOLE: read_console(chan, buf); break;
	case TC_PORT:    read_port(chan, buf); break;
	case TC_PWM:     read_pwm(chan, buf); break;
	case TC_COUNT:   read_count(chan, buf); break;
	case TC_ADC:     read_adc(chan, buf); break;
	default: break; // cannot happen
	}

	while(len--) {
		xmit_byte(*bp);
		crc = crc16(crc,*bp);
		bp++;
	}
	end_transmission(crc);
	switch(dtype) {
	case TC_CONSOLE: read_console_done(chan); break;
	case TC_PORT:    read_port_done(chan); break;
	default: break;
	}
}

void moat_write(void) {
	uint16_t crc = 0;
	uint8_t dtype;

	/*
	 Implement reading data. We read whatever necessary, read the length,
	 read the data, write the resulting CRC, read the inverted CRC back,
	 and then do whatever necessary to effect the write (e.g. clear a flag,
	 update a stored value, whatever).

	 Do not forget that bus errors and whatnot can abort this code in any
	 recv/xmit call.
	 */
	
	recv_byte();
	crc = crc16(crc,_1W_WRITE_GENERIC);
	dtype = recv_byte_in();
	//DBG_C('W'); DBG_X(dtype);
	recv_byte();
	crc = crc16(crc,dtype);

	switch(dtype) {
	case TC_CONSOLE: write_console(crc); break;
	case TC_PORT: write_port(crc); break;
	case TC_PWM: write_pwm(crc); break;
	case TC_ADC: write_adc(crc); break;
	default: DBG_C('?'); return;
	}
}

void do_command(uint8_t cmd)
{
	if(cmd == _1W_READ_GENERIC) {
		//DBG_P(":I");
		moat_read();
	} else if(cmd == _1W_WRITE_GENERIC) {
		//DBG_P(":I");
		moat_write();
	} else {
		DBG(0x0E);
		DBG_P("?CI ");
		DBG_X(cmd);
		set_idle();
	}
}

void update_idle(uint8_t bits)
{
}

#if CONSOLE_PING
timer_t t;
#endif
void init_state(void)
{
#if CONSOLE_PING
	timer_start(CONSOLE_PING,&t);
#endif
}

#ifdef CONDITIONAL_SEARCH
char condition_seen;
uint8_t condition_met(void) {
	return condition_seen;
}
#endif

void mainloop(void) {
	DBG(0x1E);
#ifdef CONDITIONAL_SEARCH
	condition_seen = (
		console_alert() ||
		port_alert() ||
		count_alert() ||
		0);
#endif
#if CONSOLE_PING
	if(timer_done(&t)) {
		console_putc('!');
		timer_start(CONSOLE_PING,&t);
	}
#endif
}
