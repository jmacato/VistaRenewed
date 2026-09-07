#ifndef VULKAN_D3DDDI_H_
#define VULKAN_D3DDDI_H_ 1

#ifdef __cplusplus
extern "C" {
#endif

#define VK_STRUCTURE_TYPE_D3DDDI_CALLBACKS       ((VkStructureType)4281808695u)
#define VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE ((VkStructureType)4281808696u)
#define VK_STRUCTURE_TYPE_D3DDDI_OPEN_RESOURCE   ((VkStructureType)4281808697u)

typedef struct {
    VkStructureType sType;
    void *pNext;

    LUID AdapterLuid;

    HANDLE                          hRTAdapter;         // in: Runtime handle
    HANDLE                          hRTDevice;          // in: Runtime handle
    const D3DDDI_ADAPTERCALLBACKS  *pAdapterCallbacks;  // in: Pointer to runtime callbacks that invoke kernel
    const D3DDDI_DEVICECALLBACKS   *pKTCallbacks;       // in: Pointer to runtime callbacks that invoke kernel
    const DXGI_DDI_BASE_CALLBACKS  *pDXGIBaseCallbacks; // in: The driver should record this pointer for later use

    D3D10DDI_HRTCORELAYER                     hRTCoreLayer;   // in:  CoreLayer handle
    const D3D11DDI_CORELAYER_DEVICECALLBACKS* p11UMCallbacks; // in:  callbacks that stay in usermode

    HANDLE hContext; // out: Context handle
} VkD3DDDICallbacks;

typedef struct {
    VkStructureType sType;
    void *pNext;
    HANDLE hRTResource;
    const D3D10DDIARG_CREATERESOURCE *pCreateResource;
} VkD3DDDICreateResource;

typedef struct {
    VkStructureType sType;
    void *pNext;
    HANDLE hRTResource;
    const D3D10DDIARG_OPENRESOURCE *pOpenResource;
    const void *pResourceInfo; /* VIOGPU_RES_INFO_REQ */
} VkD3DDDIOpenResource;

#define VK_STRUCTURE_TYPE_D3DDDI_CALLBACKS_cast       VkD3DDDICallbacks
#define VK_STRUCTURE_TYPE_D3DDDI_CREATE_RESOURCE_cast VkD3DDDICreateResource
#define VK_STRUCTURE_TYPE_D3DDDI_OPEN_RESOURCE_cast   VkD3DDDIOpenResource

#ifdef __cplusplus
}
#endif

#endif
