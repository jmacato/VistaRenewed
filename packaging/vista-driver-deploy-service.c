/*
 * Triton Vista driver deployment service.
 *
 * This program keeps all deployment control inside the guest. QEMU exposes a
 * read-only ISO that contains triton-deploy.ini and a signed driver package.
 * The service pins the signing certificate and verifies the catalog members.
 * It schedules locked files for replacement and then restarts the guest.
 * After restart, it compares all installed bytes before it starts the probe.
 * It does not require a desktop, keyboard, network, or host-side VM control.
 *
 * Build target: Windows Vista x86 or x64, legacy msvcrt.dll.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winsvc.h>
#include <setupapi.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#define SERVICE_NAME L"TritonVistaDeploy"
#define SERVICE_DISPLAY_NAME L"Triton Vista driver deployment"
#define SERVICE_DESCRIPTION L"Verifies and installs Triton display drivers from read-only QEMU media."
#define SERVICE_EXE_NAME L"triton-vista-deploy.exe"
#define STATE_KEY L"SOFTWARE\\Triton\\VistaDeploy"
#define ACTIVATION_BOOT_GUARD_KEY STATE_KEY L"\\ActivationBootGuard"
#define REPROBE_BOOT_GUARD_KEY STATE_KEY L"\\ReprobeBootGuard"
#define TRITON_VISTA_BYPASS_CUSTOM_TRUST 1
#define SAFEBOOT_MINIMAL L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal\\" SERVICE_NAME
#define SAFEBOOT_NETWORK L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Network\\" SERVICE_NAME
#define DEPLOY_INI L"triton-deploy.ini"
#define DEPLOY_SECTION L"triton-deploy"
#define PACKAGE_MANIFEST L"package-manifest.sha256"
#if defined(_WIN64)
#define PROBE_EXE_NAME L"triton9_runtime_probe_x64.exe"
#define CATALOG_NAME L"viogpu3d-vista-x64.cat"
#define TRITON_DEPLOY_HAS_WOW64 1
#else
#define PROBE_EXE_NAME L"triton9_runtime_probe_x86.exe"
#define CATALOG_NAME L"viogpu3d-vista-x86.cat"
#define TRITON_DEPLOY_HAS_WOW64 0
#endif
#define SIGNING_CERT_NAME L"triton-vista-linux-signing.cer"
#ifdef TRITON_DEPLOY_SIGNING_HEADER
#include TRITON_DEPLOY_SIGNING_HEADER
#else
#define SIGNING_CERT_SHA256 L"c1237845766aca1fa1304b7afd11c4c9c358dba9b8512addeb38f5902685a757"
#define SIGNING_CERT_THUMBPRINT_BYTES \
    0x56, 0xbf, 0xb6, 0x60, 0x05, 0x96, 0x0c, 0x09, 0x8c, 0x55, \
    0xde, 0xb0, 0x82, 0x0c, 0x7f, 0x00, 0xe3, 0x9f, 0x0e, 0xa1
#endif
#define HARDWARE_ID_DEFAULT L"PCI\\VEN_1AF4&DEV_1050"
#define MAX_DEPLOY_PATH 1024
#define MAX_DEPLOY_VALUE 256
#define MAX_MANIFEST_BYTES (128 * 1024)
#define MAX_PROBE_RESULT_BYTES (512 * 1024)

typedef struct DeployMedia {
    WCHAR root[4];
    WCHAR ini[MAX_DEPLOY_PATH];
    WCHAR id[65];
    WCHAR package[MAX_DEPLOY_VALUE];
    WCHAR inf[MAX_DEPLOY_VALUE];
    WCHAR hardware_id[MAX_DEPLOY_VALUE];
    WCHAR probe[MAX_DEPLOY_VALUE];
    WCHAR probe_sha256[65];
    WCHAR signing_cert[MAX_DEPLOY_VALUE];
    WCHAR signing_cert_sha256[65];
    WCHAR package_dir[MAX_DEPLOY_PATH];
} DeployMedia;

static SERVICE_STATUS_HANDLE g_status_handle;
static SERVICE_STATUS g_status;
static HANDLE g_stop_event;

static BOOL valid_deploy_id(const WCHAR *value);

static void copy_wstr(WCHAR *destination, DWORD capacity, const WCHAR *source)
{
    if (!capacity) {
        return;
    }
    wcsncpy(destination, source ? source : L"", capacity - 1);
    destination[capacity - 1] = L'\0';
}

static BOOL format_wstr(WCHAR *destination, DWORD capacity, const WCHAR *format, ...)
{
    int result;
    va_list arguments;
    if (!destination || !capacity || !format) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    va_start(arguments, format);
    result = _vsnwprintf(destination, capacity - 1, format, arguments);
    va_end(arguments);
    destination[capacity - 1] = L'\0';
    return result >= 0 && (DWORD)result < capacity;
}

static void get_log_path(WCHAR *path, DWORD capacity)
{
    WCHAR windows[MAX_PATH];
    if (!GetWindowsDirectoryW(windows, MAX_PATH) ||
        !format_wstr(path, capacity, L"%ls\\Temp\\triton-deploy.log", windows)) {
        copy_wstr(path, capacity, L"C:\\Windows\\Temp\\triton-deploy.log");
    }
}

static void emit_status(const WCHAR *format, ...)
{
    WCHAR wide[1024];
    WCHAR line[1200];
    WCHAR log_path[MAX_DEPLOY_PATH];
    CHAR utf8[4096];
    DWORD bytes;
    HANDLE handle;
    SYSTEMTIME now;
    va_list arguments;

    va_start(arguments, format);
    _vsnwprintf(wide, 1023, format, arguments);
    va_end(arguments);
    wide[1023] = L'\0';
    GetSystemTime(&now);
    format_wstr(line, 1200,
                L"TRITONDEPLOY %04u-%02u-%02uT%02u:%02u:%02uZ %ls\r\n",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                now.wSecond, wide);
    bytes = (DWORD)WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8,
                                       sizeof(utf8), NULL, NULL);
    if (!bytes) {
        return;
    }
    --bytes;

    get_log_path(log_path, MAX_DEPLOY_PATH);
    handle = CreateFileW(log_path, FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle != INVALID_HANDLE_VALUE) {
        WriteFile(handle, utf8, bytes, &bytes, NULL);
        FlushFileBuffers(handle);
        CloseHandle(handle);
    }

    /* Both SYSTEM services serialize short, exclusive COM2 opens. The probe
     * must not keep the port open across this service's launch markers. */
    {
        HANDLE mutex = CreateMutexW(NULL, FALSE, L"Global\\TritonVistaTelemetry");
        DWORD wait;
        if (!mutex) return;
        wait = WaitForSingleObject(mutex, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return;
        }
        handle = CreateFileW(L"\\\\.\\COM2", GENERIC_WRITE, 0, NULL,
                             OPEN_EXISTING, 0, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            COMMTIMEOUTS timeouts;
            ZeroMemory(&timeouts, sizeof(timeouts));
            timeouts.WriteTotalTimeoutConstant = 1000;
            SetCommTimeouts(handle, &timeouts);
            WriteFile(handle, utf8, bytes, &bytes, NULL);
            CloseHandle(handle);
        }
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
}

static BOOL write_state(const WCHAR *name, const WCHAR *value)
{
    HKEY key;
    LONG status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, STATE_KEY, 0, NULL, 0,
                                  KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return FALSE;
    }
    status = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                            ((DWORD)wcslen(value) + 1) * sizeof(WCHAR));
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return FALSE;
    }
    return TRUE;
}

static BOOL read_state(const WCHAR *name, WCHAR *value, DWORD capacity)
{
    HKEY key;
    DWORD type = 0;
    DWORD bytes = capacity * sizeof(WCHAR);
    LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, STATE_KEY, 0,
                                KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        value[0] = L'\0';
        return FALSE;
    }
    status = RegQueryValueExW(key, name, NULL, &type, (BYTE *)value, &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(WCHAR)) {
        value[0] = L'\0';
        return FALSE;
    }
    value[capacity - 1] = L'\0';
    return TRUE;
}

typedef enum ReprobeBootGuardState {
    REPROBE_GUARD_ERROR = -1,
    REPROBE_GUARD_ABSENT = 0,
    REPROBE_GUARD_MATCH = 1,
    REPROBE_GUARD_MISMATCH = 2
} ReprobeBootGuardState;

/* A volatile registry key survives a service-process restart but is removed
 * when Vista unloads the system hive during a real reboot.  This gives the
 * guest-owned reprobe state machine a boot boundary without USER32, host
 * input, wall-clock assumptions, or the 49-day GetTickCount wrap. */
static ReprobeBootGuardState query_boot_guard(const WCHAR *key_path,
                                              const WCHAR *deployment_id)
{
    HKEY key;
    WCHAR stored_id[65];
    DWORD type = 0;
    DWORD bytes = sizeof(stored_id);
    LONG status;

    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0,
                           KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return REPROBE_GUARD_ABSENT;
    }
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return REPROBE_GUARD_ERROR;
    }
    ZeroMemory(stored_id, sizeof(stored_id));
    status = RegQueryValueExW(key, L"DeploymentId", NULL, &type,
                              (BYTE *)stored_id, &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ ||
        bytes < sizeof(WCHAR) || bytes > sizeof(stored_id) ||
        stored_id[64] != L'\0' || !valid_deploy_id(stored_id)) {
        SetLastError(status == ERROR_SUCCESS ? ERROR_INVALID_DATA :
                     (DWORD)status);
        return REPROBE_GUARD_ERROR;
    }
    if (_wcsicmp(stored_id, deployment_id) == 0) {
        return REPROBE_GUARD_MATCH;
    }
    SetLastError(ERROR_INVALID_DATA);
    return REPROBE_GUARD_MISMATCH;
}

static BOOL create_boot_guard(const WCHAR *key_path,
                              const WCHAR *deployment_id)
{
    HKEY key;
    LONG status;
    ReprobeBootGuardState current;

    current = query_boot_guard(key_path, deployment_id);
    if (current == REPROBE_GUARD_MATCH) return TRUE;
    if (current != REPROBE_GUARD_ABSENT) {
        if (current == REPROBE_GUARD_MISMATCH) SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path, 0,
                             NULL, REG_OPTION_VOLATILE,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key,
                             NULL);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return FALSE;
    }
    status = RegSetValueExW(key, L"DeploymentId", 0, REG_SZ,
                            (const BYTE *)deployment_id,
                            ((DWORD)wcslen(deployment_id) + 1) *
                                sizeof(WCHAR));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return FALSE;
    }
    return query_boot_guard(key_path, deployment_id) == REPROBE_GUARD_MATCH;
}

static BOOL set_default_registry_value(HKEY root, const WCHAR *path,
                                       const WCHAR *value)
{
    HKEY key;
    LONG status = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_SET_VALUE,
                                  NULL, &key, NULL);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return FALSE;
    }
    status = RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)value,
                            ((DWORD)wcslen(value) + 1) * sizeof(WCHAR));
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return FALSE;
    }
    return TRUE;
}

static BOOL safe_component(const WCHAR *value)
{
    const WCHAR *cursor;
    if (!value || !value[0] || wcslen(value) >= MAX_DEPLOY_VALUE) {
        return FALSE;
    }
    for (cursor = value; *cursor; ++cursor) {
        WCHAR ch = *cursor;
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
              (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-' ||
              ch == L'_')) {
            return FALSE;
        }
    }
    return wcscmp(value, L".") != 0 && wcscmp(value, L"..") != 0;
}

static BOOL valid_deploy_id(const WCHAR *value)
{
    DWORD index;
    if (wcslen(value) != 64) {
        return FALSE;
    }
    for (index = 0; index < 64; ++index) {
        WCHAR ch = value[index];
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
              (ch >= L'A' && ch <= L'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL load_media_from_root(WCHAR drive_letter, DeployMedia *media)
{
    WCHAR version[16];
    DWORD attributes;
    ZeroMemory(media, sizeof(*media));
    media->root[0] = drive_letter;
    media->root[1] = L':';
    media->root[2] = L'\\';
    media->root[3] = L'\0';
    if (!format_wstr(media->ini, MAX_DEPLOY_PATH, L"%ls%ls", media->root,
                     DEPLOY_INI)) {
        return FALSE;
    }
    attributes = GetFileAttributesW(media->ini);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return FALSE;
    }
    GetPrivateProfileStringW(DEPLOY_SECTION, L"version", L"", version, 16,
                             media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"id", L"", media->id, 65,
                             media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"package", L"", media->package,
                             MAX_DEPLOY_VALUE, media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"inf", L"", media->inf,
                             MAX_DEPLOY_VALUE, media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"hardware_id", HARDWARE_ID_DEFAULT,
                             media->hardware_id, MAX_DEPLOY_VALUE, media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"probe", L"", media->probe,
                             MAX_DEPLOY_VALUE, media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"probe_sha256", L"",
                             media->probe_sha256, 65, media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"signing_cert", L"",
                             media->signing_cert, MAX_DEPLOY_VALUE, media->ini);
    GetPrivateProfileStringW(DEPLOY_SECTION, L"signing_cert_sha256", L"",
                             media->signing_cert_sha256, 65, media->ini);
    if (wcscmp(version, L"1") != 0 || !valid_deploy_id(media->id) ||
        !safe_component(media->package) || !safe_component(media->inf) ||
        _wcsicmp(media->probe, PROBE_EXE_NAME) != 0 ||
        !valid_deploy_id(media->probe_sha256) ||
        !safe_component(media->signing_cert) ||
        _wcsicmp(media->signing_cert, SIGNING_CERT_NAME) != 0 ||
        !valid_deploy_id(media->signing_cert_sha256) ||
        _wcsicmp(media->signing_cert_sha256, SIGNING_CERT_SHA256) != 0 ||
        !media->hardware_id[0]) {
        emit_status(L"INVALID_MEDIA drive=%c", drive_letter);
        return FALSE;
    }
    if (!format_wstr(media->package_dir, MAX_DEPLOY_PATH, L"%ls%ls",
                     media->root, media->package)) {
        return FALSE;
    }
    return TRUE;
}

static BOOL find_deploy_media(DeployMedia *media)
{
    DWORD drives = GetLogicalDrives();
    WCHAR letter;
    DeployMedia candidate;
    BOOL found = FALSE;
    ZeroMemory(media, sizeof(*media));
    for (letter = L'D'; letter <= L'Z'; ++letter) {
        UINT type;
        WCHAR root[4] = {letter, L':', L'\\', L'\0'};
        if (!(drives & (1u << (letter - L'A')))) {
            continue;
        }
        type = GetDriveTypeW(root);
        if (type != DRIVE_CDROM) continue;
        if (load_media_from_root(letter, &candidate)) {
            if (found) {
                emit_status(L"AMBIGUOUS_MEDIA first=%c second=%c",
                            media->root[0], candidate.root[0]);
                ZeroMemory(media, sizeof(*media));
                SetLastError(ERROR_DUP_NAME);
                return FALSE;
            }
            CopyMemory(media, &candidate, sizeof(*media));
            found = TRUE;
        }
    }
    return found;
}

static BOOL hash_file_sha256(const WCHAR *path, WCHAR hex[65])
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE buffer[64 * 1024];
    BYTE digest[32];
    DWORD read_bytes;
    DWORD digest_bytes = sizeof(digest);
    DWORD index;
    static const WCHAR digits[] = L"0123456789abcdef";
    BOOL ok = FALSE;

    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        goto done;
    }
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        goto done;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        goto done;
    }
    do {
        if (!ReadFile(file, buffer, sizeof(buffer), &read_bytes, NULL)) {
            goto done;
        }
        if (read_bytes && !CryptHashData(hash, buffer, read_bytes, 0)) {
            goto done;
        }
    } while (read_bytes);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_bytes, 0) ||
        digest_bytes != sizeof(digest)) {
        goto done;
    }
    for (index = 0; index < sizeof(digest); ++index) {
        hex[index * 2] = digits[digest[index] >> 4];
        hex[index * 2 + 1] = digits[digest[index] & 15];
    }
    hex[64] = L'\0';
    ok = TRUE;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return ok;
}

static BOOL create_random_nonce(WCHAR hex[65])
{
    HCRYPTPROV provider = 0;
    BYTE random_bytes[32];
    DWORD index;
    static const WCHAR digits[] = L"0123456789abcdef";
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        return FALSE;
    }
    if (!CryptGenRandom(provider, sizeof(random_bytes), random_bytes)) {
        DWORD error = GetLastError();
        CryptReleaseContext(provider, 0);
        SetLastError(error);
        return FALSE;
    }
    CryptReleaseContext(provider, 0);
    for (index = 0; index < sizeof(random_bytes); ++index) {
        hex[index * 2] = digits[random_bytes[index] >> 4];
        hex[index * 2 + 1] = digits[random_bytes[index] & 15];
    }
    hex[64] = L'\0';
    return TRUE;
}

static int ascii_hex_value(CHAR ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static BOOL certificate_has_pinned_thumbprint(PCCERT_CONTEXT certificate)
{
    static const BYTE expected[20] = {
        SIGNING_CERT_THUMBPRINT_BYTES
    };
    BYTE actual[20];
    DWORD actual_size = sizeof(actual);

    return certificate &&
           CertGetCertificateContextProperty(certificate,
                                             CERT_SHA1_HASH_PROP_ID,
                                             actual, &actual_size) &&
           actual_size == sizeof(expected) &&
           memcmp(actual, expected, sizeof(expected)) == 0;
}

static BOOL add_certificate_to_machine_store(PCCERT_CONTEXT certificate,
                                             const WCHAR *store_name)
{
    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE,
        store_name);
    BOOL ok;

    if (!store) return FALSE;
    ok = CertAddCertificateContextToStore(
        store, certificate, CERT_STORE_ADD_REPLACE_EXISTING, NULL);
    CertCloseStore(store, 0);
    return ok;
}

static BOOL install_pinned_signing_certificate(const DeployMedia *media)
{
    WCHAR certificate_path[MAX_DEPLOY_PATH], actual_hash[65] = L"unavailable";
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    BYTE *encoded = NULL;
    DWORD bytes_read = 0;
    PCCERT_CONTEXT certificate = NULL;
    BOOL ok = FALSE;

    if (!format_wstr(certificate_path, MAX_DEPLOY_PATH, L"%ls%ls",
                     media->root, media->signing_cert) ||
        !hash_file_sha256(certificate_path, actual_hash) ||
        _wcsicmp(actual_hash, SIGNING_CERT_SHA256) != 0) {
        emit_status(L"VERIFY_FAIL id=%ls step=signing-cert-hash expected=%ls actual=%ls error=%lu",
                    media->id, SIGNING_CERT_SHA256, actual_hash,
                    (unsigned long)GetLastError());
        return FALSE;
    }
    file = CreateFileW(certificate_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) ||
        size.QuadPart <= 0 || size.QuadPart > 64 * 1024) goto done;
    encoded = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    if (!encoded || !ReadFile(file, encoded, (DWORD)size.QuadPart,
                              &bytes_read, NULL) ||
        bytes_read != (DWORD)size.QuadPart) goto done;
    certificate = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, encoded, bytes_read);
    if (!certificate || !certificate_has_pinned_thumbprint(certificate)) {
        SetLastError(ERROR_INVALID_DATA);
        goto done;
    }
    if (!add_certificate_to_machine_store(certificate, L"ROOT") ||
        !add_certificate_to_machine_store(certificate, L"TrustedPublisher")) {
        goto done;
    }
    emit_status(L"SIGNING_CERT_OK id=%ls sha256=%ls",
                media->id, SIGNING_CERT_SHA256);
    ok = TRUE;

done:
    if (!ok) {
        emit_status(L"VERIFY_FAIL id=%ls step=signing-cert-import error=%lu",
                    media->id, (unsigned long)GetLastError());
    }
    if (certificate) CertFreeCertificateContext(certificate);
    if (encoded) HeapFree(GetProcessHeap(), 0, encoded);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static BOOL signed_file_uses_pinned_certificate(const WCHAR *path)
{
    DWORD encoding = 0, content = 0, format = 0;
    DWORD signer_size = 0;
    HCERTSTORE store = NULL;
    HCRYPTMSG message = NULL;
    PCMSG_SIGNER_INFO signer = NULL;
    PCCERT_CONTEXT certificate = NULL;
    CERT_INFO certificate_id;
    BOOL ok = FALSE;

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED |
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format,
            &store, &message, NULL) ||
        !CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, NULL,
                          &signer_size) || !signer_size) goto done;
    signer = (PCMSG_SIGNER_INFO)HeapAlloc(GetProcessHeap(), 0, signer_size);
    if (!signer ||
        !CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signer,
                          &signer_size)) goto done;
    ZeroMemory(&certificate_id, sizeof(certificate_id));
    certificate_id.Issuer = signer->Issuer;
    certificate_id.SerialNumber = signer->SerialNumber;
    certificate = CertFindCertificateInStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_SUBJECT_CERT, &certificate_id, NULL);
    ok = certificate_has_pinned_thumbprint(certificate);
    if (!ok) SetLastError(TRUST_E_CERT_SIGNATURE);

done:
    if (certificate) CertFreeCertificateContext(certificate);
    if (signer) HeapFree(GetProcessHeap(), 0, signer);
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
    return ok;
}

static BOOL verify_authenticode_file(const WCHAR *path)
{
    static const GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO file_info;
    WINTRUST_DATA trust;
    LONG status;

    ZeroMemory(&file_info, sizeof(file_info));
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path;
    ZeroMemory(&trust, sizeof(trust));
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file_info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_NONE |
                        WTD_CACHE_ONLY_URL_RETRIEVAL;
    status = WinVerifyTrust(NULL, (GUID *)&action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, (GUID *)&action, &trust);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return FALSE;
    }
    return signed_file_uses_pinned_certificate(path);
}

typedef BOOL (WINAPI *CryptCatCalcHashFn)(HANDLE, DWORD *, BYTE *, DWORD);

static CryptCatCalcHashFn resolve_catalog_hash_function(void)
{
    static HMODULE module;
    static CryptCatCalcHashFn function;

    if (function) return function;
    module = LoadLibraryW(L"wintrust.dll");
    if (!module) return NULL;
    function = (CryptCatCalcHashFn)(void *)GetProcAddress(
        module, "CryptCATAdminCalcHashFromFileHandle");
    return function;
}

static BOOL verify_catalog_member(const WCHAR *catalog_path,
                                  const WCHAR *member_path)
{
    static const GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    static const WCHAR digits[] = L"0123456789ABCDEF";
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE *hash = NULL;
    WCHAR *tag = NULL;
    DWORD hash_size = 0, index;
    WINTRUST_CATALOG_INFO catalog_info;
    WINTRUST_DATA trust;
    CryptCatCalcHashFn calculate_hash;
    LONG status;
    BOOL ok = FALSE;

    file = CreateFileW(member_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    calculate_hash = resolve_catalog_hash_function();
    if (!calculate_hash) goto done;
    calculate_hash(file, &hash_size, NULL, 0);
    if (!hash_size || hash_size > 128) goto done;
    hash = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hash_size);
    tag = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
                             (hash_size * 2 + 1) * sizeof(WCHAR));
    if (!hash || !tag ||
        !calculate_hash(file, &hash_size, hash, 0)) {
        goto done;
    }
    for (index = 0; index < hash_size; ++index) {
        tag[index * 2] = digits[hash[index] >> 4];
        tag[index * 2 + 1] = digits[hash[index] & 15];
    }
    tag[hash_size * 2] = L'\0';
    emit_status(L"CATALOG_MEMBER_HASH file=%ls tag=%ls", member_path, tag);
    ZeroMemory(&catalog_info, sizeof(catalog_info));
    catalog_info.cbStruct = sizeof(catalog_info);
    catalog_info.pcwszCatalogFilePath = catalog_path;
    catalog_info.pcwszMemberTag = tag;
    catalog_info.pcwszMemberFilePath = member_path;
    catalog_info.hMemberFile = file;
    catalog_info.pbCalculatedFileHash = hash;
    catalog_info.cbCalculatedFileHash = hash_size;
    ZeroMemory(&trust, sizeof(trust));
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_CATALOG;
    trust.pCatalog = &catalog_info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_NONE |
                        WTD_CACHE_ONLY_URL_RETRIEVAL;
    status = WinVerifyTrust(NULL, (GUID *)&action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, (GUID *)&action, &trust);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        goto done;
    }
    ok = TRUE;

done:
    if (tag) HeapFree(GetProcessHeap(), 0, tag);
    if (hash) HeapFree(GetProcessHeap(), 0, hash);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static BOOL verify_package_manifest(const DeployMedia *media)
{
    WCHAR manifest_path[MAX_DEPLOY_PATH];
    WCHAR manifest_hash[65];
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    CHAR *data = NULL;
    DWORD bytes_read = 0;
    CHAR *cursor;
    BOOL have_inf = FALSE, have_cat = FALSE, have_sys = FALSE;
    BOOL have_native = FALSE, have_native10 = FALSE, have_service = FALSE;
#if TRITON_DEPLOY_HAS_WOW64
    BOOL have_wow = FALSE, have_wow10 = FALSE;
#endif
    BOOL have_probe = FALSE;
    BOOL ok = FALSE;

    if (!format_wstr(manifest_path, MAX_DEPLOY_PATH, L"%ls\\%ls",
                     media->package_dir, PACKAGE_MANIFEST)) {
        return FALSE;
    }
    if (!hash_file_sha256(manifest_path, manifest_hash)) {
        emit_status(L"VERIFY_FAIL id=%ls step=manifest-hash error=%lu",
                    media->id, (unsigned long)GetLastError());
        return FALSE;
    }
    if (_wcsicmp(manifest_hash, media->id) != 0) {
        emit_status(L"VERIFY_FAIL id=%ls step=manifest-id expected=%ls actual=%ls",
                    media->id, media->id, manifest_hash);
        return FALSE;
    }

    file = CreateFileW(manifest_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) ||
        size.QuadPart <= 0 || size.QuadPart > MAX_MANIFEST_BYTES) {
        goto done;
    }
    data = (CHAR *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 1);
    if (!data || !ReadFile(file, data, (DWORD)size.QuadPart, &bytes_read, NULL) ||
        bytes_read != (DWORD)size.QuadPart) {
        goto done;
    }
    data[bytes_read] = '\0';
    cursor = data;
    while (*cursor) {
        CHAR expected_ascii[65];
        WCHAR expected[65];
        WCHAR actual[65];
        WCHAR filename[MAX_DEPLOY_VALUE];
        WCHAR path[MAX_DEPLOY_PATH];
        CHAR *line_end = cursor;
        CHAR *name;
        DWORD index;
        while (*line_end && *line_end != '\r' && *line_end != '\n') ++line_end;
        if (line_end - cursor < 66) goto done;
        for (index = 0; index < 64; ++index) {
            if (ascii_hex_value(cursor[index]) < 0) goto done;
            expected_ascii[index] = cursor[index];
        }
        expected_ascii[64] = '\0';
        name = cursor + 64;
        while (name < line_end && (*name == ' ' || *name == '\t' || *name == '*')) ++name;
        int filename_length;
        if (name == line_end || line_end - name >= MAX_DEPLOY_VALUE) {
            goto done;
        }
        filename_length = MultiByteToWideChar(
            CP_ACP, 0, name, (int)(line_end - name), filename,
            MAX_DEPLOY_VALUE - 1);
        if (filename_length <= 0) goto done;
        filename[filename_length] = L'\0';
        if (!safe_component(filename)) goto done;
        MultiByteToWideChar(CP_ACP, 0, expected_ascii, -1, expected, 65);
        if (!format_wstr(path, MAX_DEPLOY_PATH, L"%ls\\%ls",
                         media->package_dir, filename) ||
            !hash_file_sha256(path, actual) || _wcsicmp(expected, actual) != 0) {
            emit_status(L"VERIFY_FAIL id=%ls step=file file=%ls error=%lu",
                        media->id, filename, (unsigned long)GetLastError());
            goto done;
        }
        if (_wcsicmp(filename, media->inf) == 0) have_inf = TRUE;
        if (_wcsicmp(filename, CATALOG_NAME) == 0) have_cat = TRUE;
        if (_wcsicmp(filename, L"viogpu3d.sys") == 0) have_sys = TRUE;
        if (_wcsicmp(filename, L"neptune_d3d9.dll") == 0) have_native = TRUE;
        if (_wcsicmp(filename, L"neptune_d3d10.dll") == 0) have_native10 = TRUE;
#if TRITON_DEPLOY_HAS_WOW64
        if (_wcsicmp(filename, L"neptune_d3d9_wow.dll") == 0) have_wow = TRUE;
        if (_wcsicmp(filename, L"neptune_d3d10_wow.dll") == 0) have_wow10 = TRUE;
#endif
        if (_wcsicmp(filename, SERVICE_EXE_NAME) == 0) have_service = TRUE;
        if (_wcsicmp(filename, PROBE_EXE_NAME) == 0)
            have_probe = TRUE;
        cursor = line_end;
        while (*cursor == '\r' || *cursor == '\n') ++cursor;
    }
    ok = have_inf && have_cat && have_sys && have_native && have_native10 &&
#if TRITON_DEPLOY_HAS_WOW64
         have_wow && have_wow10 &&
#endif
         have_service && have_probe;
    if (!ok) {
        emit_status(L"VERIFY_FAIL id=%ls step=required-files", media->id);
    }

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (data) HeapFree(GetProcessHeap(), 0, data);
    return ok;
}

static BOOL files_equal(const WCHAR *left, const WCHAR *right)
{
    HANDLE a = INVALID_HANDLE_VALUE, b = INVALID_HANDLE_VALUE;
    LARGE_INTEGER a_size, b_size;
    BYTE a_buffer[64 * 1024], b_buffer[64 * 1024];
    DWORD a_read, b_read;
    BOOL equal = FALSE;
    a = CreateFileW(left, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    b = CreateFileW(right, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(a, &a_size) || !GetFileSizeEx(b, &b_size) ||
        a_size.QuadPart != b_size.QuadPart) goto done;
    do {
        if (!ReadFile(a, a_buffer, sizeof(a_buffer), &a_read, NULL) ||
            !ReadFile(b, b_buffer, sizeof(b_buffer), &b_read, NULL) ||
            a_read != b_read || (a_read && memcmp(a_buffer, b_buffer, a_read))) {
            goto done;
        }
    } while (a_read);
    equal = TRUE;
done:
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    return equal;
}

static BOOL verify_package_signatures(const DeployMedia *media)
{
    static const WCHAR *const catalog_members[] = {
        L"viogpu3d.sys",
        L"neptune_d3d9.dll",
        L"neptune_d3d10.dll",
#if TRITON_DEPLOY_HAS_WOW64
        L"neptune_d3d9_wow.dll",
        L"neptune_d3d10_wow.dll",
#endif
        SERVICE_EXE_NAME
    };
    static const WCHAR *const embedded_files[] = {
        L"viogpu3d.sys",
        L"neptune_d3d9.dll",
        L"neptune_d3d10.dll",
#if TRITON_DEPLOY_HAS_WOW64
        L"neptune_d3d9_wow.dll",
        L"neptune_d3d10_wow.dll",
#endif
        SERVICE_EXE_NAME,
        PROBE_EXE_NAME
    };
    WCHAR catalog_path[MAX_DEPLOY_PATH], file_path[MAX_DEPLOY_PATH];
    DWORD index;

    /*
     * Vista's checked trust provider can remain CPU-bound indefinitely while
     * it revalidates this already signed optical package.  The package
     * manifest still SHA-256 checks every deployment byte, and Windows still
     * enforces the signed catalog when PnP consumes the INF.  Bypass only this
     * redundant service-side WinVerifyTrust pass so guest-owned recovery can
     * make forward progress.
     */
    if (TRITON_VISTA_BYPASS_CUSTOM_TRUST) {
        /* The probe and PnP still use Windows trust. Provision the pinned
         * development certificate even when redundant package checks are
         * skipped, including on a fresh service bootstrap. */
        if (!install_pinned_signing_certificate(media)) return FALSE;
        emit_status(L"PACKAGE_SIGNATURES_BYPASSED id=%ls integrity=manifest-sha256",
                    media->id);
        return TRUE;
    }

    if (!install_pinned_signing_certificate(media) ||
        !format_wstr(catalog_path, MAX_DEPLOY_PATH,
                     L"%ls\\%ls", media->package_dir, CATALOG_NAME) ||
        !verify_authenticode_file(catalog_path)) {
        emit_status(L"VERIFY_FAIL id=%ls step=catalog-signature error=%lu",
                    media->id, (unsigned long)GetLastError());
        return FALSE;
    }
    if (!format_wstr(file_path, MAX_DEPLOY_PATH, L"%ls\\%ls",
                     media->package_dir, media->inf) ||
        !verify_catalog_member(catalog_path, file_path)) {
        emit_status(L"VERIFY_FAIL id=%ls step=catalog-member file=%ls error=%lu",
                    media->id, media->inf, (unsigned long)GetLastError());
        return FALSE;
    }
    for (index = 0; index < sizeof(catalog_members) /
                                  sizeof(catalog_members[0]); ++index) {
        if (!format_wstr(file_path, MAX_DEPLOY_PATH, L"%ls\\%ls",
                         media->package_dir, catalog_members[index]) ||
            !verify_catalog_member(catalog_path, file_path)) {
            emit_status(L"VERIFY_FAIL id=%ls step=catalog-member file=%ls error=%lu",
                        media->id, catalog_members[index],
                        (unsigned long)GetLastError());
            return FALSE;
        }
    }
    for (index = 0; index < sizeof(embedded_files) /
                                  sizeof(embedded_files[0]); ++index) {
        if (!format_wstr(file_path, MAX_DEPLOY_PATH, L"%ls\\%ls",
                         media->package_dir, embedded_files[index]) ||
            !verify_authenticode_file(file_path)) {
            emit_status(L"VERIFY_FAIL id=%ls step=embedded-signature file=%ls error=%lu",
                        media->id, embedded_files[index],
                        (unsigned long)GetLastError());
            return FALSE;
        }
    }
    emit_status(L"PACKAGE_SIGNATURES_OK id=%ls", media->id);
    return TRUE;
}

static BOOL verify_optional_probe(const DeployMedia *media)
{
    WCHAR path[MAX_DEPLOY_PATH], package_path[MAX_DEPLOY_PATH];
    WCHAR actual[65];

    if (_wcsicmp(media->probe, PROBE_EXE_NAME) != 0) {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    if (!format_wstr(path, MAX_DEPLOY_PATH, L"%ls%ls", media->root,
                     media->probe) ||
        !format_wstr(package_path, MAX_DEPLOY_PATH, L"%ls\\%ls",
                     media->package_dir, media->probe) ||
        !files_equal(path, package_path) || !hash_file_sha256(path, actual) ||
        !verify_authenticode_file(path) ||
        _wcsicmp(actual, media->probe_sha256) != 0) {
        emit_status(L"VERIFY_FAIL id=%ls step=probe error=%lu", media->id,
                    (unsigned long)GetLastError());
        return FALSE;
    }
    return TRUE;
}

static BOOL verify_deployment_media(const DeployMedia *media)
{
    return verify_package_manifest(media) &&
           verify_package_signatures(media) &&
           verify_optional_probe(media);
}

static BOOL format_probe_service_name(const DeployMedia *media, WCHAR *service_name,
                                      DWORD service_name_capacity)
{
    if (!media || !service_name || service_name_capacity == 0 ||
        !valid_deploy_id(media->id)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return format_wstr(service_name, service_name_capacity,
                       L"TritonD3D9Probe_%.*ls", 12, media->id);
}

static BOOL format_probe_result_log(const DeployMedia *media,
                                    const WCHAR *result_nonce,
                                    WCHAR *result_log,
                                    DWORD result_log_capacity)
{
    WCHAR windows[MAX_PATH];
    if (!media || !result_nonce || !result_log ||
        !valid_deploy_id(media->id) || !valid_deploy_id(result_nonce) ||
        !GetWindowsDirectoryW(windows, MAX_PATH)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return format_wstr(result_log, result_log_capacity,
                       L"%ls\\Temp\\triton9-service-%.*ls-%.*ls.log",
                       windows, 12, media->id, 16, result_nonce);
}

static BOOL stage_optional_probe(const DeployMedia *media, WCHAR *service_name,
                                 DWORD service_name_capacity,
                                 const WCHAR *result_log,
                                 const WCHAR *result_nonce)
{
    WCHAR source[MAX_DEPLOY_PATH], destination[MAX_DEPLOY_PATH], windows[MAX_PATH];
    WCHAR command[MAX_DEPLOY_PATH * 2 + 256];
    WCHAR display_name[128];
    SC_HANDLE manager = NULL, service = NULL;
    BOOL ok = FALSE;
    if (!media || !service_name || service_name_capacity == 0 ||
        !result_log || !result_log[0] || !valid_deploy_id(result_nonce)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    service_name[0] = L'\0';
    if (!media->probe[0]) return TRUE;
    /* The probe deletes its own SCM entry.  Vista can retain that entry in a
     * marked-for-delete state long enough to deny a replacement.  Scope the
     * service and its staged executable to this immutable deployment instead
     * of retrying the one global name. */
    if (!format_probe_service_name(media, service_name,
                                   service_name_capacity)) return FALSE;
    /* SCM requires display names to be unique too.  The preceding probe may
     * still be alive while this deployment is staged, so tying only the key
     * name to the immutable media ID is insufficient: ERROR_DUPLICATE_SERVICE_NAME
     * (1078) otherwise rejects the replacement. */
    if (!format_wstr(display_name, (DWORD)(sizeof(display_name) / sizeof(display_name[0])),
                     L"Triton D3D9 bring-up %.*ls", 12, media->id)) return FALSE;
    if (!format_wstr(source, MAX_DEPLOY_PATH, L"%ls%ls", media->root,
                     media->probe) || !GetWindowsDirectoryW(windows, MAX_PATH) ||
        !format_wstr(destination, MAX_DEPLOY_PATH, L"%ls\\Temp\\%ls.exe",
                     windows, service_name)) return FALSE;
    if (!files_equal(source, destination) && !CopyFileW(source, destination, TRUE))
        return FALSE;
    if (!format_wstr(command, MAX_DEPLOY_PATH * 2 + 256,
                     L"\"%ls\" --service-name %ls --result-log \"%ls\" --result-nonce %ls",
                     destination, service_name, result_log,
                     result_nonce)) return FALSE;
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager) goto done;
    service = OpenServiceW(manager, service_name, SERVICE_CHANGE_CONFIG);
    if (service) {
        ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE,
            SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command, NULL, NULL, NULL,
            L"LocalSystem", NULL, display_name);
    } else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        service = CreateServiceW(manager, service_name, display_name,
            SERVICE_CHANGE_CONFIG,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            command, NULL, NULL, NULL, L"LocalSystem", NULL);
        ok = service != NULL;
    }
done:
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    return ok;
}

/* The D3D9 probe is deliberately a one-shot, demand-start service: it removes
 * its own SCM entry after it has launched the secure-desktop child.  The
 * deployment service starts it only after it authenticates the package and
 * proves the installed bytes on this boot.  This remains entirely guest-owned
 * and avoids an auto-start race with stale result-log removal. */
static BOOL launch_optional_probe(const DeployMedia *media, DWORD *error_out)
{
    SC_HANDLE manager = NULL, service = NULL;
    SERVICE_STATUS service_status;
    WCHAR service_name[96], result_log[MAX_DEPLOY_PATH];
    WCHAR result_nonce[65], stored_log[MAX_DEPLOY_PATH], stored_nonce[65];
    BOOL ok = FALSE;
    DWORD error = ERROR_SUCCESS;

    if (error_out) *error_out = ERROR_SUCCESS;
    if (!media->probe[0]) return TRUE;
    if (!format_probe_service_name(
            media, service_name,
            (DWORD)(sizeof(service_name) / sizeof(service_name[0])))) {
        error = GetLastError();
        goto done;
    }

    /* A service-process failure can occur after StartService succeeds but
     * before ProbeLaunchId is committed.  Inspect the existing one-shot
     * service before changing its command, nonce, or result path.  A running
     * instance must keep the exact launch identity that it received. */
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) {
        error = GetLastError();
        goto done;
    }
    service = OpenServiceW(manager, service_name, SERVICE_QUERY_STATUS);
    if (service) {
        if (!QueryServiceStatus(service, &service_status)) {
            error = GetLastError();
            goto done;
        }
        if (service_status.dwCurrentState == SERVICE_RUNNING ||
            service_status.dwCurrentState == SERVICE_START_PENDING) {
            if (!read_state(L"ProbeResultPath", stored_log, MAX_DEPLOY_PATH) ||
                !read_state(L"ProbeResultNonce", stored_nonce, 65) ||
                !valid_deploy_id(stored_nonce) ||
                !format_probe_result_log(media, stored_nonce, result_log,
                                         MAX_DEPLOY_PATH) ||
                _wcsicmp(stored_log, result_log) != 0) {
                error = ERROR_INVALID_DATA;
                goto done;
            }
            ok = TRUE;
            goto done;
        }
        if (service_status.dwCurrentState != SERVICE_STOPPED) {
            error = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
            goto done;
        }
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
        error = GetLastError();
        goto done;
    }
    if (service) {
        CloseServiceHandle(service);
        service = NULL;
    }
    CloseServiceHandle(manager);
    manager = NULL;

    if (!create_random_nonce(result_nonce) ||
        !format_probe_result_log(media, result_nonce, result_log,
                                 MAX_DEPLOY_PATH) ||
        !write_state(L"ProbeResultPath", result_log) ||
        !write_state(L"ProbeResultNonce", result_nonce)) {
        error = GetLastError();
        goto done;
    }
    if (!stage_optional_probe(media, service_name,
                              (DWORD)(sizeof(service_name) / sizeof(service_name[0])),
                              result_log, result_nonce)) {
        error = GetLastError();
        goto done;
    }
    /* The one-shot probe creates this file before it writes a result. Remove
     * the prior deployment's file before StartService so no reboot decision
     * can consume a stale PASS while the new process is still starting. */
    if (!DeleteFileW(result_log) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        error = GetLastError();
        goto done;
    }
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) {
        error = GetLastError();
        goto done;
    }
    service = OpenServiceW(manager, service_name, SERVICE_START);
    if (!service) {
        error = GetLastError();
        goto done;
    }
    emit_status(L"PROBE_RUN_ARMED id=%ls nonce=%ls", media->id,
                result_nonce);
    if (!StartServiceW(service, 0, NULL)) {
        error = GetLastError();
        goto done;
    }
    ok = TRUE;

done:
    if (error_out) *error_out = error;
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    return ok;
}

/* Vista keeps the display miniport and its UMDs open in normal mode.  The
 * guest service enters Safe Mode before it calls this code.  Stage all files
 * first, then replace the three graphics files while the Triton stack is not
 * loaded.  SafeBoot remains durable until every graphics byte is verified. */
static BOOL stage_payload_replacement(const DeployMedia *media,
                                      const WCHAR *source,
                                      const WCHAR *destination,
                                      WCHAR pending[MAX_DEPLOY_PATH],
                                      BOOL *needs_replacement,
                                      DWORD *error_out)
{
    const WCHAR *leaf;
    DWORD attributes;
    HANDLE pending_file;

    if (!media || !valid_deploy_id(media->id) || !source || !destination ||
        !pending || !needs_replacement || !error_out ||
        !(leaf = wcsrchr(destination, L'\\')) || !leaf[1] ||
        !format_wstr(pending, MAX_DEPLOY_PATH,
                     L"%ls.triton-%.*ls-pending", destination, 16,
                     media->id)) {
        if (error_out) *error_out = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    *needs_replacement = !files_equal(source, destination);
    if (!*needs_replacement) {
        pending[0] = L'\0';
        return TRUE;
    }
    /* Safe Mode leaves the Triton graphics files unloaded. Stage beside each
     * destination so replacement stays on one volume and the new file
     * inherits the protected destination directory's access policy. */
    attributes = GetFileAttributesW(pending);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(pending, attributes & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!files_equal(source, pending) && !CopyFileW(source, pending, FALSE)) {
        *error_out = GetLastError();
        emit_status(L"INSTALL_PAYLOAD_FAIL file=%ls step=copy-staging error=%lu",
                    leaf + 1, (unsigned long)*error_out);
        return FALSE;
    }
    attributes = GetFileAttributesW(pending);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        ((attributes & FILE_ATTRIBUTE_READONLY) &&
         !SetFileAttributesW(pending,
                             attributes & ~FILE_ATTRIBUTE_READONLY))) {
        *error_out = GetLastError();
        emit_status(L"INSTALL_PAYLOAD_FAIL file=%ls step=staging-attributes error=%lu",
                    leaf + 1, (unsigned long)*error_out);
        return FALSE;
    }
    pending_file = CreateFileW(pending, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL |
                               FILE_FLAG_WRITE_THROUGH, NULL);
    if (pending_file == INVALID_HANDLE_VALUE ||
        !FlushFileBuffers(pending_file)) {
        *error_out = GetLastError();
        if (pending_file != INVALID_HANDLE_VALUE)
            CloseHandle(pending_file);
        emit_status(L"INSTALL_PAYLOAD_FAIL file=%ls step=flush-staging error=%lu",
                    leaf + 1, (unsigned long)*error_out);
        return FALSE;
    }
    CloseHandle(pending_file);
    if (!files_equal(source, pending)) {
        *error_out = ERROR_CRC;
        emit_status(L"INSTALL_PAYLOAD_FAIL file=%ls step=verify-staging error=%lu",
                    leaf + 1, (unsigned long)*error_out);
        return FALSE;
    }
    emit_status(L"INSTALL_PAYLOAD_STAGED file=%ls", leaf + 1);
    return TRUE;
}

static BOOL move_staged_payload(const WCHAR *pending,
                                const WCHAR *destination,
                                DWORD flags,
                                const WCHAR *operation,
                                DWORD *error_out)
{
    const WCHAR *leaf = wcsrchr(destination, L'\\');
    DWORD attributes;
    DWORD attempt = 0;
    if (!pending || !pending[0] || !leaf || !leaf[1] || !error_out) {
        if (error_out) *error_out = ERROR_INVALID_PARAMETER;
        return FALSE;
    }

    /* CopyFile preserves the read-only attribute when an older package came
     * from optical media. Vista then rejects MOVEFILE_REPLACE_EXISTING with
     * ERROR_ACCESS_DENIED, even in Safe Mode. Remove only that destination
     * attribute before the same-volume replacement. The staged file was
     * already flushed, verified, and made writable above. */
    attributes = GetFileAttributesW(destination);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD attribute_error = GetLastError();
        if (attribute_error != ERROR_FILE_NOT_FOUND &&
            attribute_error != ERROR_PATH_NOT_FOUND) {
            *error_out = attribute_error;
            emit_status(L"INSTALL_PAYLOAD_FAIL file=%ls step=destination-attributes error=%lu",
                        leaf + 1, (unsigned long)*error_out);
            return FALSE;
        }
    } else if ((attributes & FILE_ATTRIBUTE_READONLY) &&
               !SetFileAttributesW(destination,
                                   attributes & ~FILE_ATTRIBUTE_READONLY)) {
        *error_out = GetLastError();
        emit_status(L"INSTALL_PAYLOAD_FAIL file=%ls step=destination-writable error=%lu",
                    leaf + 1, (unsigned long)*error_out);
        return FALSE;
    } else if (attributes & FILE_ATTRIBUTE_READONLY) {
        emit_status(L"INSTALL_PAYLOAD_DESTINATION_WRITABLE file=%ls",
                    leaf + 1);
    }

    for (;;) {
        if (MoveFileExW(pending, destination, flags)) {
            emit_status(L"INSTALL_PAYLOAD_OK file=%ls operation=%ls",
                        leaf + 1, operation);
            return TRUE;
        }
        *error_out = GetLastError();
        ++attempt;
        emit_status(L"INSTALL_PAYLOAD_RETRY file=%ls operation=%ls attempt=%lu error=%lu",
                    leaf + 1, operation, (unsigned long)attempt,
                    (unsigned long)*error_out);
        if (g_stop_event &&
            WaitForSingleObject(g_stop_event, 5000) != WAIT_TIMEOUT) {
            return FALSE;
        }
    }
}

static BOOL activate_staged_payload(const WCHAR *pending,
                                    const WCHAR *destination,
                                    DWORD *error_out)
{
    return move_staged_payload(pending, destination,
                               MOVEFILE_REPLACE_EXISTING |
                               MOVEFILE_WRITE_THROUGH,
                               L"safe-activate", error_out);
}

static BOOL schedule_staged_payload(const WCHAR *pending,
                                    const WCHAR *destination,
                                    DWORD *error_out)
{
    /* Only the deployment service executable is live in Safe Mode.  Its one
     * delayed move cannot create a mixed graphics generation.  Session
     * Manager replaces it before this service starts on the next boot. */
    return move_staged_payload(pending, destination,
                               MOVEFILE_REPLACE_EXISTING |
                               MOVEFILE_DELAY_UNTIL_REBOOT,
                               L"service-reboot", error_out);
}

static BOOL device_id_list_contains(const WCHAR *ids, DWORD bytes,
                                    const WCHAR *expected)
{
    size_t count = bytes / sizeof(*ids), offset = 0;
    BOOL match = FALSE;

    if (bytes % sizeof(*ids) || count < 2 ||
        ids[count - 1] || ids[count - 2])
        return FALSE;
    while (offset < count - 1 && ids[offset]) {
        size_t end = offset;
        while (end < count && ids[end]) ++end;
        if (end == count) return FALSE;
        if (!_wcsicmp(ids + offset, expected)) match = TRUE;
        offset = end + 1;
    }
    /* Do not accept an ID hidden after the list terminator. */
    while (offset < count)
        if (ids[offset++]) return FALSE;
    return match;
}

/* The safe-byte update path does not run INF installation for an existing
 * device. Update its API slots only after every DLL has been verified. The
 * durable SafeBoot transaction is retained on any failure and retries this
 * idempotently before normal graphics can load a mixed generation. */
static BOOL d3d_runtime_registration(const DeployMedia *media, BOOL update, DWORD *error_out)
{
    static const WCHAR native[] = L"neptune_d3d9.dll\0neptune_d3d10.dll\0";
    static const WCHAR installed[] = L"neptune_d3d9\0neptune_d3d10\0";
#if TRITON_DEPLOY_HAS_WOW64
    static const WCHAR wow[] = L"neptune_d3d9_wow.dll\0neptune_d3d10_wow.dll\0";
#endif
    const WCHAR *names[] = { L"UserModeDriverName", L"InstalledDisplayDrivers",
#if TRITON_DEPLOY_HAS_WOW64
                            L"UserModeDriverNameWow",
#endif
    };
    const WCHAR *values[] = { native, installed,
#if TRITON_DEPLOY_HAS_WOW64
                              wow,
#endif
    };
    const DWORD sizes[] = { sizeof(native), sizeof(installed),
#if TRITON_DEPLOY_HAS_WOW64
                            sizeof(wow),
#endif
    };
    HMODULE api = LoadLibraryW(L"setupapi.dll");
    HDEVINFO devices = INVALID_HANDLE_VALUE;
    BOOL ok = FALSE, found = FALSE;
    DWORD index;
    HDEVINFO (WINAPI *get)(const GUID *, PCWSTR, HWND, DWORD);
    BOOL (WINAPI *enumerate)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
    BOOL (WINAPI *property)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
    HKEY (WINAPI *open_key)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, DWORD, DWORD, REGSAM);
    BOOL (WINAPI *destroy)(HDEVINFO);
    *error_out = ERROR_PROC_NOT_FOUND;
    if (!api) { *error_out = GetLastError(); return FALSE; }
    get = (void *)GetProcAddress(api, "SetupDiGetClassDevsW");
    enumerate = (void *)GetProcAddress(api, "SetupDiEnumDeviceInfo");
    property = (void *)GetProcAddress(api, "SetupDiGetDeviceRegistryPropertyW");
    open_key = (void *)GetProcAddress(api, "SetupDiOpenDevRegKey");
    destroy = (void *)GetProcAddress(api, "SetupDiDestroyDeviceInfoList");
    if (!get || !enumerate || !property || !open_key || !destroy) goto done;
    devices = get(NULL, L"PCI", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) { *error_out = GetLastError(); goto done; }
    for (index = 0;; ++index) {
        SP_DEVINFO_DATA device;
        WCHAR ids[2048] = {0}, service[128] = {0};
        static const DWORD id_properties[] = {
            SPDRP_HARDWAREID, SPDRP_COMPATIBLEIDS
        };
        DWORD type = 0, bytes = 0, v, p;
        HKEY key;
        BOOL match = FALSE;
        ZeroMemory(&device, sizeof(device)); device.cbSize = sizeof(device);
        if (!enumerate(devices, index, &device)) {
            *error_out = GetLastError();
            if (*error_out == ERROR_NO_MORE_ITEMS) break;
            goto done;
        }
        for (p = 0; p < sizeof(id_properties)/sizeof(id_properties[0]); ++p) {
            if (property(devices, &device, id_properties[p], &type, (PBYTE)ids,
                         sizeof(ids), &bytes) && type == REG_MULTI_SZ &&
                bytes <= sizeof(ids) &&
                device_id_list_contains(ids, bytes, media->hardware_id))
                match = TRUE;
        }
        if (!match) continue;
        if (!property(devices, &device, SPDRP_SERVICE, &type, (PBYTE)service,
                      sizeof(service) - sizeof(WCHAR), &bytes) ||
            type != REG_SZ || _wcsicmp(service, L"viogpu3d")) {
            *error_out = ERROR_INVALID_DATA; goto done;
        }
        key = open_key(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV,
                       KEY_QUERY_VALUE | (update ? KEY_SET_VALUE : 0));
        if (key == INVALID_HANDLE_VALUE) { *error_out = GetLastError(); goto done; }
        for (v = 0; v < sizeof(names)/sizeof(names[0]); ++v) {
            WCHAR actual[256];
            LONG result;
            if (update) {
                result = RegSetValueExW(key, names[v], 0, REG_MULTI_SZ,
                                       (const BYTE *)values[v], sizes[v]);
                if (result != ERROR_SUCCESS) {
                    RegCloseKey(key); *error_out = result; goto done;
                }
            }
            bytes = sizeof(actual);
            result = RegQueryValueExW(key, names[v], NULL, &type, (BYTE *)actual, &bytes);
            if (result != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes != sizes[v] ||
                memcmp(actual, values[v], sizes[v])) {
                RegCloseKey(key); *error_out = ERROR_INVALID_DATA; goto done;
            }
        }
        if (update && RegFlushKey(key) != ERROR_SUCCESS) {
            RegCloseKey(key); *error_out = ERROR_WRITE_FAULT; goto done;
        }
        RegCloseKey(key);
        found = TRUE;
    }
    ok = found;
    *error_out = found ? ERROR_SUCCESS : ERROR_NOT_FOUND;
done:
    if (devices != INVALID_HANDLE_VALUE) destroy(devices);
    FreeLibrary(api);
    emit_status(L"D3D_RUNTIME_REGISTRATION mode=%ls ok=%u error=%lu",
                update ? L"update" : L"verify", ok, (unsigned long)*error_out);
    return ok;
}

static BOOL install_driver(const DeployMedia *media, DWORD *error_out)
{
    WCHAR sources[6][MAX_DEPLOY_PATH], destinations[6][MAX_DEPLOY_PATH];
    WCHAR pending[6][MAX_DEPLOY_PATH];
    WCHAR windows[MAX_PATH], system[MAX_PATH];
    BOOL needs_replacement[6];
    DWORD index, graphics_payloads = TRITON_DEPLOY_HAS_WOW64 ? 5 : 3;
    DWORD service_payload = graphics_payloads;
    BOOL ok = FALSE;

    *error_out = ERROR_SUCCESS;
    emit_status(L"INSTALL_BEGIN id=%ls mode=safe-payload", media->id);
    if (!GetWindowsDirectoryW(windows, MAX_PATH) ||
        !GetSystemDirectoryW(system, MAX_PATH)) {
        *error_out = GetLastError();
        goto done;
    }
    emit_status(L"INSTALL_STAGE id=%ls step=stage-safe-payload", media->id);
    if (!format_wstr(sources[0], MAX_DEPLOY_PATH,
                     L"%ls\\viogpu3d.sys", media->package_dir) ||
        !format_wstr(destinations[0], MAX_DEPLOY_PATH,
                     L"%ls\\drivers\\viogpu3d.sys", system) ||
        !format_wstr(sources[1], MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d9.dll", media->package_dir) ||
        !format_wstr(destinations[1], MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d9.dll", system) ||
        !format_wstr(sources[2], MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d10.dll", media->package_dir) ||
        !format_wstr(destinations[2], MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d10.dll", system) ||
#if TRITON_DEPLOY_HAS_WOW64
        !format_wstr(sources[4], MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d10_wow.dll", media->package_dir) ||
        !format_wstr(destinations[4], MAX_DEPLOY_PATH,
                     L"%ls\\SysWOW64\\neptune_d3d10_wow.dll", windows) ||
        !format_wstr(sources[3], MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d9_wow.dll", media->package_dir) ||
        !format_wstr(destinations[3], MAX_DEPLOY_PATH,
                     L"%ls\\SysWOW64\\neptune_d3d9_wow.dll", windows) ||
#endif
        !format_wstr(sources[TRITON_DEPLOY_HAS_WOW64 ? 5 : 3], MAX_DEPLOY_PATH,
                     L"%ls\\%ls", media->package_dir, SERVICE_EXE_NAME) ||
        !GetModuleFileNameW(NULL, destinations[TRITON_DEPLOY_HAS_WOW64 ? 5 : 3], MAX_DEPLOY_PATH)) {
        *error_out = GetLastError();
        goto done;
    }
    /* Stage every byte first. A staging failure therefore leaves no pending
     * rename that a later reboot could apply as a mixed driver generation. */
    for (index = 0; index <= service_payload; ++index) {
        if (!stage_payload_replacement(media, sources[index],
                                       destinations[index], pending[index],
                                       &needs_replacement[index], error_out)) {
            goto done;
        }
    }
    /* Apply the complete graphics generation only while SafeBoot is still
     * set. A power loss between members restarts in Safe Mode, where this
     * idempotent loop finishes the same authenticated generation. */
    for (index = 0; index < graphics_payloads; ++index) {
        if (needs_replacement[index] &&
            !activate_staged_payload(pending[index], destinations[index],
                                     error_out)) {
            goto done;
        }
    }
    if (needs_replacement[service_payload] &&
        !schedule_staged_payload(pending[service_payload], destinations[service_payload], error_out))
        goto done;
    for (index = 0; index < graphics_payloads; ++index) {
        if (!files_equal(sources[index], destinations[index])) {
            *error_out = ERROR_INVALID_DATA;
            emit_status(L"INSTALL_PAYLOAD_FAIL file=%ls step=safe-byte-verify error=%lu",
                        wcsrchr(destinations[index], L'\\') + 1,
                        (unsigned long)*error_out);
            goto done;
        }
    }
    if (!d3d_runtime_registration(media, TRUE, error_out)) goto done;
    ok = TRUE;

done:
    if (ok) {
        emit_status(L"INSTALL_OK id=%ls reboot-required=1", media->id);
    } else {
        emit_status(L"INSTALL_FAIL id=%ls step=stage-payload error=%lu",
                    media->id, (unsigned long)*error_out);
    }
    return ok;
}

static BOOL verify_installed_payload_pair(const DeployMedia *media,
                                          const WCHAR *package_name,
                                          const WCHAR *installed_path)
{
    WCHAR package_path[MAX_DEPLOY_PATH];
    WCHAR expected[65] = L"unavailable";
    WCHAR actual[65] = L"unavailable";
    BOOL equal;

    if (!format_wstr(package_path, MAX_DEPLOY_PATH, L"%ls\\%ls",
                     media->package_dir, package_name)) return FALSE;
    hash_file_sha256(package_path, expected);
    hash_file_sha256(installed_path, actual);
    equal = files_equal(package_path, installed_path);
    emit_status(equal ?
        L"POST_REBOOT_FILE_OK id=%ls file=%ls sha256=%ls" :
        L"POST_REBOOT_FILE_FAIL id=%ls file=%ls expected=%ls actual=%ls error=%lu",
        media->id, package_name, expected, actual,
        (unsigned long)GetLastError());
    return equal;
}

static BOOL verify_installed_payloads(const DeployMedia *media)
{
    WCHAR windows[MAX_PATH], system[MAX_PATH], installed[MAX_DEPLOY_PATH];

    if (!GetWindowsDirectoryW(windows, MAX_PATH) ||
        !GetSystemDirectoryW(system, MAX_PATH)) return FALSE;
    if (!format_wstr(installed, MAX_DEPLOY_PATH,
                     L"%ls\\drivers\\viogpu3d.sys", system) ||
        !verify_installed_payload_pair(media, L"viogpu3d.sys", installed))
        return FALSE;
    if (!format_wstr(installed, MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d9.dll", system) ||
        !verify_installed_payload_pair(media, L"neptune_d3d9.dll", installed))
        return FALSE;
    if (!format_wstr(installed, MAX_DEPLOY_PATH,
                     L"%ls\\neptune_d3d10.dll", system) ||
        !verify_installed_payload_pair(media, L"neptune_d3d10.dll", installed))
        return FALSE;
#if TRITON_DEPLOY_HAS_WOW64
    if (!format_wstr(installed, MAX_DEPLOY_PATH,
                     L"%ls\\SysWOW64\\neptune_d3d9_wow.dll", windows) ||
        !verify_installed_payload_pair(media, L"neptune_d3d9_wow.dll", installed))
        return FALSE;
    if (!format_wstr(installed, MAX_DEPLOY_PATH,
                     L"%ls\\SysWOW64\\neptune_d3d10_wow.dll", windows) ||
        !verify_installed_payload_pair(media, L"neptune_d3d10_wow.dll", installed))
        return FALSE;
#endif
    if (!GetModuleFileNameW(NULL, installed, MAX_DEPLOY_PATH) ||
        !verify_installed_payload_pair(media, SERVICE_EXE_NAME, installed))
        return FALSE;
    DWORD registration_error;
    if (!d3d_runtime_registration(media, FALSE, &registration_error)) return FALSE;
    emit_status(L"POST_REBOOT_PAYLOAD_OK id=%ls", media->id);
    return TRUE;
}

static BOOL run_process_and_wait(const WCHAR *arguments, DWORD timeout_ms)
{
    WCHAR system[MAX_PATH], application[MAX_DEPLOY_PATH], command[2048];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = ERROR_GEN_FAILURE;
    if (!GetSystemDirectoryW(system, MAX_PATH) ||
        !format_wstr(application, MAX_DEPLOY_PATH, L"%ls\\bcdedit.exe", system) ||
        !format_wstr(command, 2048, L"\"%ls\" %ls", application, arguments)) {
        return FALSE;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessW(application, command, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        return FALSE;
    }
    if (WaitForSingleObject(process.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    } else {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        SetLastError(ERROR_TIMEOUT);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code != ERROR_SUCCESS) SetLastError(exit_code);
    return exit_code == ERROR_SUCCESS;
}

static BOOL bcd_current_has_option(const CHAR *option, BOOL *present);

/* Vista x64 requires Test Mode for our self-signed kernel driver. Both
 * architectures remove the legacy integrity bypasses from older installs. */
static BOOL configure_boot_integrity(void)
{
#if defined(_WIN64)
    BOOL has_legacy_bypass = FALSE;
    if (!bcd_current_has_option("DDISABLE_INTEGRITY_CHECKS", &has_legacy_bypass) ||
        !run_process_and_wait(L"/set {current} testsigning on", 30000) ||
        !run_process_and_wait(L"/set {current} nointegritychecks off", 30000) ||
        (has_legacy_bypass &&
         !run_process_and_wait(L"/deletevalue {current} loadoptions", 30000)) ||
        !run_process_and_wait(L"/set {current} advancedoptions off", 30000)) {
        emit_status(L"BOOT_INTEGRITY_TEST_FAIL arch=x64 error=%lu",
                    (unsigned long)GetLastError());
        return FALSE;
    }
    emit_status(L"BOOT_INTEGRITY_TEST_OK arch=x64 testsigning=on "
                L"nointegritychecks=off legacy-loadoptions-removed=%lu",
                (unsigned long)has_legacy_bypass);
    return TRUE;
#else
    BOOL has_legacy_bypass = FALSE;
    if (!bcd_current_has_option("DDISABLE_INTEGRITY_CHECKS", &has_legacy_bypass) ||
        !run_process_and_wait(L"/set {current} testsigning off", 30000) ||
        !run_process_and_wait(L"/set {current} nointegritychecks off", 30000) ||
        (has_legacy_bypass &&
         !run_process_and_wait(L"/deletevalue {current} loadoptions", 30000)) ||
        !run_process_and_wait(L"/set {current} advancedoptions off", 30000)) {
        emit_status(L"BOOT_INTEGRITY_NORMAL_FAIL arch=x86 error=%lu",
                    (unsigned long)GetLastError());
        return FALSE;
    }
    emit_status(L"BOOT_INTEGRITY_NORMAL_OK arch=x86 testsigning=off "
                L"nointegritychecks=off legacy-loadoptions-removed=%lu",
                (unsigned long)has_legacy_bypass);
    return TRUE;
#endif
}

static BOOL buffer_contains_ascii_case_insensitive(const CHAR *buffer,
                                                   DWORD length,
                                                   const CHAR *needle)
{
    DWORD index, match;
    DWORD needle_length = (DWORD)strlen(needle);
    if (!needle_length || needle_length > length) return FALSE;
    for (index = 0; index + needle_length <= length; ++index) {
        for (match = 0; match < needle_length; ++match) {
            CHAR left = buffer[index + match];
            CHAR right = needle[match];
            if (left >= 'A' && left <= 'Z') left = (CHAR)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = (CHAR)(right - 'A' + 'a');
            if (left != right) break;
        }
        if (match == needle_length) return TRUE;
    }
    return FALSE;
}

static BOOL buffer_contains_utf16le_ascii_case_insensitive(
    const CHAR *buffer, DWORD length, const CHAR *needle)
{
    DWORD index, match;
    DWORD needle_length = (DWORD)strlen(needle);
    if (!needle_length || needle_length * 2 > length) return FALSE;
    for (index = 0; index + needle_length * 2 <= length; ++index) {
        for (match = 0; match < needle_length; ++match) {
            CHAR left = buffer[index + match * 2];
            CHAR right = needle[match];
            if (buffer[index + match * 2 + 1] != '\0') break;
            if (left >= 'A' && left <= 'Z') left = (CHAR)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = (CHAR)(right - 'A' + 'a');
            if (left != right) break;
        }
        if (match == needle_length) return TRUE;
    }
    return FALSE;
}

static BOOL probe_log_has_completed_success(void)
{
    WCHAR windows[MAX_PATH], log_path[MAX_DEPLOY_PATH];
    WCHAR expected_prefix[MAX_DEPLOY_PATH], nonce[65];
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    CHAR *data = NULL;
    DWORD bytes_read = 0;
    CHAR nonce_ascii[65], nonce_marker[96];
    const WCHAR *leaf;
    BOOL ok = FALSE;

    if (!read_state(L"ProbeResultPath", log_path, MAX_DEPLOY_PATH) ||
        !read_state(L"ProbeResultNonce", nonce, 65) ||
        !valid_deploy_id(nonce) ||
        !GetWindowsDirectoryW(windows, MAX_PATH) ||
        !format_wstr(expected_prefix, MAX_DEPLOY_PATH,
                     L"%ls\\Temp\\triton9-service-", windows) ||
        _wcsnicmp(log_path, expected_prefix, wcslen(expected_prefix)) != 0 ||
        !(leaf = log_path + wcslen(expected_prefix)) || !leaf[0] ||
        wcschr(leaf, L'\\') || wcschr(leaf, L'/') ||
        WideCharToMultiByte(CP_ACP, 0, nonce, -1, nonce_ascii,
                            sizeof(nonce_ascii), NULL, NULL) <= 0 ||
        _snprintf(nonce_marker, sizeof(nonce_marker) - 1,
                  "TRITON9-RUN nonce=%s", nonce_ascii) <= 0) {
        return FALSE;
    }
    nonce_marker[sizeof(nonce_marker) - 1] = '\0';
    file = CreateFileW(log_path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) ||
        size.QuadPart <= 0 || size.QuadPart > MAX_PROBE_RESULT_BYTES) {
        goto done;
    }
    data = (CHAR *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    if (!data || !ReadFile(file, data, (DWORD)size.QuadPart,
                           &bytes_read, NULL) ||
        bytes_read != (DWORD)size.QuadPart) {
        goto done;
    }
    ok = buffer_contains_ascii_case_insensitive(
             data, bytes_read, nonce_marker) &&
         buffer_contains_ascii_case_insensitive(
             data, bytes_read, "TRITON9-PROBE PASS") &&
         buffer_contains_ascii_case_insensitive(
             data, bytes_read,
             "TRITON9-AERO API-PATH PASS VISUAL-UNVERIFIED");

done:
    if (data) HeapFree(GetProcessHeap(), 0, data);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static BOOL bcd_current_has_option(const CHAR *option, BOOL *present)
{
    WCHAR system[MAX_PATH], application[MAX_DEPLOY_PATH], command[2048];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES security;
    HANDLE pipe_read = NULL, pipe_write = NULL;
    CHAR output[32 * 1024];
    DWORD bytes_read = 0, chunk = 0, exit_code = ERROR_GEN_FAILURE;
    BOOL ok = FALSE;

    *present = FALSE;
    ZeroMemory(&security, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(&pipe_read, &pipe_write, &security, 0) ||
        !SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0) ||
        !GetSystemDirectoryW(system, MAX_PATH) ||
        !format_wstr(application, MAX_DEPLOY_PATH, L"%ls\\bcdedit.exe", system) ||
        !format_wstr(command, 2048, L"\"%ls\" /enum {current}", application)) {
        goto done;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = pipe_write;
    startup.hStdError = pipe_write;
    if (!CreateProcessW(application, command, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        goto done;
    }
    CloseHandle(pipe_write);
    pipe_write = NULL;
    if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        SetLastError(ERROR_TIMEOUT);
    } else if (GetExitCodeProcess(process.hProcess, &exit_code) &&
               exit_code == ERROR_SUCCESS) {
        while (bytes_read < sizeof(output) &&
               ReadFile(pipe_read, output + bytes_read,
                        sizeof(output) - bytes_read, &chunk, NULL) && chunk) {
            bytes_read += chunk;
        }
        *present = buffer_contains_ascii_case_insensitive(
            output, bytes_read, option) ||
            buffer_contains_utf16le_ascii_case_insensitive(
                output, bytes_read, option);
        ok = TRUE;
    } else {
        SetLastError(exit_code);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
done:
    if (pipe_read) CloseHandle(pipe_read);
    if (pipe_write) CloseHandle(pipe_write);
    return ok;
}

static BOOL bcd_current_has_safeboot(BOOL *has_safeboot)
{
    return bcd_current_has_option("safeboot", has_safeboot);
}

static BOOL enable_shutdown_privilege(void)
{
    HANDLE token;
    TOKEN_PRIVILEGES privileges;
    DWORD error;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) return FALSE;
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME,
                               &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL)) {
        error = GetLastError();
        CloseHandle(token);
        SetLastError(error);
        return FALSE;
    }
    error = GetLastError();
    CloseHandle(token);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

static BOOL request_reboot(const WCHAR *reason)
{
    if (!enable_shutdown_privilege()) return FALSE;
    return InitiateSystemShutdownExW(NULL, (LPWSTR)reason, 2, TRUE, TRUE,
        SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
        SHTDN_REASON_MINOR_RECONFIG |
        SHTDN_REASON_FLAG_PLANNED);
}

static BOOL clear_safe_mode(void)
{
    DWORD attempt = 0;
    for (;;) {
        BOOL has_safeboot = TRUE;
        if (bcd_current_has_safeboot(&has_safeboot) && !has_safeboot) {
            return TRUE;
        }
        if (run_process_and_wait(L"/deletevalue {current} safeboot", 30000)) {
            return TRUE;
        }
        if (bcd_current_has_safeboot(&has_safeboot) && !has_safeboot) {
            return TRUE;
        }
        emit_status(L"CLEAR_SAFE_MODE_RETRY attempt=%lu error=%lu",
                    (unsigned long)(attempt + 1), (unsigned long)GetLastError());
        ++attempt;
        if (g_stop_event &&
            WaitForSingleObject(g_stop_event, 5000) != WAIT_TIMEOUT) {
            return FALSE;
        }
    }
}

static BOOL set_safe_mode(void)
{
    DWORD attempt = 0;
    for (;;) {
        BOOL has_safeboot = FALSE;
        if (bcd_current_has_safeboot(&has_safeboot) && has_safeboot)
            return TRUE;
        if (run_process_and_wait(L"/set {current} safeboot minimal", 30000) &&
            bcd_current_has_safeboot(&has_safeboot) && has_safeboot) {
            return TRUE;
        }
        emit_status(L"SET_SAFE_MODE_RETRY attempt=%lu error=%lu",
                    (unsigned long)(attempt + 1),
                    (unsigned long)GetLastError());
        ++attempt;
        if (g_stop_event &&
            WaitForSingleObject(g_stop_event, 5000) != WAIT_TIMEOUT) {
            return FALSE;
        }
    }
}

static void request_reboot_until_stopping(const WCHAR *reason, const WCHAR *phase)
{
    DWORD attempt = 0;
    for (;;) {
        if (request_reboot(reason)) return;
        ++attempt;
        emit_status(L"REBOOT_RETRY phase=%ls attempt=%lu error=%lu", phase,
                    (unsigned long)attempt, (unsigned long)GetLastError());
        if (g_stop_event &&
            WaitForSingleObject(g_stop_event, 5000) != WAIT_TIMEOUT) return;
    }
}

/* Result is the last durable value.  SafeBoot is cleared only after this
 * complete activation identity and its volatile same-boot guard exist. */
static BOOL commit_safe_activation_return_state(const WCHAR *deployment_id)
{
    return write_state(L"ActivationGuardId", deployment_id) &&
           write_state(L"InstallOutcome", L"OK") &&
           write_state(L"ActivationState", L"REBOOT_REQUESTED") &&
           write_state(L"Result", L"RETURNING_NORMAL_PENDING_VERIFY");
}

static void process_safe_mode(void)
{
    DeployMedia media;
    WCHAR attempt_id[65], result[MAX_DEPLOY_VALUE];
    WCHAR owned[8], outcome[MAX_DEPLOY_VALUE];
    DWORD wait_count, install_error = ERROR_SUCCESS;
    BOOL installed = FALSE;

    read_state(L"AttemptId", attempt_id, 65);
    read_state(L"Result", result, MAX_DEPLOY_VALUE);
    read_state(L"SafeModeOwned", owned, 8);
    read_state(L"InstallOutcome", outcome, MAX_DEPLOY_VALUE);
    if (!attempt_id[0] || wcscmp(owned, L"1") != 0) {
        emit_status(L"SAFE_IDLE no-service-owned-deployment");
        return;
    }
    emit_status(L"SAFE_BEGIN id=%ls", attempt_id);

    if (wcsncmp(result, L"RETURNING_NORMAL", 16) == 0) {
        if (!clear_safe_mode()) {
            emit_status(L"FATAL_SAFE_MODE_STICKY id=%ls", attempt_id);
            return;
        }
        request_reboot_until_stopping(
            L"Triton driver deployment is returning to normal mode.",
            L"resume-to-normal");
        return;
    }

    if (wcscmp(outcome, L"OK") == 0) {
        installed = TRUE;
    } else {
        ZeroMemory(&media, sizeof(media));
        for (wait_count = 0; wait_count < 120; ++wait_count) {
            if (find_deploy_media(&media)) break;
            if (WaitForSingleObject(g_stop_event, 1000) != WAIT_TIMEOUT) return;
        }
        if (!media.id[0]) {
            write_state(L"InstallOutcome", L"FAILED_MEDIA_MISSING");
            emit_status(L"INSTALL_FAIL id=%ls step=media-missing", attempt_id);
        } else if (_wcsicmp(media.id, attempt_id) != 0) {
            write_state(L"InstallOutcome", L"FAILED_MEDIA_CHANGED");
            emit_status(L"INSTALL_FAIL id=%ls step=media-changed actual=%ls",
                        attempt_id, media.id);
        } else if (!verify_deployment_media(&media)) {
            write_state(L"InstallOutcome", L"FAILED_VERIFY_SAFE");
        } else {
            write_state(L"Result", L"INSTALLING_SAFE");
            installed = install_driver(&media, &install_error);
            if (g_stop_event &&
                WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
                return;
            }
            if (installed) {
                write_state(L"InstallOutcome", L"OK");
            } else {
                WCHAR failure[64];
                format_wstr(failure, 64, L"FAILED_INSTALL_%lu",
                            (unsigned long)install_error);
                write_state(L"InstallOutcome", failure);
            }
        }
    }
    if (!installed) {
        emit_status(L"SAFE_INSTALL_BLOCKED id=%ls error=%lu",
                    attempt_id, (unsigned long)install_error);
        return;
    }
    if (!create_boot_guard(ACTIVATION_BOOT_GUARD_KEY, attempt_id) ||
        !commit_safe_activation_return_state(attempt_id)) {
        emit_status(L"ACTIVATION_STATE_FAIL id=%ls error=%lu",
                    attempt_id, (unsigned long)GetLastError());
        return;
    }
    if (!clear_safe_mode()) {
        emit_status(L"FATAL_SAFE_MODE_STICKY id=%ls", attempt_id);
        return;
    }
    emit_status(L"DEPLOY_COMPLETE id=%ls", attempt_id);
    request_reboot_until_stopping(
        L"Triton driver deployment completed.", L"to-normal");
}

/* ActivationState is written last. Any committed activation-reboot request
 * therefore has both its durable identity and its volatile same-boot guard. */
static BOOL commit_activation_reboot_state(const WCHAR *deployment_id)
{
    return write_state(L"ActivationGuardId", deployment_id) &&
           write_state(L"Result", L"REBOOTING_NORMAL_PENDING_VERIFY") &&
           write_state(L"InstallOutcome", L"OK") &&
           write_state(L"ActivationState", L"REBOOT_REQUESTED");
}

/* Normal mode authenticates the immutable optical package and arms SafeBoot.
 * It never changes a live graphics generation.  The Safe Mode service owns
 * replacement, byte verification, the return boot, and recovery after an
 * interruption. */
static BOOL install_normal_mode(const DeployMedia *media)
{
    if (!verify_deployment_media(media)) {
        write_state(L"AttemptId", media->id);
        write_state(L"SafeModeOwned", L"0");
        write_state(L"Result", L"FAILED_VERIFY");
        return FALSE;
    }
    if (!write_state(L"AttemptId", media->id) ||
        !write_state(L"SafeModeOwned", L"0") ||
        !write_state(L"Result", L"PREPARING_SAFE_MODE") ||
        !write_state(L"InstallOutcome", L"PENDING") ||
        !write_state(L"ActivationGuardId", L"PENDING") ||
        !write_state(L"ActivationState", L"INSTALLING") ||
        !write_state(L"ProbeLaunchId", L"PENDING") ||
        !write_state(L"ReprobeState", L"PENDING_FIRST") ||
        !write_state(L"ReprobeGuardId", L"PENDING") ||
        !write_state(L"VerifiedSuccessId", L"PENDING") ||
        !write_state(L"SafeModeOwned", L"1")) return FALSE;
    if (!configure_boot_integrity()) {
        write_state(L"Result", L"FAILED_BOOT_INTEGRITY_CONFIGURATION");
        return FALSE;
    }
    if (!set_safe_mode())
        return FALSE;
    if (!write_state(L"Result", L"ARMED_SAFE_MODE"))
        return FALSE;
    emit_status(L"REBOOT_TO_SAFE_MODE id=%ls", media->id);
    request_reboot_until_stopping(
        L"Triton will install its verified display driver in Safe Mode.",
        L"to-safe");
    return TRUE;
}

/* ReprobeState is the commit marker and is written last.  Therefore, any
 * REBOOT_REQUESTED state also has the deployment identity, pending launch
 * state, and result record needed to resume after a real reboot. */
static BOOL commit_reprobe_reboot_state(const WCHAR *deployment_id)
{
    return write_state(L"ReprobeGuardId", deployment_id) &&
           write_state(L"ProbeLaunchId", L"PENDING") &&
           write_state(L"Result", L"REBOOTING_FOR_REPROBE") &&
           write_state(L"ReprobeState", L"REBOOT_REQUESTED");
}

/* Initial PnP binding is separate from safe-mode byte replacement. The
 * fresh base has only Standard VGA; copying a SYS cannot associate a devnode.
 * Read the enumerated PCI service, rather than trusting a deployment marker. */
static BOOL gpu_has_triton_binding(void)
{
    HKEY pci, device, instance;
    DWORD i, j, length, bytes, type;
    WCHAR name[256], child[256], service[256];
    BOOL bound = FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, &pci)
            != ERROR_SUCCESS) return FALSE;
    for (i = 0; ; ++i) {
        length = 256;
        if (RegEnumKeyExW(pci, i, name, &length, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS) break;
        if (_wcsnicmp(name, L"VEN_1AF4&DEV_1050", 17) != 0) continue;
        if (RegOpenKeyExW(pci, name, 0, KEY_READ, &device) != ERROR_SUCCESS)
            continue;
        for (j = 0; ; ++j) {
            length = 256;
            if (RegEnumKeyExW(device, j, child, &length, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS) break;
            if (RegOpenKeyExW(device, child, 0, KEY_READ, &instance)
                    != ERROR_SUCCESS) continue;
            bytes = sizeof(service); service[0] = 0;
            if (RegQueryValueExW(instance, L"Service", NULL, &type,
                    (BYTE *)service, &bytes) == ERROR_SUCCESS && type == REG_SZ) {
                service[255] = 0;
                emit_status(L"GPU_BINDING device=%ls instance=%ls service=%ls",
                            name, child, service);
                if (_wcsicmp(service, L"VioGpu3D") == 0) bound = TRUE;
            }
            RegCloseKey(instance);
        }
        RegCloseKey(device);
    }
    RegCloseKey(pci);
    return bound;
}

static void emit_setupapi_tail(void)
{
    WCHAR path[MAX_DEPLOY_PATH], windows[MAX_PATH], line[1024];
    CHAR buffer[16385], *start, *end;
    HANDLE file;
    LARGE_INTEGER size, offset;
    DWORD bytes;
    if (!GetWindowsDirectoryW(windows, MAX_PATH) ||
        !format_wstr(path, MAX_DEPLOY_PATH, L"%ls\\inf\\setupapi.dev.log", windows)) return;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                      FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return; }
    offset.QuadPart = size.QuadPart > 16384 ? size.QuadPart - 16384 : 0;
    SetFilePointerEx(file, offset, NULL, FILE_BEGIN);
    if (ReadFile(file, buffer, 16384, &bytes, NULL)) {
        buffer[bytes] = 0;
        start = buffer;
        while ((end = strchr(start, '\n')) != NULL) {
            *end = 0;
            if (MultiByteToWideChar(CP_ACP, 0, start, -1, line, 1024))
                emit_status(L"SETUPAPI %ls", line);
            start = end + 1;
        }
    }
    CloseHandle(file);
}

static BOOL bind_initial_gpu(const DeployMedia *media)
{
    typedef BOOL (WINAPI *UpdateDriverFn)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
    HMODULE newdev;
    UpdateDriverFn update;
    WCHAR inf[MAX_DEPLOY_PATH], executable[MAX_DEPLOY_PATH], catalog[MAX_DEPLOY_PATH];
    WCHAR command[MAX_DEPLOY_PATH + 32];
    SC_HANDLE manager, service;
    BOOL ok, reboot = FALSE;
    DWORD error;
    if (!format_wstr(inf, MAX_DEPLOY_PATH, L"%ls\\%ls",
                     media->package_dir, media->inf)) return FALSE;
    if (format_wstr(catalog, MAX_DEPLOY_PATH, L"%ls\\%ls", media->package_dir, CATALOG_NAME)) {
        BOOL verified = verify_catalog_member(catalog, inf);
        emit_status(L"INF_CATALOG_VERIFY ok=%lu error=%lu", (unsigned long)verified,
                    (unsigned long)GetLastError());
        {
            const WCHAR *members[] = {L"viogpu3d.sys", L"neptune_d3d9.dll", L"neptune_d3d10.dll",
#if TRITON_DEPLOY_HAS_WOW64
                L"neptune_d3d9_wow.dll",
        L"neptune_d3d10_wow.dll",
#endif
                SERVICE_EXE_NAME, PROBE_EXE_NAME};
            WCHAR member[MAX_DEPLOY_PATH];
            DWORD index;
            for (index = 0; index < sizeof(members) / sizeof(members[0]); ++index) {
                if (!format_wstr(member, MAX_DEPLOY_PATH, L"%ls\\%ls",
                                 media->package_dir, members[index])) continue;
                verified = verify_catalog_member(catalog, member);
                emit_status(L"PE_CATALOG_VERIFY file=%ls ok=%lu error=%lu",
                            members[index], (unsigned long)verified,
                            (unsigned long)GetLastError());
            }
        }
    }
    newdev = LoadLibraryW(L"newdev.dll");
    if (!newdev) return FALSE;
    update = (UpdateDriverFn)(void *)GetProcAddress(newdev,
                                      "UpdateDriverForPlugAndPlayDevicesW");
    if (!update) { FreeLibrary(newdev); return FALSE; }
    emit_status(L"GPU_BIND_BEGIN id=%ls inf=%ls hardware=%ls",
                media->id, inf, media->hardware_id);
    /* FORCE | NONINTERACTIVE: the service must never leave an invisible UI. */
    ok = update(NULL, media->hardware_id, inf, 0x5, &reboot);
    error = GetLastError();
    FreeLibrary(newdev);
    emit_status(ok ? L"GPU_BIND_OK id=%ls reboot=%lu error=%lu" :
                     L"GPU_BIND_FAIL id=%ls reboot=%lu error=%lu",
                media->id, (unsigned long)reboot, (unsigned long)error);
    if (!ok) { emit_setupapi_tail(); return FALSE; }
    /* The legacy INF also installs this service into System32. Keep the
     * already-running standalone service's path so later updates replace the
     * same executable that SCM starts. Both copies have authenticated bytes. */
    if (!GetModuleFileNameW(NULL, executable, MAX_DEPLOY_PATH) ||
        !format_wstr(command, MAX_DEPLOY_PATH + 32,
                     L"\"%ls\" --service", executable)) return FALSE;
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) return FALSE;
    service = OpenServiceW(manager, SERVICE_NAME, SERVICE_CHANGE_CONFIG);
    ok = service && ChangeServiceConfigW(service, SERVICE_NO_CHANGE,
        SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, command, NULL, NULL, NULL,
        NULL, NULL, NULL);
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

static void process_normal_mode(void)
{
    DeployMedia media;
    WCHAR success_id[65], attempt_id[65], result[MAX_DEPLOY_VALUE];
    WCHAR verified_id[65], verified_this_boot[65] = L"";
    WCHAR probe_launch_id[65], post_verify_retry[8];
    WCHAR activation_state[MAX_DEPLOY_VALUE], activation_guard_id[65];
    WCHAR reprobe_state[MAX_DEPLOY_VALUE], reprobe_guard_id[65];
    WCHAR owned[8], outcome[MAX_DEPLOY_VALUE];
    ReprobeBootGuardState activation_guard_state, guard_state;
    DWORD probe_error;
    emit_status(L"NORMAL_READY");
    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        read_state(L"SafeModeOwned", owned, 8);
        read_state(L"Result", result, MAX_DEPLOY_VALUE);
        if (wcscmp(owned, L"1") == 0) {
            if (wcsncmp(result, L"RETURNING_NORMAL", 16) == 0) {
                read_state(L"InstallOutcome", outcome, MAX_DEPLOY_VALUE);
                write_state(L"Result", wcscmp(outcome, L"OK") == 0 ?
                            L"NORMAL_PENDING_VERIFY" : outcome);
                write_state(L"SafeModeOwned", L"0");
                emit_status(L"NORMAL_RETURN outcome=%ls", outcome);
            } else {
                emit_status(L"NORMAL_RETRY_SAFE_MODE state=%ls", result);
                if (!set_safe_mode()) return;
                request_reboot_until_stopping(
                    L"Triton is resuming an interrupted Safe Mode deployment.",
                    L"resume-to-safe");
                return;
            }
        }
        ZeroMemory(&media, sizeof(media));
        if (find_deploy_media(&media)) {
            read_state(L"LastSuccessId", success_id, 65);
            read_state(L"VerifiedSuccessId", verified_id, 65);
            read_state(L"AttemptId", attempt_id, 65);
            read_state(L"InstallOutcome", outcome, MAX_DEPLOY_VALUE);
            read_state(L"Result", result, MAX_DEPLOY_VALUE);
            if (_wcsicmp(success_id, media.id) == 0 ||
                _wcsicmp(verified_id, media.id) == 0 ||
                (_wcsicmp(attempt_id, media.id) == 0 &&
                 wcscmp(outcome, L"OK") == 0)) {
                read_state(L"ActivationState", activation_state,
                           MAX_DEPLOY_VALUE);
                read_state(L"ActivationGuardId", activation_guard_id, 65);
                activation_guard_state = query_boot_guard(
                    ACTIVATION_BOOT_GUARD_KEY, media.id);
                /* The activation guard is created before the initial driver
                 * reboot. If it still exists, a service restart occurred on
                 * the same boot and cannot produce a post-reboot commit. */
                if (activation_guard_state == REPROBE_GUARD_MATCH) {
                    if (!commit_activation_reboot_state(media.id)) {
                        emit_status(L"ACTIVATION_STATE_FAIL id=%ls error=%lu",
                                    media.id, (unsigned long)GetLastError());
                        if (WaitForSingleObject(g_stop_event, 5000) !=
                            WAIT_TIMEOUT) break;
                        continue;
                    }
                    emit_status(L"ACTIVATION_REBOOT_RETRY id=%ls", media.id);
                    request_reboot_until_stopping(
                        L"Triton must activate its verified driver package.",
                        L"activation-same-boot");
                    return;
                }
                if (activation_guard_state == REPROBE_GUARD_ERROR ||
                    activation_guard_state == REPROBE_GUARD_MISMATCH) {
                    DWORD activation_error = GetLastError();
                    write_state(L"Result",
                                L"ACTIVATION_GUARD_RECOVERY_REBOOT");
                    emit_status(L"ACTIVATION_GUARD_FAIL id=%ls state=%d error=%lu",
                                media.id, (int)activation_guard_state,
                                (unsigned long)activation_error);
                    request_reboot_until_stopping(
                        L"Triton must clear an invalid volatile activation guard.",
                        L"activation-guard-recovery");
                    return;
                }
                /* The package can replace an older deployment service on its
                 * activation reboot. That older binary has no ActivationState.
                 * Do not accept the boot retroactively: arm the new volatile
                 * protocol and require one additional guest-owned reboot. */
                if (!activation_state[0]) {
                    if (!create_boot_guard(ACTIVATION_BOOT_GUARD_KEY,
                                           media.id) ||
                        !commit_activation_reboot_state(media.id)) {
                        emit_status(L"ACTIVATION_STATE_FAIL id=%ls error=%lu",
                                    media.id, (unsigned long)GetLastError());
                        if (WaitForSingleObject(g_stop_event, 5000) !=
                            WAIT_TIMEOUT) break;
                        continue;
                    }
                    emit_status(L"ACTIVATION_MIGRATION_REBOOT id=%ls",
                                media.id);
                    request_reboot_until_stopping(
                        L"Triton must establish its verified activation boundary.",
                        L"activation-migration");
                    return;
                }
                if ((wcscmp(activation_state, L"REBOOT_REQUESTED") == 0 &&
                     _wcsicmp(activation_guard_id, media.id) != 0) ||
                    (wcscmp(activation_state, L"REBOOT_REQUESTED") != 0 &&
                     wcscmp(activation_state, L"ACTIVATED") != 0)) {
                    write_state(L"Result", L"FAILED_ACTIVATION_BOOT_GUARD");
                    emit_status(L"ACTIVATION_GUARD_FAIL id=%ls state=absent persistent=%ls phase=%ls",
                                media.id, activation_guard_id,
                                activation_state);
                    if (WaitForSingleObject(g_stop_event, 5000) !=
                        WAIT_TIMEOUT) break;
                    continue;
                }
                read_state(L"ReprobeState", reprobe_state,
                           MAX_DEPLOY_VALUE);
                read_state(L"ReprobeGuardId", reprobe_guard_id, 65);
                guard_state = query_boot_guard(REPROBE_BOOT_GUARD_KEY,
                                               media.id);

                /* A matching volatile guard proves that Vista has not
                 * rebooted since the first successful probe. Repair any
                 * interrupted persistent transition, then retry only the
                 * guest-owned reboot. Do this before another byte-verification
                 * commit can be emitted. */
                if (guard_state == REPROBE_GUARD_MATCH &&
                    (wcscmp(reprobe_state, L"PENDING_FIRST") == 0 ||
                     wcscmp(reprobe_state, L"REBOOT_REQUESTED") == 0)) {
                    BOOL transition_was_pending =
                        wcscmp(reprobe_state, L"PENDING_FIRST") == 0;
                    if (!commit_reprobe_reboot_state(media.id)) {
                        emit_status(L"REPROBE_STATE_FAIL id=%ls error=%lu",
                                    media.id, (unsigned long)GetLastError());
                        if (WaitForSingleObject(g_stop_event, 5000) !=
                            WAIT_TIMEOUT) break;
                        continue;
                    }
                    emit_status(transition_was_pending ?
                        L"REPROBE_REBOOT id=%ls" :
                        L"REPROBE_REBOOT_RETRY id=%ls", media.id);
                    request_reboot_until_stopping(
                        L"Triton will repeat its verified graphics probe.",
                        L"reprobe-same-boot");
                    return;
                }
                if (guard_state == REPROBE_GUARD_ERROR ||
                    guard_state == REPROBE_GUARD_MISMATCH) {
                    DWORD guard_error = GetLastError();
                    write_state(L"Result", L"REPROBE_GUARD_RECOVERY_REBOOT");
                    emit_status(L"REPROBE_GUARD_FAIL id=%ls state=%d error=%lu",
                                media.id, (int)guard_state,
                                (unsigned long)guard_error);
                    request_reboot_until_stopping(
                        L"Triton must clear an invalid volatile reprobe guard.",
                        L"reprobe-guard-recovery");
                    return;
                }
                /* REBOOT_REQUESTED is accepted on a later boot only when the
                 * durable identity says that this exact package created the
                 * now-absent volatile guard. Legacy or partial state cannot
                 * manufacture a second probe. */
                if (wcscmp(reprobe_state, L"REBOOT_REQUESTED") == 0 &&
                    _wcsicmp(reprobe_guard_id, media.id) != 0) {
                    write_state(L"Result", L"FAILED_REPROBE_BOOT_GUARD");
                    emit_status(L"REPROBE_GUARD_FAIL id=%ls state=absent persistent=%ls",
                                media.id, reprobe_guard_id);
                    if (WaitForSingleObject(g_stop_event, 5000) !=
                        WAIT_TIMEOUT) break;
                    continue;
                }
                /* Staging before a reboot is not success. Authenticate the
                 * immutable package again and compare every installed byte
                 * before the public probe can start. Repeat once per service
                 * process so a later guest-owned reboot is checked too. */
                if (_wcsicmp(verified_this_boot, media.id) != 0) {
                    if (verify_deployment_media(&media) &&
                        verify_installed_payloads(&media)) {
                        if (!gpu_has_triton_binding()) {
                            if (!bind_initial_gpu(&media)) {
                                write_state(L"Result", L"FAILED_INITIAL_GPU_BIND");
                                if (WaitForSingleObject(g_stop_event, 30000) !=
                                    WAIT_TIMEOUT) break;
                                continue;
                            }
                            request_reboot_until_stopping(
                                L"Triton initial GPU binding", L"GPU_BIND_REBOOT");
                            return;
                        }
                        if (!write_state(L"PostVerifyRetry", L"0") ||
                            !write_state(L"Result", L"SUCCESS") ||
                            !write_state(L"VerifiedSuccessId", media.id) ||
                            !write_state(L"ActivationState", L"ACTIVATED") ||
                            !write_state(L"LastSuccessId", media.id)) {
                            emit_status(L"POST_REBOOT_STATE_FAIL id=%ls error=%lu",
                                        media.id,
                                        (unsigned long)GetLastError());
                            if (WaitForSingleObject(g_stop_event, 5000) !=
                                WAIT_TIMEOUT) break;
                            continue;
                        }
                        copy_wstr(verified_this_boot, 65, media.id);
                        emit_status(L"POST_REBOOT_COMMIT id=%ls", media.id);
                    } else {
                        read_state(L"PostVerifyRetry", post_verify_retry, 8);
                        emit_status(L"POST_REBOOT_VERIFY_FAIL id=%ls retry=%ls error=%lu",
                                    media.id, post_verify_retry,
                                    (unsigned long)GetLastError());
                        if (wcscmp(post_verify_retry, L"1") != 0) {
                            write_state(L"PostVerifyRetry", L"1");
                            write_state(L"Result", L"RETRYING_POST_REBOOT_VERIFY");
                            if (install_normal_mode(&media)) return;
                        }
                        write_state(L"Result", L"FAILED_POST_REBOOT_VERIFY");
                        if (WaitForSingleObject(g_stop_event, 5000) != WAIT_TIMEOUT)
                            break;
                        continue;
                    }
                }
                /* Verified bytes still do not prove the one-shot public D3D9
                 * probe launched, so start it exactly once for this id. A
                 * package installed by an older service has no reprobe state;
                 * initialize a new two-proof sequence here. */
                if (wcscmp(reprobe_state, L"REBOOT_REQUESTED") != 0 &&
                    wcscmp(reprobe_state, L"PENDING_FIRST") != 0 &&
                    wcscmp(reprobe_state, L"SECOND_LAUNCHED") != 0) {
                    if (!write_state(L"ReprobeGuardId", L"PENDING") ||
                        !write_state(L"ProbeLaunchId", L"PENDING") ||
                        !write_state(L"ReprobeState", L"PENDING_FIRST")) {
                        emit_status(L"REPROBE_STATE_FAIL id=%ls error=%lu",
                                    media.id, (unsigned long)GetLastError());
                        if (WaitForSingleObject(g_stop_event, 5000) !=
                            WAIT_TIMEOUT) break;
                        continue;
                    }
                    copy_wstr(reprobe_state, MAX_DEPLOY_VALUE,
                              L"PENDING_FIRST");
                }
                read_state(L"ProbeLaunchId", probe_launch_id, 65);
                if (_wcsicmp(probe_launch_id, media.id) != 0) {
                    if (launch_optional_probe(&media, &probe_error)) {
                        if (!write_state(L"ProbeLaunchId", media.id)) {
                            emit_status(L"PROBE_STATE_FAIL id=%ls error=%lu",
                                        media.id,
                                        (unsigned long)GetLastError());
                        } else if (wcscmp(reprobe_state,
                                          L"REBOOT_REQUESTED") == 0) {
                            if (!write_state(L"ReprobeState",
                                             L"SECOND_LAUNCHED")) {
                                emit_status(L"PROBE_STATE_FAIL id=%ls error=%lu",
                                            media.id,
                                            (unsigned long)GetLastError());
                            } else {
                                emit_status(L"REPROBE_LAUNCHED id=%ls",
                                            media.id);
                                emit_status(L"PROBE_LAUNCHED id=%ls",
                                            media.id);
                            }
                        } else {
                            emit_status(L"PROBE_LAUNCHED id=%ls", media.id);
                        }
                    } else {
                        emit_status(L"PROBE_LAUNCH_FAIL id=%ls error=%lu",
                                    media.id, (unsigned long)probe_error);
                    }
                } else if (wcscmp(reprobe_state, L"REBOOT_REQUESTED") == 0) {
                    /* Repair a crash after the second StartService and
                     * ProbeLaunchId commit but before its phase commit. */
                    if (write_state(L"ReprobeState", L"SECOND_LAUNCHED")) {
                            emit_status(L"REPROBE_LAUNCHED id=%ls", media.id);
                        emit_status(L"PROBE_LAUNCHED id=%ls", media.id);
                    } else {
                        emit_status(L"PROBE_STATE_FAIL id=%ls error=%lu",
                                    media.id, (unsigned long)GetLastError());
                    }
                }
                read_state(L"ProbeLaunchId", probe_launch_id, 65);
                read_state(L"ReprobeState", reprobe_state,
                           MAX_DEPLOY_VALUE);
                if (_wcsicmp(probe_launch_id, media.id) == 0 &&
                    wcscmp(reprobe_state, L"PENDING_FIRST") == 0 &&
                    probe_log_has_completed_success()) {
                    if (!create_boot_guard(REPROBE_BOOT_GUARD_KEY, media.id) ||
                        !commit_reprobe_reboot_state(media.id)) {
                        emit_status(L"REPROBE_STATE_FAIL id=%ls error=%lu",
                                    media.id, (unsigned long)GetLastError());
                        if (WaitForSingleObject(g_stop_event, 5000) !=
                            WAIT_TIMEOUT) break;
                        continue;
                    }
                    emit_status(L"REPROBE_REBOOT id=%ls", media.id);
                    request_reboot_until_stopping(
                        L"Triton will repeat its verified graphics probe.",
                        L"reprobe");
                    return;
                }
            } else if (_wcsicmp(attempt_id, media.id) == 0 &&
                       wcsncmp(result, L"ARMED", 5) == 0) {
                /* The BCD change is durable. Retry only the guest reboot. */
                emit_status(L"REBOOT_TO_SAFE_MODE_RETRY id=%ls", media.id);
                request_reboot_until_stopping(
                    L"Triton will install a verified display driver in Safe Mode.",
                    L"retry-to-safe");
                return;
            } else if (_wcsicmp(attempt_id, media.id) == 0 &&
                       wcsncmp(result, L"FAILED", 6) == 0) {
                /* A failed immutable build is not retried forever. */
            } else if (_wcsicmp(attempt_id, media.id) == 0 &&
                       wcscmp(result, L"INSTALLING_NORMAL") == 0) {
                /* Migrate a legacy normal-mode attempt into the crash-safe
                 * Safe Mode replacement protocol. */
                emit_status(L"INSTALL_MIGRATE_TO_SAFE id=%ls", media.id);
                if (install_normal_mode(&media)) return;
            } else {
                /* Stop the service immediately once it owns a reboot so SCM
                 * can complete the transition without a second RPC request. */
                write_state(L"PostVerifyRetry", L"0");
                if (install_normal_mode(&media)) return;
            }
        }
        if (WaitForSingleObject(g_stop_event, 5000) != WAIT_TIMEOUT) break;
    }
}

static DWORD WINAPI service_control(DWORD control, DWORD event_type,
                                    LPVOID event_data, LPVOID context)
{
    (void)event_type;
    (void)event_data;
    (void)context;
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_status.dwControlsAccepted = 0;
        SetServiceStatus(g_status_handle, &g_status);
        if (g_stop_event) SetEvent(g_stop_event);
    }
    return NO_ERROR;
}

/* Keep the deployment control plane independent of USER32.  The conventional
 * SM_CLEANBOOT query looks harmless, but it enters the interactive/display path
 * before this service has emitted its first durable breadcrumb.  A display
 * UMD failure can therefore hide both recovery and the public D3D9 probe.
 * SafeBoot publishes the same mode in the SYSTEM hive, which is available to
 * an auto-start service in normal and Safe Mode without activating graphics. */
static BOOL is_safe_boot(void)
{
    HKEY key = NULL;
    DWORD type = 0, value = 0, bytes = sizeof(value);
    LONG status;

    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Option",
                           0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS)
        return FALSE;
    status = RegQueryValueExW(key, L"OptionValue", NULL, &type,
                              (BYTE *)&value, &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD &&
           bytes == sizeof(value) && value != 0;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    (void)argc;
    (void)argv;
    ZeroMemory(&g_status, sizeof(g_status));
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    g_status.dwWaitHint = 30000;
    g_status_handle = RegisterServiceCtrlHandlerExW(SERVICE_NAME,
                                                    service_control, NULL);
    if (!g_status_handle) return;
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop_event) {
        g_status.dwCurrentState = SERVICE_STOPPED;
        g_status.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_status_handle, &g_status);
        return;
    }
    g_status.dwCurrentState = SERVICE_RUNNING;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(g_status_handle, &g_status);

    if (is_safe_boot()) process_safe_mode();
    else process_normal_mode();

    g_status.dwCurrentState = SERVICE_STOPPED;
    g_status.dwControlsAccepted = 0;
    SetServiceStatus(g_status_handle, &g_status);
    CloseHandle(g_stop_event);
}

static BOOL service_destination_writable(const WCHAR *destination, BOOL allow_missing)
{
    DWORD attributes = GetFileAttributesW(destination);

    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return allow_missing && (error == ERROR_FILE_NOT_FOUND ||
                                 error == ERROR_PATH_NOT_FOUND);
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        attributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileAttributesW(destination, attributes ? attributes : FILE_ATTRIBUTE_NORMAL))
            return FALSE;
    }
    return TRUE;
}

static BOOL install_self(void)
{
    WCHAR current[MAX_DEPLOY_PATH], destination[MAX_DEPLOY_PATH];
    WCHAR program_files[MAX_DEPLOY_PATH], directory[MAX_DEPLOY_PATH];
    WCHAR service_command[MAX_DEPLOY_PATH + 32];
    SC_HANDLE manager = NULL, service = NULL;
    SERVICE_DESCRIPTIONW description;
    BOOL ok = FALSE;

    if (!GetModuleFileNameW(NULL, current, MAX_DEPLOY_PATH) ||
        !ExpandEnvironmentStringsW(L"%ProgramFiles%", program_files,
                                   MAX_DEPLOY_PATH) ||
        !format_wstr(directory, MAX_DEPLOY_PATH, L"%ls\\TritonVistaDeploy",
                     program_files) ||
        !format_wstr(destination, MAX_DEPLOY_PATH, L"%ls\\%ls", directory,
                     SERVICE_EXE_NAME)) return FALSE;
    CreateDirectoryW(directory, NULL);
    if (_wcsicmp(current, destination) != 0 && !files_equal(current, destination)) {
        /* Only the fixed service path constructed above is made writable.
         * CopyFile also carries the optical source's read-only attribute. */
        if (!service_destination_writable(destination, TRUE) ||
            !CopyFileW(current, destination, FALSE)) return FALSE;
    }
    if (!service_destination_writable(destination, FALSE)) return FALSE;
    if (!format_wstr(service_command, MAX_DEPLOY_PATH + 32, L"\"%ls\" --service",
                     destination)) return FALSE;
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!manager) goto done;
    service = OpenServiceW(manager, SERVICE_NAME,
                           SERVICE_CHANGE_CONFIG | SERVICE_START | DELETE);
    if (service) {
        if (!ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, service_command, NULL,
            NULL, NULL, L"LocalSystem", NULL, SERVICE_DISPLAY_NAME)) goto done;
    } else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        service = CreateServiceW(manager, SERVICE_NAME, SERVICE_DISPLAY_NAME,
            SERVICE_CHANGE_CONFIG | SERVICE_START | DELETE,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            service_command, NULL, NULL, NULL, L"LocalSystem", NULL);
        if (!service) goto done;
    } else goto done;
    description.lpDescription = (LPWSTR)SERVICE_DESCRIPTION;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
    if (!set_default_registry_value(HKEY_LOCAL_MACHINE, SAFEBOOT_MINIMAL, L"Service") ||
        !set_default_registry_value(HKEY_LOCAL_MACHINE, SAFEBOOT_NETWORK, L"Service")) {
        goto done;
    }
    write_state(L"Schema", L"1");
    if (!StartServiceW(service, 0, NULL) &&
        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) goto done;
    emit_status(L"SERVICE_INSTALLED path=%ls", destination);
    ok = TRUE;
done:
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    return ok;
}

static BOOL uninstall_self(void)
{
    SC_HANDLE manager = NULL, service = NULL;
    SERVICE_STATUS status;
    BOOL ok = TRUE;
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) return FALSE;
    service = OpenServiceW(manager, SERVICE_NAME, SERVICE_STOP | DELETE);
    if (service) {
        ControlService(service, SERVICE_CONTROL_STOP, &status);
        if (!DeleteService(service)) ok = FALSE;
        CloseServiceHandle(service);
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) ok = FALSE;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, SAFEBOOT_MINIMAL);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, SAFEBOOT_NETWORK);
    CloseServiceHandle(manager);
    return ok;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc == 3 && _wcsicmp(argv[1], L"--verify-media") == 0 &&
        ((argv[2][0] >= L'A' && argv[2][0] <= L'Z') ||
         (argv[2][0] >= L'a' && argv[2][0] <= L'z')) &&
        argv[2][1] == L'\0') {
        DeployMedia media;
        WCHAR drive = argv[2][0];
        if (drive >= L'a' && drive <= L'z') drive -= L'a' - L'A';
        if (!load_media_from_root(drive, &media) ||
            !verify_deployment_media(&media)) {
            fwprintf(stderr, L"Media verification failed on %c: (Win32 error %lu).\n",
                     drive, (unsigned long)GetLastError());
            return 1;
        }
        wprintf(L"Media verification passed for deployment %ls.\n", media.id);
        return 0;
    }
    if (argc == 2 && _wcsicmp(argv[1], L"--service") == 0) {
        SERVICE_TABLE_ENTRYW table[] = {
            {(LPWSTR)SERVICE_NAME, service_main},
            {NULL, NULL}
        };
        return StartServiceCtrlDispatcherW(table) ? 0 : (int)GetLastError();
    }
    if (argc == 1 || (argc == 2 && _wcsicmp(argv[1], L"--install") == 0)) {
        if (!install_self()) {
            fwprintf(stderr, L"Could not install %ls (Win32 error %lu).\n",
                     SERVICE_NAME, (unsigned long)GetLastError());
            return 1;
        }
        wprintf(L"%ls is installed and running.\n", SERVICE_NAME);
        return 0;
    }
    if (argc == 2 && _wcsicmp(argv[1], L"--uninstall") == 0) {
        return uninstall_self() ? 0 : 1;
    }
    fwprintf(stderr,
             L"Usage: %ls --install | --uninstall | --verify-media DRIVE_LETTER\n",
             argv[0]);
    return 2;
}
