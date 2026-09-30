#if defined(_WIN64)
#define EXPECTED_TESTSIGNING L"/set {current} testsigning on"
#else
#define EXPECTED_TESTSIGNING L"/set {current} testsigning off"
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define CHAR char
static int present, read_ok, fail_at, calls;
static wchar_t commands[8][128];
static BOOL bcd_current_has_option(const char *s, BOOL *p) {
    assert(!strcmp(s, "DDISABLE_INTEGRITY_CHECKS"));
    *p = present;
    return read_ok;
}
static BOOL run_process_and_wait(const wchar_t *s, int t) {
    assert(t == 30000);
    wcscpy(commands[calls++], s);
    return calls != fail_at;
}
#define emit_status(...) ((void)0)
#define GetLastError() 1
static void reset(int p) {
    present = p;
    read_ok = 1;
    fail_at = calls = 0;
}
/* SOURCE_UNDER_TEST */
int main(void) {
    reset(1);
    assert(configure_boot_integrity());
    assert(calls == 4);
    assert(!wcscmp(commands[0], EXPECTED_TESTSIGNING));
    assert(!wcscmp(commands[1], L"/set {current} nointegritychecks off"));
    assert(!wcscmp(commands[2], L"/deletevalue {current} loadoptions"));
    assert(!wcscmp(commands[3], L"/set {current} advancedoptions off"));
    reset(0);
    assert(configure_boot_integrity());
    assert(calls == 3);
    assert(!wcscmp(commands[2], L"/set {current} advancedoptions off"));
    for (int i = 1; i <= 4; i++) {
        reset(1);
        fail_at = i;
        assert(!configure_boot_integrity());
        assert(calls == i);
    }
    reset(1);
    read_ok = 0;
    assert(!configure_boot_integrity());
    assert(calls == 0);
    puts("PASS boot-policy legacy migration, clean install, every command failure, and "
         "BCD read failure");
}
