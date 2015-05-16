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

/* This code implements reading port pins via 1wire.
 */

#include <string.h> // memset

#include "moat_internal.h"
#include "dev_data.h"
#include "debug.h"
#include "onewire.h"
#include "port.h"

#ifdef N_PORT

uint8_t read_port_len(uint8_t chan)
{
	if(chan)
		return 1;
	else
		return (N_PORT+7)>>3;
}

void read_port(uint8_t chan, uint8_t *buf)
{
	port_t *portp;
	if (chan) { // one input: send flags, mark as scanned
		if (chan > N_PORT)
			next_idle('p');
		portp = &ports[chan-1];
		_P_VARS(portp)

		port_pre_send(portp);
		buf[0] = flg;
	} else { // all inputs: send bits
		uint8_t b=0,i,mask=1;
		portp = ports;

		for(i=0;i<N_PORT;i++) {
			if(portp->flags & PFLG_CURRENT)
				b |= mask;
			mask <<= 1;
			if (!mask) { // byte is full
				*buf++ = b;
				b=0; mask=1;
			}
			portp++;
		}
		if (mask != 1) // residue
			*buf = b;
	}
}

void read_port_done(uint8_t chan) {
	if (chan)
		port_post_send(&ports[chan-1]);
}

void write_port(uint16_t crc)
{
	uint8_t chan;
	uint8_t len, a,b;
	port_t *portp;
	chan = recv_byte_in();
	recv_byte();

	if (chan == 0 || chan > N_PORT)
		next_idle('p');
	portp = &ports[chan-1];
	_P_VARS(portp)

	crc = crc16(crc,chan);

	len = recv_byte_in();
	recv_byte();
	crc = crc16(crc,len);
	a = recv_byte_in();
	crc = crc16(crc,a);
	if(len == 2) { // set parameters (a:value; b:bitmask)
		recv_byte();
		b = recv_byte_in();
		crc = crc16(crc,b);
	} else if(len != 1) { // len=1: set value
		next_idle('q');
	}

	end_transmission(crc);
	if (len == 2) {
		flg = (portp->flags&~b) | (a&b);
		portp->flags = flg;
		if(b&PFLG_CURRENT)
			port_set(portp,a&0x80);
		else if (b&3)
			port_set_out(portp,flg&3);
	} else {
		port_set(portp,a);
	}
}

#ifdef CONDITIONAL_SEARCH

char alert_port_check(void)
{
	return (port_changed_cache+7)>>3;
}

void alert_port_fill(uint8_t *buf)
{
	uint8_t i;
	port_t *pp = ports;
	uint8_t m=1;

	memset(buf,0,(N_PORT+7)>>3);
	for(i=0;i < N_PORT; i++,pp++) {
		if (!m) {
			m=1;
			buf++;
		}
		if (pp->flags & (PFLG_CHANGED|PFLG_POLL) && pp->flags & PFLG_ALERT)
			*buf |= m;
		m <<= 1;
	}
}

#endif // conditional

#endif // N_PORT
