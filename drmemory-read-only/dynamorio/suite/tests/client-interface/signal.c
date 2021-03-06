/* **********************************************************
 * Copyright (c) 2009-2010 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * API regression test for signal event
 */

#include "tools.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <assert.h>
#include <setjmp.h>

#ifdef WINDOWS
# define NOP __nop()
#else /* LINUX */
# define NOP asm("nop")
#endif

static jmp_buf mark;
static int bar;

static void foo(void)
{ /* nothing: just a marker */ }

static void
redirect_target(void)
{
    /* use 2 NOPs + call so client can locate this spot (since we don't
     * have dr_get_proc_address() on linux yet)
     */
    NOP; NOP;
    foo();

    print("Redirected\n");
    longjmp(mark, 1);
}

/* handler with SA_SIGINFO flag set gets three arguments: */
typedef void (*handler_t)(int, struct siginfo *, void *);

static void
signal_handler(int sig, siginfo_t *siginfo, ucontext_t *ucxt)
{
    if (sig == SIGUSR1)
	print("Got SIGUSR1\n");
    else if (sig == SIGUSR2)
	print("Got SIGUSR2\n");
    else if (sig == SIGURG)
	print("Got SIGURG\n");
    else if (sig == SIGSEGV)
	print("Got SIGSEGV\n");
}

/* set up signal_handler as the handler for signal "sig" */
static void
intercept_signal(int sig, handler_t handler)
{
    int rc;
    struct sigaction act;
    act.sa_sigaction = handler;
    rc = sigfillset(&act.sa_mask); /* block all signals within handler */
    assert(rc == 0);
    act.sa_flags = SA_SIGINFO | SA_ONSTACK; /* send 3 args to handler */
    /* arm the signal */
    rc = sigaction(sig, &act, NULL);
    assert(rc == 0);
}

static void
unintercept_signal(int sig)
{
    int rc;
    struct sigaction act;
    act.sa_sigaction = (handler_t) SIG_DFL;
    /* disarm the signal */
    rc = sigaction(sig, &act, NULL);
    assert(rc == 0);
}

int
main(int argc, char** argv)
{
    intercept_signal(SIGUSR1, (handler_t) signal_handler);
    intercept_signal(SIGUSR2, (handler_t) signal_handler);
    intercept_signal(SIGURG, (handler_t) signal_handler);
    intercept_signal(SIGSEGV, (handler_t) signal_handler);

    print("Sending SIGURG\n");
    kill(getpid(), SIGURG);
    print("Sending SIGURG\n");
    kill(getpid(), SIGURG);
    print("Sending SIGURG\n");
    kill(getpid(), SIGURG);

    unintercept_signal(SIGURG);

    print("Sending SIGURG\n");
    kill(getpid(), SIGURG);
    print("Sending SIGURG\n");
    kill(getpid(), SIGURG);
    print("Sending SIGURG\n");
    kill(getpid(), SIGURG);

    print("Sending SIGTERM\n");
    kill(getpid(), SIGTERM);

    if (setjmp(mark) == 0) {
        /* execute so that client sees the spot */
        redirect_target();
    }
    if (setjmp(mark) == 0) {
        print("Sending SIGUSR2\n");
        kill(getpid(), SIGUSR2);
    }

    /* generate SIGSEGV; we'll re-crash post-handler unless client
     * modifies mcontext
     */
#ifdef X64
# define EAX "%rax"
# define ECX "%rcx"
#else
# define EAX "%eax"
# define ECX "%ecx"
#endif
    asm("push "EAX);
    asm("push "ECX);
    asm("mov %0, %"ECX"" : : "g"(&bar)); /* ok place to read from */
    asm("mov $0, "EAX);
    asm("mov ("EAX"), "EAX);
    asm("pop "ECX);
    asm("pop "EAX);

    print("Sending SIGUSR1\n");
    kill(getpid(), SIGUSR1);

    print("Done\n");
}
