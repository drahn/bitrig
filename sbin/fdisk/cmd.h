/*	$OpenBSD: cmd.h,v 1.2 1997/09/29 23:33:33 mickey Exp $	*/

/*
 * Copyright (c) 1997 Tobias Weingartner
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Tobias Weingartner.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _CMD_H
#define _CMD_H

/* Includes */
#include "disk.h"
#include "mbr.h"


/* Constants (returned by cmd funs */
#define CMD_EXIT	0x0000
#define CMD_CONT	0x0001
#define CMD_CLEAN	0x0002
#define CMD_DIRTY	0x0003


/* Data types */
struct _cmd_table_t;
typedef struct _cmd_t {
	struct _cmd_table_t *table;
	char cmd[10];
	char args[100];
} cmd_t;

typedef struct _cmd_table_t {
	char *cmd;
	int (*fcn)(cmd_t *, disk_t *, mbr_t *, mbr_t *, int);
	char *help;
} cmd_table_t;


/* Prototypes */
int Xinit __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xdisk __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xedit __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xselect __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xprint __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xwrite __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xexit __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xquit __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xhelp __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xflag __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));
int Xupdate __P((cmd_t *, disk_t *, mbr_t *, mbr_t *, int));

#endif _CMD_H


