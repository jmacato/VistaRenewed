#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef int HANDLE;
#define INVALID_HANDLE_VALUE (-1)
#define GENERIC_WRITE 1
#define FILE_SHARE_READ 2
#define FILE_SHARE_WRITE 4
#define OPEN_ALWAYS 8
#define FILE_ATTRIBUTE_NORMAL 16
#define FILE_END 2
static unsigned opens, writes, debug_calls;
static int verbose_env;
static LONG InterlockedCompareExchange(LONG *p,LONG value,LONG expected) {
    LONG old=*p; if(old==expected)*p=value; return old;
}
static LONG InterlockedIncrement(LONG *p) { return ++*p; }
static DWORD GetEnvironmentVariableA(const char *name,char *value,DWORD size) {
    assert(!strcmp(name,"TRITON9_VERBOSE_DDI") && size==2);
    value[0]=verbose_env?'1':0; value[1]=0; return verbose_env?1:0;
}
static void OutputDebugStringA(const char *s) { assert(s); debug_calls++; }
#define CreateFileA(...) (++opens, 1)
#define SetFilePointer(...) ((void)0)
#define WriteFile(...) (++writes)
#define CloseHandle(h) ((void)(h))
/* Macro stubs deliberately do not evaluate every Win32 argument. */
#pragma clang diagnostic ignored "-Wunused-variable"
/* SOURCE_UNDER_TEST */
int main(void) {
    triton9Diag(NULL);
    for(unsigned i=0;i<100000;i++)triton9Diag("TRITON9-PRESENT success\n");
    assert(opens==128 && writes==128 && debug_calls==128);
    triton9Diag("TRITON9-PRESENT fail\n");
    triton9Diag("TRITON9-FAULT-PC\n");
    triton9Diag("TRITON9-PRESENT-REJECT=123\n");
    triton9Diag("TRITON9-UNSUPPORTED\n");
    assert(writes==132 && opens==132);
    verbose_env=1;
    for(unsigned i=0;i<1000;i++)triton9DiagVerbose("TRITON9-PRESENT success\n");
    assert(writes==1132 && opens==1132);
    puts("PASS routine trace I/O bounded; failures preserved; verbose override");
}
