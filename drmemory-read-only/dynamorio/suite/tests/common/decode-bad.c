/* **********************************************************
 * Copyright (c) 2003-2010 VMware, Inc.  All rights reserved.
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

#ifndef ASM_CODE_ONLY /* C code */
#include "tools.h" /* for print() */
#include <assert.h>
#include <stdio.h>
#include <math.h>

#ifdef LINUX
# include <unistd.h>
# include <signal.h>
# include <ucontext.h>
# include <errno.h>
# include <stdlib.h>
#endif

#include <setjmp.h>

/* asm routines */
void test_priv_0(void);
void test_priv_1(void);
void test_priv_2(void);
void test_priv_3(void);
void test_prefix_0(void);
void test_prefix_1(void);
void test_inval_0(void);
void test_inval_1(void);
void test_inval_2(void);
void test_inval_3(void);
void test_inval_4(void);
void test_inval_5(void);
void test_inval_6(void);
void test_inval_7(void);

jmp_buf mark;
static int count = 0;
static bool invalid_lock;

/* just use single-arg handlers */
typedef void (*handler_t)(int);
typedef void (*handler_3_t)(int, struct siginfo *, void *);

#ifdef USE_DYNAMO
#include "dynamorio.h"
#endif

#ifdef LINUX
static void
signal_handler(int sig)
{
    if (sig == SIGILL) {
        count++;
        if (invalid_lock) {
            print("Invalid lock sequence, instance %d\n", count);
            /* add this so output matches test on windows, FIXME : would like to test
             * this on linux too, but won't work now (bug 651, 832) */
            print("eax=1 ebx=2 ecx=3 edx=4 edi=5 esi=6 ebp=7\n");
        } else 
            print("Bad instruction, instance %d\n", count);
        longjmp(mark, count);
    }
    if (sig == SIGSEGV) {
        count++;
        /* We can't distinguish but this is the only segv we expect */
        print("Privileged instruction, instance %d\n", count);
        longjmp(mark, count);
    }
    exit(-1);
}

#define ASSERT_NOERR(rc) do {                                   \
  if (rc) {                                                     \
     fprintf(stderr, "%s:%d rc=%d errno=%d %s\n",               \
             __FILE__, __LINE__,                                \
             rc, errno, strerror(errno));                       \
  }                                                             \
} while (0);

/* set up signal_handler as the handler for signal "sig" */
static void
intercept_signal(int sig, handler_t handler)
{
    int rc;
    struct sigaction act;

    act.sa_sigaction = (handler_3_t) handler;
    /* FIXME: due to DR bug 840 we cannot block ourself in the handler
     * since the handler does not end in a sigreturn, so we have an empty mask
     * and we use SA_NOMASK
     */
    rc = sigemptyset(&act.sa_mask); /* block no signals within handler */
    ASSERT_NOERR(rc);
    /* FIXME: due to DR bug #654 we use SA_SIGINFO -- change it once DR works */
    act.sa_flags = SA_NOMASK | SA_SIGINFO | SA_ONSTACK;
    
    /* arm the signal */
    rc = sigaction(sig, &act, NULL);
    ASSERT_NOERR(rc);
}

#else
/* sort of a hack to avoid the MessageBox of the unhandled exception spoiling
 * our batch runs
 */
# include <windows.h>
/* top-level exception handler */
static LONG
our_top_handler(struct _EXCEPTION_POINTERS * pExceptionInfo)
{
    if (pExceptionInfo->ExceptionRecord->ExceptionCode ==
        /* Windows doesn't have an EXCEPTION_ constant to equal the invalid lock code */
        0xc000001e/*STATUS_INVALID_LOCK_SEQUENCE*/
        /* FIXME: DR doesn't generate the invalid lock exception */
        || (invalid_lock && pExceptionInfo->ExceptionRecord->ExceptionCode ==
            STATUS_ILLEGAL_INSTRUCTION)) {
        CONTEXT *cxt = pExceptionInfo->ContextRecord;
        count++;
        print("Invalid lock sequence, instance %d\n", count);
        /* FIXME : add CXT_XFLAGS (currently comes back incorrect), eip, esp? */
        print("eax="SZFMT" ebx="SZFMT" ecx="SZFMT" edx="SZFMT" "
              "edi="SZFMT" esi="SZFMT" ebp="SZFMT"\n", 
              cxt->CXT_XAX, cxt->CXT_XBX, cxt->CXT_XCX, cxt->CXT_XDX,
              cxt->CXT_XDI, cxt->CXT_XSI, cxt->CXT_XBP);
        longjmp(mark, count);
    }
    if (pExceptionInfo->ExceptionRecord->ExceptionCode == STATUS_ILLEGAL_INSTRUCTION) {
        count++;
        print("Bad instruction, instance %d\n", count);
        longjmp(mark, count);
    }
    if (pExceptionInfo->ExceptionRecord->ExceptionCode ==
        STATUS_PRIVILEGED_INSTRUCTION) {
        count++;
        print("Privileged instruction, instance %d\n", count);
        longjmp(mark, count);
    }
    /* Shouldn't get here in normal operation so this isn't #if VERBOSE */
    print("Exception 0x"PFMT" occurred, process about to die silently\n",
          pExceptionInfo->ExceptionRecord->ExceptionCode);
    if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        print("\tPC "PFX" tried to %s address "PFX"\n",
            pExceptionInfo->ExceptionRecord->ExceptionAddress, 
            (pExceptionInfo->ExceptionRecord->ExceptionInformation[0]==0)?"read":"write",
            pExceptionInfo->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_EXECUTE_HANDLER; /* => global unwind and silent death */
}
#endif

int main(int argc, char *argv[])
{
    double res = 0.;
    int i;

#ifdef USE_DYNAMO
    dynamorio_app_init();
    dynamorio_app_start();
#endif
  
#ifdef LINUX
    intercept_signal(SIGILL, signal_handler);
    intercept_signal(SIGSEGV, signal_handler);
#else
# ifdef X64_DEBUGGER
    /* FIXME: the vectored handler works fine in the debugger, but natively
     * the app crashes here: yet the SetUnhandled hits infinite fault loops
     * in the debugger, and works fine natively!
     */
    AddVectoredExceptionHandler(1/*first*/,
                                (PVECTORED_EXCEPTION_HANDLER) our_top_handler);
# else
    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER) our_top_handler);
# endif
#endif

    /* privileged instructions */
    print("Privileged instructions about to happen\n");
    count = 0;
    i = setjmp(mark);
    switch (i) {
    case 0: test_priv_0();
    case 1: test_priv_1();
    case 2: test_priv_2();
    case 3: test_priv_3();
    }

    /* prefix tests */
    print("OK instr about to happen\n");
    /* multiple prefixes */
    /* FIXME: actually these prefixes on a jmp are "reserved" but this seems to work */
    test_prefix_0();
    print("Bad instr about to happen\n");
    /* lock prefix, which is illegal instruction if placed on jmp */
    count = 0;
    invalid_lock = true;
    if (setjmp(mark) == 0) {
        test_prefix_1();
    }
    invalid_lock = false;

    print("Invalid instructions about to happen\n");
    count = 0;
    i = setjmp(mark);
    switch (i) {
        /* note that we decode until a CTI, so for every case the suffix is decoded
         * and changes in later cases may fail even the earlier ones.
         */
    case 0: test_inval_0();
    case 1: test_inval_1();
    case 2: test_inval_2();
    case 3: test_inval_3();
    case 4: test_inval_4();
    case 5: test_inval_5();
    case 6: test_inval_6();
    case 7: test_inval_7();
    default: ;
    }

    print("All done\n");

#ifdef USE_DYNAMO
    dynamorio_app_stop();
    dynamorio_app_exit();
#endif
    return 0;
}

#else /* asm code *************************************************************/
#include "asm_defines.asm"
START_FILE

#define FUNCNAME test_priv_0
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        mov  REG_XAX, dr0
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_priv_1
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        mov  dr7, REG_XAX
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_priv_2
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        mov  REG_XAX, cr0
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_priv_3
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        mov  cr3, REG_XAX
        ret
        END_FUNC(FUNCNAME)

#undef FUNCNAME
#define FUNCNAME test_prefix_0
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
# ifdef X64
        /* avoid ASSERT "no support yet for application using non-NPTL segment" */
        RAW(64) RAW(f2) RAW(f3) RAW(eb) RAW(00)
# else
        RAW(65) RAW(f2) RAW(f3) RAW(eb) RAW(00)
# endif
        ret
        END_FUNC(FUNCNAME)

#undef FUNCNAME
#define FUNCNAME test_prefix_1
       /* If we make this a leaf function (no SEH directives), our top-level handler
        * does not get called!  Annoying.
        */
        DECLARE_FUNC_SEH(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        /* push callee-saved registers */
        PUSH_SEH(REG_XBX)
        PUSH_SEH(REG_XBP)
        PUSH_SEH(REG_XSI)
        PUSH_SEH(REG_XDI)
        END_PROLOG
        mov  eax, 1
        mov  ebx, 2
        mov  ecx, 3
        mov  edx, 4
        mov  edi, 5
        mov  esi, 6
        mov  ebp, 7
        RAW(f0) RAW(eb) RAW(00)
        add      REG_XSP, 0 /* make a legal SEH64 epilog */
        pop      REG_XDI
        pop      REG_XSI
        pop      REG_XBP
        pop      REG_XBX
        ret
        END_FUNC(FUNCNAME)

#undef FUNCNAME
#define FUNCNAME test_inval_0
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        RAW(df) RAW(fa)
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_inval_1
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        RAW(0f) RAW(04)
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_inval_2
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        RAW(fe) RAW(30)
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_inval_3
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        RAW(ff) RAW(38)
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_inval_4
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        RAW(f3) RAW(0f) RAW(13)
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_inval_5
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        /* case 8840 we crash after going through this bb */
        /* ud2 */
        RAW(0f) RAW(0b)
        RAW(20) RAW(0f) 
#if 0
        RAW(8c) RAW(c6) 
#endif
        RAW(ff) RAW(ff) 
        RAW(ff) RAW(d9)
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_inval_6
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
        /* case 6962 - invalid Mod byte for a call far,
         * so should get #UD as well
         * we should make sure we either mark invalid during decode
         * or properly detect it is invalid during execution
         */
        /* we just have to force ecx = 0x13 to make sure the same result */
        RAW(ff) RAW(d9)
        ret
        END_FUNC(FUNCNAME)
#undef FUNCNAME
#define FUNCNAME test_inval_7
        DECLARE_FUNC(FUNCNAME)
GLOBAL_LABEL(FUNCNAME:)
         /* Although data16 means it is 4 bytes and fits in
         * a register, this is invalid.
         */
        RAW(66) RAW(ff) RAW(d9)
        ret
        END_FUNC(FUNCNAME)

END_FILE
#endif
