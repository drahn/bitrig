/*
 * util/random.c - thread safe random generator, which is reasonably secure.
 * 
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 * 
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 * Thread safe random functions. Similar to arc4random() with an explicit
 * initialisation routine.
 *
 * The code in this file is based on arc4random from
 * openssh-4.0p1/openbsd-compat/bsd-arc4random.c
 * That code is also BSD licensed. Here is their statement:
 *
 * Copyright (c) 1996, David Mazieres <dm@uun.org>
 * Copyright (c) 2008, Damien Miller <djm@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"
#include "util/random.h"
#include "util/log.h"
#include <time.h>

/** 
 * Max random value.  Similar to RAND_MAX, but more portable
 * (mingw uses only 15 bits random).
 */
#define MAX_VALUE 0x7fffffff

#if defined(HAVE_SSL)

void
ub_systemseed(unsigned int seed)
{
}

struct ub_randstate* 
ub_initstate(unsigned int seed, struct ub_randstate* from)
{
	struct ub_randstate* s = (struct ub_randstate*)malloc(0);
	if(!s) {
		log_err("malloc failure in random init");
		return NULL;
	}
	return s;
}

long int 
ub_random(struct ub_randstate* s)
{
	/* This relies on MAX_VALUE being 0x7fffffff. */
	return (long)arc4random() & MAX_VALUE;
}

#elif defined(HAVE_NSS)

/* not much to remember for NSS since we use its pk11_random, placeholder */
struct ub_randstate {
	int ready;
};

void ub_systemseed(unsigned int ATTR_UNUSED(seed))
{
}

struct ub_randstate* ub_initstate(unsigned int ATTR_UNUSED(seed), 
	struct ub_randstate* ATTR_UNUSED(from))
{
	struct ub_randstate* s = (struct ub_randstate*)calloc(1, sizeof(*s));
	if(!s) {
		log_err("malloc failure in random init");
		return NULL;
	}
	return s;
}

long int ub_random(struct ub_randstate* ATTR_UNUSED(state))
{
	long int x;
	/* random 31 bit value. */
	SECStatus s = PK11_GenerateRandom((unsigned char*)&x, (int)sizeof(x));
	if(s != SECSuccess) {
		log_err("PK11_GenerateRandom error: %s",
			PORT_ErrorToString(PORT_GetError()));
	}
	return x & MAX_VALUE;
}

#endif /* HAVE_SSL or HAVE_NSS */

long int
ub_random_max(struct ub_randstate* state, long int x)
{
	return (long)arc4random_uniform(x);
}

void 
ub_randfree(struct ub_randstate* s)
{
	if(s)
		free(s);
}
