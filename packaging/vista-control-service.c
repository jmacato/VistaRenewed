/* Host-only Vista test control. The QEMU Unix socket is the trust boundary. */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winsvc.h>
#include <sddl.h>
#include <wincrypt.h>
#include <errno.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#define NAME L"TritonVistaControl"
#define ROOT L"C:\\ProgramData\\TritonControl\\jobs"
#define LIMIT (1024u*1024u)
static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS service_status;
static HANDLE stop_event, port=INVALID_HANDLE_VALUE;
static struct { char id[33]; HANDLE process, job; DWORD start, timeout; } active;

static BOOL number(const char *s,DWORD *out) {
    char *end;unsigned long n;const char *p=s;
    if(!*p)return FALSE;
    for(;*p;p++)if(*p<'0'||*p>'9')return FALSE;
    errno=0;n=strtoul(s,&end,10);if(errno==ERANGE||*end)return FALSE;
    *out=(DWORD)n;return TRUE;
}
static BOOL payload_valid(const BYTE *data,DWORD len,const char *hex) {
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;BYTE bytes[32];DWORD size=32;char actual[65];unsigned i;BOOL ok=FALSE;
    if(strlen(hex)!=64)return FALSE;
    if(!CryptAcquireContextW(&provider,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))goto done;
    if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)||!CryptHashData(hash,data,len,0)||!CryptGetHashParam(hash,HP_HASHVAL,bytes,&size,0)||size!=32)goto done;
    for(i=0;i<32;i++)sprintf(actual+i*2,"%02x",bytes[i]);
    ok=!strcmp(actual,hex);
done:
    if(hash)CryptDestroyHash(hash);
    if(provider)CryptReleaseContext(provider,0);
    return ok;
}
static BOOL putfile(const WCHAR *path, const void *data, DWORD len) {
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD n=0; BOOL ok;
    if(f==INVALID_HANDLE_VALUE) return FALSE;
    ok=WriteFile(f,data,len,&n,NULL) && n==len && FlushFileBuffers(f);
    CloseHandle(f); return ok;
}
static void path_for(WCHAR *out,const char *id,const WCHAR *ext) {
    WCHAR wide[33]; MultiByteToWideChar(CP_UTF8,0,id,-1,wide,33);
    swprintf(out,MAX_PATH,L"%ls\\%ls.%ls",ROOT,wide,ext);
}
static BOOL valid_id(const char *id) {
    size_t i;if(strlen(id)!=32)return FALSE;
    for(i=0;i<32;i++)if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return FALSE;
    return TRUE;
}
static BOOL state_write(const char *id,const char *state,DWORD code) {
    WCHAR path[MAX_PATH],temp[MAX_PATH];char data[80];int n;
    path_for(path,id,L"state");path_for(temp,id,L"state.tmp");
    n=snprintf(data,sizeof(data),"%s %lu\n",state,(unsigned long)code);
    return putfile(temp,data,(DWORD)n)&&MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
static void finish_job(DWORD code) {
    /* Close kills any descendants that outlived cmd.exe. */
    if(active.job)CloseHandle(active.job);
    if(active.process)CloseHandle(active.process);
    state_write(active.id,"DONE",code);ZeroMemory(&active,sizeof(active));
}
static void poll_job(void) {
    DWORD code;
    if(!active.process)return;
    if(WaitForSingleObject(active.process,0)==WAIT_OBJECT_0) {
        if(!GetExitCodeProcess(active.process,&code))code=GetLastError();
        finish_job(code);
    } else if((DWORD)(GetTickCount()-active.start)>=active.timeout) {
        TerminateJobObject(active.job,ERROR_TIMEOUT);WaitForSingleObject(active.process,5000);finish_job(ERROR_TIMEOUT);
    }
}
static BOOL send_bytes(const void *data,DWORD len) {
    const BYTE *p=data;DWORD n;
    while(len) {
        if(!WriteFile(port,p,len,&n,NULL)||!n)return FALSE;
        p+=n;len-=n;
    }
    return TRUE;
}
static void reply(const char *id,const char *status,DWORD code,const void *data,DWORD len) {
    char head[160];int n=snprintf(head,sizeof(head),"TC1 %s %s %lu %lu\n",id,status,(unsigned long)code,(unsigned long)len);
    if(send_bytes(head,(DWORD)n)&&len)send_bytes(data,len);
}
static void error_reply(const char *id,DWORD code,const char *message) {
    reply(id,"ERROR",code,message,(DWORD)strlen(message));
}
static void job_status(const char *id) {
    WCHAR path[MAX_PATH];char data[80]={0},state[20];unsigned long code;DWORD got=0;HANDLE f;
    poll_job();
    if(active.process&&!strcmp(active.id,id)){reply(id,"RUNNING",0,NULL,0);return;}
    path_for(path,id,L"state");f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE){error_reply(id,GetLastError(),"job not found");return;}
    ReadFile(f,data,sizeof(data)-1,&got,NULL);CloseHandle(f);
    if(sscanf(data,"%19s %lu",state,&code)!=2){error_reply(id,ERROR_INVALID_DATA,"invalid saved state");return;}
    if(!strcmp(state,"DONE"))reply(id,"DONE",(DWORD)code,NULL,0);
    else if(!strcmp(state,"ERROR"))error_reply(id,(DWORD)code,"job launch failed");
    else error_reply(id,ERROR_PROCESS_ABORTED,"service restarted during job; it will not be rerun");
}
static WCHAR *utf8(const BYTE *data,DWORD len) {
    int n;WCHAR *out;
    if(memchr(data,0,len)){SetLastError(ERROR_INVALID_DATA);return NULL;}
    n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char*)data,(int)len,NULL,0);
    if(!n)return NULL;
    out=calloc((size_t)n+1,sizeof(WCHAR));if(!out){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char*)data,(int)len,out,n);return out;
}
static BOOL absolute_path(const WCHAR *s) {
    /* Local drive paths only; control never accesses a network share. */
    return s&&wcslen(s)>3&&((s[0]>=L'A'&&s[0]<=L'Z')||(s[0]>=L'a'&&s[0]<=L'z'))&&s[1]==L':'&&s[2]==L'\\';
}
static BOOL acl(const WCHAR *path,const WCHAR *sddl) {
    PSECURITY_DESCRIPTOR sd=NULL;BOOL ok;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&sd,NULL))return FALSE;
    ok=SetFileSecurityW(path,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,sd);LocalFree(sd);return ok;
}
static void privilege(const WCHAR *name) {
    HANDLE token;TOKEN_PRIVILEGES tp;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return;
    tp.PrivilegeCount=1;tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    if(LookupPrivilegeValueW(NULL,name,&tp.Privileges[0].Luid))AdjustTokenPrivileges(token,FALSE,&tp,0,NULL,NULL);
    CloseHandle(token);
}
static HANDLE user_token(void) {
    typedef BOOL (WINAPI *Query)(ULONG,PHANDLE);
    HMODULE lib=LoadLibraryW(L"wtsapi32.dll");Query query;HANDLE token=NULL;
    DWORD session=WTSGetActiveConsoleSessionId(),needed;TOKEN_LINKED_TOKEN linked;
    if(!lib)return NULL;
    query=(Query)(void*)GetProcAddress(lib,"WTSQueryUserToken");
    if(session==0xffffffff||!query||!query(session,&token)){DWORD e=GetLastError();FreeLibrary(lib);SetLastError(e?e:ERROR_NO_SUCH_LOGON_SESSION);return NULL;}
    FreeLibrary(lib);
    if(GetTokenInformation(token,TokenLinkedToken,&linked,sizeof(linked),&needed)) {
        TOKEN_ELEVATION elevation;
        if(GetTokenInformation(linked.LinkedToken,TokenElevation,&elevation,sizeof(elevation),&needed)&&elevation.TokenIsElevated) {CloseHandle(token);token=linked.LinkedToken;}
        else CloseHandle(linked.LinkedToken);
    }
    return token;
}
static void start_job(const char *id,const BYTE *data,DWORD len,DWORD timeout,BOOL user) {
    WCHAR script[MAX_PATH],output[MAX_PATH],state[MAX_PATH],shell[MAX_PATH],line[1024];
    WCHAR *command;char *oem;int bytes;BOOL used=FALSE,ok=FALSE;DWORD err=0;
    typedef BOOL (WINAPI *MakeEnv)(LPVOID*,HANDLE,BOOL);
    typedef BOOL (WINAPI *FreeEnv)(LPVOID);
    HMODULE envlib=NULL;LPVOID env=NULL;FreeEnv free_env=NULL;
    HANDLE token=NULL,job=NULL;STARTUPINFOW si;PROCESS_INFORMATION pi;JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    if(timeout<1||timeout>3600000||len==0||len>65536){error_reply(id,ERROR_INVALID_PARAMETER,"invalid command or timeout");return;}
    path_for(state,id,L"state");if(GetFileAttributesW(state)!=INVALID_FILE_ATTRIBUTES){job_status(id);return;}
    poll_job();if(active.process){error_reply(id,ERROR_BUSY,"another job is running");return;}
    command=utf8(data,len);if(!command){error_reply(id,GetLastError(),"invalid UTF-8 command");return;}
    bytes=WideCharToMultiByte(CP_OEMCP,WC_NO_BEST_FIT_CHARS,command,-1,NULL,0,NULL,&used);
    oem=bytes?malloc((size_t)bytes+2):NULL;
    if(!oem){free(command);error_reply(id,ERROR_NOT_ENOUGH_MEMORY,"command allocation");return;}
    used=FALSE;WideCharToMultiByte(CP_OEMCP,WC_NO_BEST_FIT_CHARS,command,-1,oem,bytes,NULL,&used);free(command);
    if(used){free(oem);error_reply(id,ERROR_NO_UNICODE_TRANSLATION,"command cannot be represented in guest OEM encoding");return;}
    oem[bytes-1]='\r';oem[bytes]='\n';oem[bytes+1]=0;
    path_for(script,id,L"cmd");path_for(output,id,L"out");
    if(!state_write(id,"RUNNING",0)){err=GetLastError();goto done;}
    if(!putfile(script,oem,(DWORD)bytes+1)||!putfile(output,"",0)){err=GetLastError();goto done;}
    if(user&&!acl(output,L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGW;;;BU)")){err=GetLastError();goto done;}
    GetSystemDirectoryW(shell,MAX_PATH);wcscat(shell,L"\\cmd.exe");
    swprintf(line,1024,L"\"%ls\" /d /s /c \"\"%ls\" > \"%ls\" 2>&1\"",shell,script,output);
    ZeroMemory(&si,sizeof(si));si.cb=sizeof(si);ZeroMemory(&pi,sizeof(pi));
    if(user){
        MakeEnv make_env;
        token=user_token();if(!token){err=GetLastError();goto done;}
        envlib=LoadLibraryW(L"userenv.dll");if(!envlib){err=GetLastError();goto done;}
        make_env=(MakeEnv)(void*)GetProcAddress(envlib,"CreateEnvironmentBlock");
        free_env=(FreeEnv)(void*)GetProcAddress(envlib,"DestroyEnvironmentBlock");
        if(!make_env||!free_env||!make_env(&env,token,FALSE)){err=GetLastError();goto done;}
        si.lpDesktop=L"winsta0\\default";
    }
    job=CreateJobObjectW(NULL,NULL);if(!job){err=GetLastError();goto done;}
    ZeroMemory(&limits,sizeof(limits));limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){err=GetLastError();goto done;}
    ok=user?CreateProcessAsUserW(token,shell,line,NULL,NULL,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,env,L"C:\\",&si,&pi)
           :CreateProcessW(shell,line,NULL,NULL,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,NULL,L"C:\\",&si,&pi);
    if(!ok){err=GetLastError();goto done;}
    if(!AssignProcessToJobObject(job,pi.hProcess)||ResumeThread(pi.hThread)==(DWORD)-1){err=GetLastError();TerminateProcess(pi.hProcess,err);CloseHandle(pi.hProcess);CloseHandle(pi.hThread);ok=FALSE;goto done;}
    CloseHandle(pi.hThread);strcpy(active.id,id);active.process=pi.hProcess;active.job=job;job=NULL;active.start=GetTickCount();active.timeout=timeout;
done:
    free(oem);if(env&&free_env)free_env(env);if(envlib)FreeLibrary(envlib);
    if(token)CloseHandle(token);
    if(job)CloseHandle(job);
    if(ok)reply(id,"RUNNING",0,NULL,0);
    else {if(!err)err=ERROR_GEN_FAILURE;state_write(id,"ERROR",err);error_reply(id,err,"cannot launch job");}
}
static void handle_request(const char *id,const char *op,DWORD arg,BYTE *data,DWORD len) {
    WCHAR *path;HANDLE f;DWORD got=0;BYTE *buffer;BYTE *zero;
    if(!strcmp(op,"PING")){reply(id,"OK",0,"TritonVistaControl v1.1 LocalSystem COM2",(DWORD)strlen("TritonVistaControl v1.1 LocalSystem COM2"));return;}
    if(!strcmp(op,"RUN")||!strcmp(op,"USER")){start_job(id,data,len,arg,!strcmp(op,"USER"));return;}
    if(!strcmp(op,"STATUS")){job_status(id);return;}
    if(!strcmp(op,"READ")) {
        path=utf8(data,len);if(!absolute_path(path)){free(path);error_reply(id,ERROR_INVALID_NAME,"local absolute path required");return;}
        f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);free(path);
        if(f==INVALID_HANDLE_VALUE){error_reply(id,GetLastError(),"open file");return;}
        {LARGE_INTEGER pos;pos.QuadPart=arg;if(!SetFilePointerEx(f,pos,NULL,FILE_BEGIN)){DWORD e=GetLastError();CloseHandle(f);error_reply(id,e,"seek file");return;}}
        buffer=malloc(256*1024);if(!buffer){CloseHandle(f);error_reply(id,ERROR_NOT_ENOUGH_MEMORY,"read buffer");return;}
        if(ReadFile(f,buffer,256*1024,&got,NULL))reply(id,"OK",0,buffer,got);else error_reply(id,GetLastError(),"read file");
        free(buffer);CloseHandle(f);return;
    }
    if(!strcmp(op,"WRITE")) {
        zero=memchr(data,0,len);if(!zero||arg){error_reply(id,ERROR_INVALID_PARAMETER,"invalid write payload");return;}
        path=utf8(data,(DWORD)(zero-data));if(!absolute_path(path)){free(path);error_reply(id,ERROR_INVALID_NAME,"local absolute path required");return;}
        if(putfile(path,zero+1,len-(DWORD)(zero-data)-1))reply(id,"OK",0,NULL,0);else error_reply(id,GetLastError(),"write file");free(path);return;
    }
    error_reply(id,ERROR_INVALID_FUNCTION,"unknown operation");
}
static BOOL read_bytes(BYTE *data,DWORD size) {
    DWORD got=0,n,start=GetTickCount();
    while(got<size&&WaitForSingleObject(stop_event,0)!=WAIT_OBJECT_0) {
        poll_job();if(!ReadFile(port,data+got,size-got,&n,NULL))return FALSE;
        got+=n;if((DWORD)(GetTickCount()-start)>180000)return FALSE;
    }
    return got==size;
}
static void serve(void) {
    char header[256],id[33],op[16],argstr[16],lenstr[16],digest[65],extra;DWORD arg,len,n;size_t count=0;BYTE *body;DCB dcb;COMMTIMEOUTS tm;
    while(WaitForSingleObject(stop_event,0)!=WAIT_OBJECT_0) {
        poll_job();
        if(port==INVALID_HANDLE_VALUE) {
            port=CreateFileW(L"\\\\.\\COM2",GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
            if(port==INVALID_HANDLE_VALUE){WaitForSingleObject(stop_event,1000);continue;}
            ZeroMemory(&dcb,sizeof(dcb));dcb.DCBlength=sizeof(dcb);GetCommState(port,&dcb);
            dcb.BaudRate=CBR_115200;dcb.ByteSize=8;dcb.Parity=NOPARITY;dcb.StopBits=ONESTOPBIT;dcb.fBinary=TRUE;dcb.fParity=FALSE;dcb.fOutxCtsFlow=FALSE;dcb.fOutxDsrFlow=FALSE;dcb.fOutX=FALSE;dcb.fInX=FALSE;dcb.fDtrControl=DTR_CONTROL_ENABLE;dcb.fRtsControl=RTS_CONTROL_ENABLE;dcb.fDsrSensitivity=FALSE;dcb.fAbortOnError=FALSE;
            ZeroMemory(&tm,sizeof(tm));tm.ReadIntervalTimeout=MAXDWORD;tm.ReadTotalTimeoutConstant=100;tm.WriteTotalTimeoutConstant=5000;
            if(!SetCommState(port,&dcb)||!SetCommTimeouts(port,&tm)){CloseHandle(port);port=INVALID_HANDLE_VALUE;continue;}
            SetupComm(port,65536,65536);PurgeComm(port,PURGE_RXCLEAR|PURGE_TXCLEAR);count=0;
        }
        if(!ReadFile(port,header+count,1,&n,NULL)){CloseHandle(port);port=INVALID_HANDLE_VALUE;continue;}
        if(!n)continue;
        if(header[count]=='\n') {
            header[count]=0;count=0;
            if(sscanf(header,"TC1 %32s %15s %15s %15s %64s %c",id,op,argstr,lenstr,digest,&extra)!=5||!valid_id(id)||!number(argstr,&arg)||!number(lenstr,&len)||len>LIMIT)continue;
            body=malloc((size_t)len+1);if(!body){error_reply(id,ERROR_NOT_ENOUGH_MEMORY,"payload allocation");continue;}
            if(read_bytes(body,len)){body[len]=0;if(payload_valid(body,len,digest))handle_request(id,op,arg,body,len);else error_reply(id,ERROR_CRC,"payload checksum mismatch");}free(body);
        } else if(++count>=sizeof(header)-1)count=0;
    }
    if(active.process){TerminateJobObject(active.job,ERROR_PROCESS_ABORTED);WaitForSingleObject(active.process,5000);finish_job(ERROR_PROCESS_ABORTED);}
    if(port!=INVALID_HANDLE_VALUE){CloseHandle(port);port=INVALID_HANDLE_VALUE;}
}
static void report(DWORD state) {
    ZeroMemory(&service_status,sizeof(service_status));service_status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;service_status.dwCurrentState=state;
    service_status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    if(state==SERVICE_START_PENDING||state==SERVICE_STOP_PENDING){service_status.dwWaitHint=10000;service_status.dwCheckPoint=1;}
    SetServiceStatus(status_handle,&service_status);
}
static void WINAPI control(DWORD code) {
    if(code==SERVICE_CONTROL_STOP||code==SERVICE_CONTROL_SHUTDOWN){report(SERVICE_STOP_PENDING);SetEvent(stop_event);}
    else if(code==SERVICE_CONTROL_INTERROGATE)SetServiceStatus(status_handle,&service_status);
}
static void WINAPI service_main(DWORD argc,WCHAR **argv) {
    (void)argc;(void)argv;status_handle=RegisterServiceCtrlHandlerW(NAME,control);if(!status_handle)return;
    report(SERVICE_START_PENDING);stop_event=CreateEventW(NULL,TRUE,FALSE,NULL);if(!stop_event){report(SERVICE_STOPPED);return;}
    privilege(SE_TCB_NAME);privilege(SE_ASSIGNPRIMARYTOKEN_NAME);privilege(SE_INCREASE_QUOTA_NAME);privilege(SE_SHUTDOWN_NAME);
    report(SERVICE_RUNNING);serve();CloseHandle(stop_event);report(SERVICE_STOPPED);
}
static LONG safe_key(const WCHAR *mode,const WCHAR *service,const WCHAR *value,BOOL remove) {
    WCHAR path[256];HKEY key;DWORD disposition;LONG e;
    swprintf(path,256,L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\%ls\\%ls",mode,service);
    if(remove){e=RegDeleteKeyW(HKEY_LOCAL_MACHINE,path);return e==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:e;}
    e=RegCreateKeyExW(HKEY_LOCAL_MACHINE,path,0,NULL,0,KEY_SET_VALUE,NULL,&key,&disposition);
    if(e==ERROR_SUCCESS){if(disposition==REG_CREATED_NEW_KEY||!wcscmp(service,NAME))e=RegSetValueExW(key,NULL,0,REG_SZ,(BYTE*)value,(DWORD)(wcslen(value)+1)*2);RegCloseKey(key);}return e;
}
static int install(BOOL remove) {
    SC_HANDLE scm=OpenSCManagerW(NULL,NULL,SC_MANAGER_ALL_ACCESS),svc;WCHAR src[MAX_PATH],dst[MAX_PATH],line[MAX_PATH+40];DWORD err=0;SERVICE_STATUS ss;
    if(!scm)return (int)GetLastError();
    svc=OpenServiceW(scm,NAME,SERVICE_ALL_ACCESS);
    if(remove) {
        if(svc){ControlService(svc,SERVICE_CONTROL_STOP,&ss);if(!DeleteService(svc))err=GetLastError();CloseServiceHandle(svc);}
        safe_key(L"Minimal",NAME,L"Service",TRUE);safe_key(L"Network",NAME,L"Service",TRUE);CloseServiceHandle(scm);return (int)err;
    }
    GetModuleFileNameW(NULL,src,MAX_PATH);GetSystemDirectoryW(dst,MAX_PATH);wcscat(dst,L"\\triton-vista-control.exe");
    if(_wcsicmp(src,dst)&&!CopyFileW(src,dst,FALSE)){err=GetLastError();goto done;}
    if(!SetFileAttributesW(dst,FILE_ATTRIBUTE_NORMAL)){err=GetLastError();goto done;}
    CreateDirectoryW(L"C:\\ProgramData\\TritonControl",NULL);CreateDirectoryW(ROOT,NULL);
    if(!acl(L"C:\\ProgramData\\TritonControl",L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)")||!acl(ROOT,L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)")){err=GetLastError();goto done;}
    swprintf(line,MAX_PATH+40,L"\"%ls\" --service",dst);
    if(!svc)svc=CreateServiceW(scm,NAME,L"Triton Vista host control",SERVICE_ALL_ACCESS,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,line,NULL,NULL,NULL,L"LocalSystem",NULL);
    else if(!ChangeServiceConfigW(svc,SERVICE_NO_CHANGE,SERVICE_AUTO_START,SERVICE_NO_CHANGE,line,NULL,NULL,NULL,L"LocalSystem",NULL,NULL)){err=GetLastError();goto done;}
    if(!svc){err=GetLastError();goto done;}
    {SC_ACTION action={SC_ACTION_RESTART,2000};SERVICE_FAILURE_ACTIONSW fail={86400,NULL,NULL,1,&action};ChangeServiceConfig2W(svc,SERVICE_CONFIG_FAILURE_ACTIONS,&fail);}
    err=(DWORD)safe_key(L"Minimal",NAME,L"Service",FALSE);if(err)goto done;
    err=(DWORD)safe_key(L"Network",NAME,L"Service",FALSE);if(err)goto done;
    err=(DWORD)safe_key(L"Minimal",L"Serial",L"Driver",FALSE);if(err)goto done;
    err=(DWORD)safe_key(L"Network",L"Serial",L"Driver",FALSE);if(err)goto done;
    {
        const WCHAR *modes[]={L"Minimal",L"Network"};
        const WCHAR *drivers[]={L"serial.sys",L"Serenum",L"serenum.sys",L"{4d36e978-e325-11ce-bfc1-08002be10318}"};
        unsigned m,d;
        for(m=0;m<2;m++)for(d=0;d<4;d++) {
            err=(DWORD)safe_key(modes[m],drivers[d],d==3?L"Ports":L"Driver",FALSE);
            if(err)goto done;
        }
    }
    if(!StartServiceW(svc,0,NULL)&&GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING)err=GetLastError();
done:
    if(svc)CloseServiceHandle(svc);
    CloseServiceHandle(scm);return (int)err;
}
int wmain(int argc,WCHAR **argv) {
    SERVICE_TABLE_ENTRYW table[]={{NAME,service_main},{NULL,NULL}};
    if(argc==2&&!wcscmp(argv[1],L"--stage-update")) {
        WCHAR src[MAX_PATH],dst[MAX_PATH];
        GetModuleFileNameW(NULL,src,MAX_PATH);GetSystemDirectoryW(dst,MAX_PATH);
        wcscat(dst,L"\\triton-vista-control.exe");
        if(!_wcsicmp(src,dst))return ERROR_INVALID_PARAMETER;
        if(!SetFileAttributesW(dst,FILE_ATTRIBUTE_NORMAL))return (int)GetLastError();
        return MoveFileExW(src,dst,MOVEFILE_DELAY_UNTIL_REBOOT|MOVEFILE_REPLACE_EXISTING)?0:(int)GetLastError();
    }
    if(argc==2&&!wcscmp(argv[1],L"--install")){int e=install(FALSE);wprintf(L"TritonVistaControl install: %d\n",e);return e;}
    if(argc==2&&!wcscmp(argv[1],L"--uninstall"))return install(TRUE);
    if(argc==2&&!wcscmp(argv[1],L"--service"))return StartServiceCtrlDispatcherW(table)?0:(int)GetLastError();
    fwprintf(stderr,L"usage: triton-vista-control.exe --install|--uninstall|--service\n");return 2;
}
