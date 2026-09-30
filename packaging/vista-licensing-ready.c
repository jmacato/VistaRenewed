#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
/* Restore readiness, not activation: require both native protected-policy
 * queries to work. SCM RUNNING alone is insufficient on this Vista RTM VM. */
typedef HRESULT(WINAPI *PolicyFn)(LPCWSTR,DWORD*);
static FILE *log_file;
static void log_line(const char *format,...)
{
    va_list args;va_start(args,format);vprintf(format,args);va_end(args);
    if(log_file){va_start(args,format);vfprintf(log_file,format,args);va_end(args);fflush(log_file);}
}
static BOOL wait_state(SC_HANDLE service,DWORD target,DWORD timeout)
{
    DWORD start=GetTickCount();SERVICE_STATUS state;
    do{
        if(!QueryServiceStatus(service,&state))return FALSE;
        if(state.dwCurrentState==target)return TRUE;
        Sleep(250);
    }while(GetTickCount()-start<timeout);
    return FALSE;
}
static BOOL ready(PolicyFn policy)
{
    DWORD shell=0,search=0;
    HRESULT a=policy(L"shell32-EnableProxyFeature",&shell);
    HRESULT b=policy(L"WindowsSearchEngine-Licensing-SearchEnabled",&search);
    log_line("POLICY shell=0x%08lx/%lu search=0x%08lx/%lu\n",(DWORD)a,shell,(DWORD)b,search);
    return a==S_OK&&b==S_OK&&shell==1&&search==1;
}
int main(void)
{
    HANDLE mutex=CreateMutexW(NULL,FALSE,L"Global\\TritonLicensingReadiness");
    if(!mutex)return 1;
    if(WaitForSingleObject(mutex,0)!=WAIT_OBJECT_0){CloseHandle(mutex);return 0;}
    log_file=fopen("C:\\ProgramData\\TritonLicensing\\readiness.log","a");
    SYSTEMTIME now;GetSystemTime(&now);
    log_line("START %04u-%02u-%02uT%02u:%02u:%02uZ uptime_ms=%lu\n",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetTickCount());
    int result=1;
    HMODULE slc=LoadLibraryW(L"C:\\Windows\\System32\\slc.dll");
    PolicyFn policy=slc?(PolicyFn)(void*)GetProcAddress(slc,"SLGetWindowsInformationDWORD"):NULL;
    SC_HANDLE manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);
    SC_HANDLE service=manager?OpenServiceW(manager,L"slsvc",SERVICE_QUERY_STATUS|SERVICE_START|SERVICE_STOP):NULL;
    if(!policy||!service){log_line("Initialization failed error=%lu\n",GetLastError());goto done;}
    /* Automatic startup may still be in progress. */
    SERVICE_STATUS state;
    if(!QueryServiceStatus(service,&state))goto done;
    if(state.dwCurrentState==SERVICE_STOPPED){
        if(!StartServiceW(service,0,NULL)&&GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING)goto done;
    }
    if(!wait_state(service,SERVICE_RUNNING,30000)){log_line("Service did not reach RUNNING\n");goto done;}
    for(unsigned attempt=0;attempt<6;attempt++){
        if(ready(policy)){result=0;goto done;}
        Sleep(1000);
    }
    log_line("Service is RUNNING but its protected-policy cache is not ready; restarting once\n");
    if(!ControlService(service,SERVICE_CONTROL_STOP,&state) && GetLastError()!=ERROR_SERVICE_NOT_ACTIVE)goto done;
    if(!wait_state(service,SERVICE_STOPPED,30000))goto done;
    if(!StartServiceW(service,0,NULL)&&GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING)goto done;
    if(!wait_state(service,SERVICE_RUNNING,30000))goto done;
    for(unsigned attempt=0;attempt<20;attempt++){
        if(ready(policy)){result=0;goto done;}
        Sleep(1000);
    }
done:
    log_line("%s\n",result?"READINESS_FAILED":"LICENSING_READY");
    if(service)CloseServiceHandle(service);if(manager)CloseServiceHandle(manager);
    if(slc)FreeLibrary(slc);if(log_file)fclose(log_file);
    ReleaseMutex(mutex);CloseHandle(mutex);return result;
}
