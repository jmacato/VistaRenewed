#!/usr/bin/env python3
"""Exercise production SetupAPI adapter selection and runtime registration."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / "packaging/vista-driver-deploy-service.c").read_text()
first = source.index("static BOOL device_id_list_contains(")
last = source.index("static BOOL install_driver(", first)
production = source[first:last]

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
typedef int BOOL;
typedef uint32_t DWORD, REGSAM;
typedef int32_t LONG;
typedef wchar_t WCHAR;
typedef const WCHAR *PCWSTR;
typedef unsigned char BYTE, *PBYTE;
typedef DWORD *PDWORD;
typedef void *HMODULE, *HDEVINFO, *HWND, *HKEY;
typedef struct { int unused; } GUID;
typedef struct { DWORD cbSize; } SP_DEVINFO_DATA, *PSP_DEVINFO_DATA;
typedef struct { WCHAR hardware_id[128]; } DeployMedia;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((void *)(intptr_t)-1)
#define ERROR_SUCCESS 0
#define ERROR_INVALID_DATA 13
#define ERROR_INVALID_PARAMETER 87
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_NOT_FOUND 1168
#define ERROR_WRITE_FAULT 29
#define SPDRP_HARDWAREID 1
#define SPDRP_COMPATIBLEIDS 2
#define SPDRP_SERVICE 4
#define REG_SZ 1
#define REG_MULTI_SZ 7
#define DIGCF_ALLCLASSES 4
#define DIGCF_PRESENT 2
#define DICS_FLAG_GLOBAL 1
#define DIREG_DRV 2
#define KEY_QUERY_VALUE 1
#define KEY_SET_VALUE 2
#define ZeroMemory(p, n) memset(p, 0, n)
#define emit_status(...) ((void)0)
static DWORD last_error, opens, writes, destroys, flushes;
static WCHAR hardware[256], compatible[256], service[32];
static DWORD hardware_bytes, compatible_bytes, hardware_type, compatible_type;
static WCHAR installed[3][256];
static DWORD installed_bytes[3];
static BOOL fail_write;
static int _wcsicmp(const WCHAR *a, const WCHAR *b)
{
    while (*a && towlower(*a) == towlower(*b)) { ++a; ++b; }
    return (int)towlower(*a) - (int)towlower(*b);
}
static DWORD GetLastError(void) { return last_error; }
static HMODULE LoadLibraryW(PCWSTR name)
{ assert(!wcscmp(name, L"setupapi.dll")); return (void *)1; }
static void FreeLibrary(HMODULE module) { assert(module == (void *)1); }
static HDEVINFO get_devices(const GUID *guid, PCWSTR group, HWND window, DWORD flags)
{
    assert(!guid && !window && !wcscmp(group, L"PCI"));
    assert(flags == (DIGCF_ALLCLASSES | DIGCF_PRESENT));
    return (void *)2;
}
static BOOL enumerate(HDEVINFO devices, DWORD index, PSP_DEVINFO_DATA device)
{
    assert(devices == (void *)2 && device->cbSize == sizeof(*device));
    if (!index) return TRUE;
    last_error = ERROR_NO_MORE_ITEMS;
    return FALSE;
}
static BOOL property(HDEVINFO devices, PSP_DEVINFO_DATA device, DWORD prop,
                     PDWORD type, PBYTE buffer, DWORD capacity, PDWORD bytes)
{
    const WCHAR *value;
    assert(devices == (void *)2 && device);
    if (prop == SPDRP_HARDWAREID) {
        value = hardware; *bytes = hardware_bytes; *type = hardware_type;
    } else if (prop == SPDRP_COMPATIBLEIDS) {
        value = compatible; *bytes = compatible_bytes; *type = compatible_type;
    } else {
        assert(prop == SPDRP_SERVICE);
        value = service; *bytes = (wcslen(service) + 1) * sizeof(WCHAR); *type = REG_SZ;
    }
    if (*bytes > capacity) return FALSE;
    memcpy(buffer, value, *bytes);
    return TRUE;
}
static HKEY open_key(HDEVINFO devices, PSP_DEVINFO_DATA device, DWORD scope,
                     DWORD profile, DWORD key_type, REGSAM access)
{
    assert(devices == (void *)2 && device && scope == DICS_FLAG_GLOBAL);
    assert(!profile && key_type == DIREG_DRV && (access & KEY_QUERY_VALUE));
    ++opens;
    return (void *)3;
}
static BOOL destroy(HDEVINFO devices)
{ assert(devices == (void *)2); ++destroys; return TRUE; }
static void *GetProcAddress(HMODULE module, const char *name)
{
    assert(module == (void *)1);
    if (!strcmp(name, "SetupDiGetClassDevsW")) return (void *)get_devices;
    if (!strcmp(name, "SetupDiEnumDeviceInfo")) return (void *)enumerate;
    if (!strcmp(name, "SetupDiGetDeviceRegistryPropertyW")) return (void *)property;
    if (!strcmp(name, "SetupDiOpenDevRegKey")) return (void *)open_key;
    if (!strcmp(name, "SetupDiDestroyDeviceInfoList")) return (void *)destroy;
    return NULL;
}
static unsigned value_index(PCWSTR name)
{
    if (!wcscmp(name, L"UserModeDriverName")) return 0;
    if (!wcscmp(name, L"InstalledDisplayDrivers")) return 1;
    assert(!wcscmp(name, L"UserModeDriverNameWow"));
    return 2;
}
static LONG RegSetValueExW(HKEY key, PCWSTR name, DWORD reserved, DWORD type,
                           const BYTE *data, DWORD bytes)
{
    unsigned index = value_index(name);
    assert(key == (void *)3 && !reserved && type == REG_MULTI_SZ);
    assert(bytes <= sizeof(installed[index]));
    ++writes;
    if (fail_write) return ERROR_WRITE_FAULT;
    memcpy(installed[index], data, bytes); installed_bytes[index] = bytes;
    return ERROR_SUCCESS;
}
static LONG RegQueryValueExW(HKEY key, PCWSTR name, PDWORD reserved, PDWORD type,
                             BYTE *data, PDWORD bytes)
{
    unsigned index = value_index(name);
    assert(key == (void *)3 && !reserved && *bytes >= installed_bytes[index]);
    memcpy(data, installed[index], installed_bytes[index]);
    *bytes = installed_bytes[index]; *type = REG_MULTI_SZ;
    return ERROR_SUCCESS;
}
static LONG RegFlushKey(HKEY key) { assert(key == (void *)3); ++flushes; return 0; }
static void RegCloseKey(HKEY key) { assert(key == (void *)3); }
/* SOURCE_UNDER_TEST */
static void reset(void)
{
    static const WCHAR hw[] = L"PCI\\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01\0";
    static const WCHAR compat[] = L"PCI\\VEN_1AF4&DEV_1050&REV_01\0pci\\ven_1af4&dev_1050\0";
    memset(hardware, 0, sizeof(hardware)); memset(compatible, 0, sizeof(compatible));
    memcpy(hardware, hw, sizeof(hw)); hardware_bytes = sizeof(hw);
    memcpy(compatible, compat, sizeof(compat)); compatible_bytes = sizeof(compat);
    wcscpy(service, L"VioGpu3D");
    hardware_type = compatible_type = REG_MULTI_SZ;
    last_error = opens = writes = destroys = flushes = fail_write = 0;
    memset(installed, 0, sizeof(installed)); memset(installed_bytes, 0, sizeof(installed_bytes));
}
int main(void)
{
    DeployMedia media = {L"PCI\\VEN_1AF4&DEV_1050"}; DWORD error;
    unsigned expected_values = TRITON_DEPLOY_HAS_WOW64 ? 3 : 2;
    reset();
    assert(d3d_runtime_registration(&media, TRUE, &error));
    assert(!error && opens == 1 && writes == expected_values && flushes == 1 && destroys == 1);
    assert(d3d_runtime_registration(&media, FALSE, &error));
    assert(writes == expected_values && flushes == 1 && destroys == 2);
    reset(); wcscpy(hardware, media.hardware_id);
    hardware_bytes = (wcslen(media.hardware_id) + 2) * sizeof(WCHAR);
    hardware[hardware_bytes / sizeof(WCHAR) - 1] = 0;
    compatible_type = REG_SZ;
    assert(d3d_runtime_registration(&media, TRUE, &error));
    reset(); wcscpy(service, L"other-driver");
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    assert(error == ERROR_INVALID_DATA && !opens && !writes && destroys == 1);
    reset(); compatible_type = REG_SZ;
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    assert(error == ERROR_NOT_FOUND && !opens);
    reset(); compatible_bytes -= sizeof(WCHAR);
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    reset(); compatible_bytes -= 1;
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    reset(); compatible[compatible_bytes / sizeof(WCHAR) - 1] = L'x';
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    reset(); compatible[0] = 0;
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    reset(); compatible_bytes = 2048 * sizeof(WCHAR) + 1;
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    reset(); wcscpy(media.hardware_id, L"PCI\\VEN_1AF4&DEV_105");
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    wcscpy(media.hardware_id, L"PCI\\VEN_1AF4&DEV_1050");
    reset(); fail_write = TRUE;
    assert(!d3d_runtime_registration(&media, TRUE, &error));
    assert(error == ERROR_WRITE_FAULT && writes == 1 && !flushes && destroys == 1);
    reset();
    assert(!d3d_runtime_registration(&media, FALSE, &error));
    assert(error == ERROR_INVALID_DATA && !writes && destroys == 1);
    puts("D3D10 deployment adapter matching and runtime registration passed");
}
'''

with tempfile.TemporaryDirectory(prefix="triton-deployment-") as tmp:
    tmp = Path(tmp)
    for wow in (0, 1):
        for old_matching in (False, True):
            code = production
            if old_matching:
                code = code.replace("SPDRP_HARDWAREID, SPDRP_COMPATIBLEIDS",
                                    "SPDRP_HARDWAREID, SPDRP_HARDWAREID")
            c_file = tmp / "registration.c"
            binary = tmp / "registration"
            c_file.write_text(fixture.replace("/* SOURCE_UNDER_TEST */", code))
            subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", f"-DTRITON_DEPLOY_HAS_WOW64={wow}",
                            str(c_file), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if old_matching:
                assert result.returncode and "Assertion" in result.stderr, result
                print(f"D3D10 {'x64' if wow else 'x86'} HardwareID-only negative control passed")
            else:
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout.strip())

# Run the production self-install preparation through its SCM boundary. The
# registry/service calls after that point are unrelated to executable copying.
start = source.index("static BOOL service_destination_writable(")
end = source.index("static BOOL install_self(", start)
prepare = source[start:end]
body_start = source.index("    if (!GetModuleFileNameW", end)
body_end = source.index("    if (!format_wstr(service_command", body_start)
prepare += """static BOOL prepare_service(void)
{
    WCHAR current[MAX_DEPLOY_PATH], destination[MAX_DEPLOY_PATH];
    WCHAR program_files[MAX_DEPLOY_PATH], directory[MAX_DEPLOY_PATH];
""" + source[body_start:body_end] + "    return TRUE;\n}\n"
copy_fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
typedef int BOOL;
typedef uint32_t DWORD;
typedef wchar_t WCHAR;
#define TRUE 1
#define FALSE 0
#define MAX_DEPLOY_PATH 1024
#define SERVICE_EXE_NAME L"triton-vista-deploy.exe"
#define ERROR_ACCESS_DENIED 5
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define INVALID_FILE_ATTRIBUTES UINT32_MAX
#define FILE_ATTRIBUTE_READONLY 1
#define FILE_ATTRIBUTE_DIRECTORY 16
#define FILE_ATTRIBUTE_ARCHIVE 32
#define FILE_ATTRIBUTE_NORMAL 128
#define _wcsicmp wcscmp
static const WCHAR target[] = L"C:\\Program Files\\TritonVistaDeploy\\triton-vista-deploy.exe";
static const WCHAR optical[] = L"D:\\driver\\triton-vista-deploy.exe";
static DWORD attributes, error;
static unsigned copies, attribute_writes, fail_attribute_at;
static BOOL same_file, equal_bytes, fail_copy;
static DWORD GetLastError(void) { return error; }
static void SetLastError(DWORD value) { error = value; }
static DWORD GetModuleFileNameW(void *module, WCHAR *out, DWORD capacity)
{
    assert(!module && capacity == MAX_DEPLOY_PATH);
    wcscpy(out, same_file ? target : optical); return wcslen(out);
}
static DWORD ExpandEnvironmentStringsW(const WCHAR *name, WCHAR *out, DWORD capacity)
{
    assert(!wcscmp(name, L"%ProgramFiles%") && capacity == MAX_DEPLOY_PATH);
    wcscpy(out, L"C:\\Program Files"); return wcslen(out) + 1;
}
static BOOL format_wstr(WCHAR *out, DWORD count, const WCHAR *format, ...)
{
    va_list args; va_start(args, format);
    int written = vswprintf(out, count, format, args); va_end(args);
    return written >= 0 && (DWORD)written < count;
}
static BOOL CreateDirectoryW(const WCHAR *name, void *security)
{ assert(!wcscmp(name, L"C:\\Program Files\\TritonVistaDeploy") && !security); return TRUE; }
static BOOL files_equal(const WCHAR *from, const WCHAR *to)
{ assert(!wcscmp(from, optical) && !wcscmp(to, target)); return equal_bytes; }
static DWORD GetFileAttributesW(const WCHAR *name)
{ assert(!wcscmp(name, target)); return attributes; }
static BOOL SetFileAttributesW(const WCHAR *name, DWORD value)
{
    assert(!wcscmp(name, target)); ++attribute_writes;
    if (attribute_writes == fail_attribute_at) { error = ERROR_ACCESS_DENIED; return FALSE; }
    attributes = value; return TRUE;
}
static BOOL CopyFileW(const WCHAR *from, const WCHAR *to, BOOL fail_if_exists)
{
    assert(!wcscmp(from, optical) && !wcscmp(to, target) && !fail_if_exists); ++copies;
    if (fail_copy || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))) {
        error = ERROR_ACCESS_DENIED; return FALSE;
    }
    attributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE;
    return TRUE;
}
/* SOURCE_UNDER_TEST */
static void reset(void)
{
    attributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE;
    copies = attribute_writes = fail_attribute_at = error = 0;
    same_file = equal_bytes = fail_copy = FALSE;
}
int main(void)
{
    reset(); assert(prepare_service());
    assert(copies == 1 && attribute_writes == 2 && attributes == FILE_ATTRIBUTE_ARCHIVE);
    reset(); attributes = INVALID_FILE_ATTRIBUTES; error = ERROR_FILE_NOT_FOUND;
    assert(prepare_service() && copies == 1 && attribute_writes == 1);
    reset(); equal_bytes = TRUE;
    assert(prepare_service() && !copies && attribute_writes == 1);
    reset(); same_file = TRUE;
    assert(prepare_service() && !copies && attribute_writes == 1);
    reset(); attributes = FILE_ATTRIBUTE_DIRECTORY;
    assert(!prepare_service() && !copies && !attribute_writes && error == ERROR_ACCESS_DENIED);
    reset(); attributes = INVALID_FILE_ATTRIBUTES; error = ERROR_ACCESS_DENIED;
    assert(!prepare_service() && !copies && !attribute_writes && error == ERROR_ACCESS_DENIED);
    reset(); fail_attribute_at = 1;
    assert(!prepare_service() && !copies && attribute_writes == 1 && error == ERROR_ACCESS_DENIED);
    reset(); fail_attribute_at = 2;
    assert(!prepare_service() && copies == 1 && attribute_writes == 2 && error == ERROR_ACCESS_DENIED);
    reset(); fail_copy = TRUE;
    assert(!prepare_service() && copies == 1 && attribute_writes == 1 && error == ERROR_ACCESS_DENIED);
    reset(); attributes = FILE_ATTRIBUTE_READONLY; equal_bytes = TRUE;
    assert(prepare_service() && attributes == FILE_ATTRIBUTE_NORMAL);
    puts("Deployment service optical copy and readonly recovery passed");
}
'''
with tempfile.TemporaryDirectory(prefix="triton-service-copy-") as tmp:
    tmp = Path(tmp)
    for mutant in (False, True):
        code = prepare
        if mutant:
            code = code.replace("!service_destination_writable(destination, TRUE) ||", "")
        c_file, binary = tmp / "copy.c", tmp / "copy"
        c_file.write_text(copy_fixture.replace("/* SOURCE_UNDER_TEST */", code))
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(c_file), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if mutant:
            assert result.returncode and "Assertion" in result.stderr, result
            print("Deployment readonly-replacement negative control passed")
        else:
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
