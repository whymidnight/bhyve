/*-
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/time.h>

#include <machine/clock.h>

#include <stdio.h>
#include <assert.h>

#include "fbsdrun.h"
#include "inout.h"
#include "pit_8254.h"

#define	TIMER_SEL_MASK		0xc0
#define	TIMER_RW_MASK		0x30
#define	TIMER_MODE_MASK		0x0f
#define	TIMER_SEL_READBACK	0xc0

#define	TIMER_DIV(freq, hz)	(((freq) + (hz) / 2) / (hz))

#define	PIT_8254_FREQ		1193182
static const int nsecs_per_tick = 1000000000 / PIT_8254_FREQ;

struct counter {
	struct timeval	tv;		/* uptime when counter was loaded */
	uint16_t	initial;	/* initial counter value */
	uint8_t		cr[2];
	uint8_t		ol[2];
	int		crbyte;
	int		olbyte;
};

static void
timevalfix(struct timeval *t1)
{

	if (t1->tv_usec < 0) {
		t1->tv_sec--;
		t1->tv_usec += 1000000;
	}
	if (t1->tv_usec >= 1000000) {
		t1->tv_sec++;
		t1->tv_usec -= 1000000;
	}
}

static void
timevalsub(struct timeval *t1, const struct timeval *t2)
{

	t1->tv_sec -= t2->tv_sec;
	t1->tv_usec -= t2->tv_usec;
	timevalfix(t1);
}

static void
latch(struct counter *c)
{
	struct timeval tv2;
	uint16_t lval;
	uint64_t delta_nsecs, delta_ticks;

	/* cannot latch a new value until the old one has been consumed */
	if (c->olbyte != 0)
		return;

	if (c->initial == 0 || c->initial == 1) {
		/*
		 * XXX the program that runs the VM can be stopped and
		 * restarted at any time. This means that state that was
		 * created by the guest is destroyed between invocations
		 * of the program.
		 *
		 * If the counter's initial value is not programmed we
		 * assume a value that would be set to generate 'guest_hz'
		 * interrupts per second.
		 */
		c->initial = TIMER_DIV(PIT_8254_FREQ, guest_hz);
		gettimeofday(&c->tv, NULL);
	}
	
	(void)gettimeofday(&tv2, NULL);
	timevalsub(&tv2, &c->tv);
	delta_nsecs = tv2.tv_sec * 1000000000 + tv2.tv_usec * 1000;
	delta_ticks = delta_nsecs / nsecs_per_tick;

	lval = c->initial - delta_ticks % c->initial;
	c->olbyte = 2;
	c->ol[1] = lval;		/* LSB */
	c->ol[0] = lval >> 8;		/* MSB */
}

static int
pit_8254_handler(struct vmctx *ctx, int vcpu, int in, int port, int bytes,
		 uint32_t *eax, void *arg)
{
	int sel, rw, mode;
	uint8_t val;
	struct counter *c;

	static struct counter counter[3];

	if (bytes != 1)
		return (-1);

	val = *eax;

	if (port == TIMER_MODE) {
		assert(in == 0);
		sel = val & TIMER_SEL_MASK;
		rw = val & TIMER_RW_MASK;
		mode = val & TIMER_MODE_MASK;

		if (sel == TIMER_SEL_READBACK)
			return (-1);
		if (rw != TIMER_LATCH && rw != TIMER_16BIT)
			return (-1);

		if (rw != TIMER_LATCH) {
			/*
			 * Counter mode is not affected when issuing a
			 * latch command.
			 */
			if (mode != TIMER_RATEGEN && mode != TIMER_SQWAVE)
				return (-1);
		}
		
		c = &counter[sel >> 6];
		if (rw == TIMER_LATCH)
			latch(c);
		else
			c->olbyte = 0;	/* reset latch after reprogramming */
		
		return (0);
	}

	/* counter ports */
	assert(port >= TIMER_CNTR0 && port <= TIMER_CNTR2);
	c = &counter[port - TIMER_CNTR0];

	if (in) {
		/*
		 * XXX
		 * The spec says that once the output latch is completely
		 * read it should revert to "following" the counter. We don't
		 * do this because it is hard and any reasonable OS should
		 * always latch the counter before trying to read it.
		 */
		if (c->olbyte == 0)
			c->olbyte = 2;
		*eax = c->ol[--c->olbyte];
	} else {
		c->cr[c->crbyte++] = *eax;
		if (c->crbyte == 2) {
			c->crbyte = 0;
			c->initial = c->cr[0] | (uint16_t)c->cr[1] << 8;
			if (c->initial == 0)
				c->initial = 0xffff;
			gettimeofday(&c->tv, NULL);
		}
	}

	return (0);
}

INOUT_PORT(8254, TIMER_MODE, IOPORT_F_OUT, pit_8254_handler);
INOUT_PORT(8254, TIMER_CNTR0, IOPORT_F_INOUT, pit_8254_handler);
INOUT_PORT(8254, TIMER_CNTR1, IOPORT_F_INOUT, pit_8254_handler);
INOUT_PORT(8254, TIMER_CNTR2, IOPORT_F_INOUT, pit_8254_handler);
