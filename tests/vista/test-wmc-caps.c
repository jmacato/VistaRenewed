#include <windows.h>
#include <stdio.h>
#include <stddef.h>
typedef struct {int w,h,bpp,minw,minh,maxw,maxh;LONGLONG total,dedicated;UINT caps;} Info;
int main(void){CoInitializeEx(NULL,COINIT_MULTITHREADED);SetDllDirectoryA("C:\\Windows\\ehome");HMODULE m=LoadLibraryA("C:\\Windows\\ehome\\EhUI.dll");if(!m){printf("LOAD %lu\n",GetLastError());return 1;}HRESULT(WINAPI *check)(Info*)=(void*)GetProcAddress(m,"SpDx9DeviceCheckCaps");if(!check)return 2;Info i={0};HRESULT h=check(&i);printf("WMC-CAPS hr=%08lx required=%u screen=%dx%d bpp=%d texture=%dx%d..%dx%d total=%lld dedicated=%lld size=%u\n",(unsigned long)h,i.caps,i.w,i.h,i.bpp,i.minw,i.minh,i.maxw,i.maxh,i.total,i.dedicated,(unsigned)sizeof(i));return FAILED(h)||!(i.caps&1);}
