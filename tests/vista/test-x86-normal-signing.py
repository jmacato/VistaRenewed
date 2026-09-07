from pathlib import Path
import subprocess,tempfile
p=(Path(__file__).resolve().parents[2]/'packaging/vista-driver-deploy-service.c');s=p.read_text();a=s.index('static BOOL configure_boot_integrity(void)');b=s.index('static BOOL buffer_contains_ascii',a);helper=s[a:b]
pre=r'''
#include <assert.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define CHAR char
static int present,read_ok,fail_at,calls;
static wchar_t commands[8][128];
static BOOL bcd_current_has_option(const char *s,BOOL *p){assert(!strcmp(s,"DDISABLE_INTEGRITY_CHECKS"));*p=present;return read_ok;}
static BOOL run_process_and_wait(const wchar_t *s,int t){assert(t==30000);wcscpy(commands[calls++],s);return calls!=fail_at;}
#define emit_status(...) ((void)0)
#define GetLastError() 1
static void reset(int p){present=p;read_ok=1;fail_at=calls=0;}
'''
test=r'''
int main(void){
 reset(1);assert(configure_boot_integrity());assert(calls==4);
 assert(!wcscmp(commands[0],L"/set {current} testsigning off"));
 assert(!wcscmp(commands[1],L"/set {current} nointegritychecks off"));
 assert(!wcscmp(commands[2],L"/deletevalue {current} loadoptions"));
 assert(!wcscmp(commands[3],L"/set {current} advancedoptions off"));
 reset(0);assert(configure_boot_integrity());assert(calls==3);
 assert(!wcscmp(commands[2],L"/set {current} advancedoptions off"));
 for(int i=1;i<=4;i++){reset(1);fail_at=i;assert(!configure_boot_integrity());assert(calls==i);}
 reset(1);read_ok=0;assert(!configure_boot_integrity());assert(calls==0);
 puts("PASS x86 legacy migration, clean install, every command failure, and BCD read failure");
}
'''
for arch in ('x86', 'x64'):
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        signing_test = test.replace(
            'testsigning off',
            'testsigning on' if arch == 'x64' else 'testsigning off',
        ).replace('PASS x86', f'PASS {arch}')
        source = directory / 'test.c'
        binary = directory / 'test'
        source.write_text(pre + helper + signing_test)
        subprocess.run([
            'clang', *(['-D_WIN64'] if arch == 'x64' else []),
            '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
