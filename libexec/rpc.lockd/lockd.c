/*
 * Copyright (c) 1995
 *	A.R. Gordon (andrew.gordon@net-tel.co.uk).  All rights reserved.
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
 *	This product includes software developed for the FreeBSD project
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ANDREW GORDON AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */


/* main() function for NFS lock daemon.  Most of the code in this	*/
/* file was generated by running rpcgen /usr/include/rpcsvc/nlm_prot.x	*/
/* The actual program logic is in the file procs.c			*/

#include "lockd.h"

extern void nlm_prog_1 __P((struct svc_req, register SVCXPRT));
extern void nlm_prog_3 __P((struct svc_req, register SVCXPRT));

int debug_level = 0;	/* Zero means no debugging syslog() calls	*/

#if 1
int _rpcsvcdirty;
#endif

main(int argc, char **argv)
{
  SVCXPRT *transp;

  if (argc > 1)
  {
    if (strncmp(argv[1], "-d", 2))
    {
      fprintf(stderr, "Usage: rpc.lockd [-d [<debuglevel>]]\n");
      exit(1);
    }
    if (argc > 2) debug_level = atoi(argv[2]);
    else debug_level = atoi(argv[1] + 2);
    /* Ensure at least some debug if -d with no specified level		*/
    if (!debug_level) debug_level = 1;
  }

  (void)pmap_unset(NLM_PROG, NLM_VERS);
  (void)pmap_unset(NLM_PROG, NLM_VERSX);

  transp = svcudp_create(RPC_ANYSOCK);
  if (transp == NULL)
  {
    (void)fprintf(stderr, "cannot create udp service.\n");
    exit(1);
  }
  if (!svc_register(transp, NLM_PROG, NLM_VERS, (void (*)())nlm_prog_1, IPPROTO_UDP))
  {
    (void)fprintf(stderr, "unable to register (NLM_PROG, NLM_VERS, udp).\n");
    exit(1);
  }
  if (!svc_register(transp, NLM_PROG, NLM_VERSX, (void (*)())nlm_prog_3, IPPROTO_UDP))
  {
    (void)fprintf(stderr, "unable to register (NLM_PROG, NLM_VERSX, udp).\n");
    exit(1);
  }

  transp = svctcp_create(RPC_ANYSOCK, 0, 0);
  if (transp == NULL)
  {
    (void)fprintf(stderr, "cannot create tcp service.\n");
    exit(1);
  }
  if (!svc_register(transp, NLM_PROG, NLM_VERS, (void (*)())nlm_prog_1, IPPROTO_TCP))
  {
    (void)fprintf(stderr, "unable to register (NLM_PROG, NLM_VERS, tcp).\n");
    exit(1);
  }
  if (!svc_register(transp, NLM_PROG, NLM_VERSX, (void (*)())nlm_prog_3, IPPROTO_TCP))
  {
    (void)fprintf(stderr, "unable to register (NLM_PROG, NLM_VERSX, tcp).\n");
    exit(1);
  }

  /* Note that it is NOT sensible to run this program from inetd - the 	*/
  /* protocol assumes that it will run immediately at boot time.	*/
  if (daemon(0,0)) {
    perror("cannot fork");
    exit(1);
  }
  openlog("rpc.lockd", 0, LOG_DAEMON);
  if (debug_level) syslog(LOG_INFO, "Starting, debug level %d", debug_level);
  else syslog(LOG_INFO, "Starting");

  svc_run();	/* Should never return					*/
  exit(1);
}
