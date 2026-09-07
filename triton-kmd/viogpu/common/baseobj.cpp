#include "baseobj.h"
#include "viogpu.h"

// clang-format off
_When_((PoolType & NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
        "Allocation failures cause a system crash"))
    void* __cdecl operator new(size_t Size, POOL_TYPE PoolType)
// clang-format on
{
    Size = (Size != 0) ? Size : 1;

    void *pObject = VioGpuAllocatePool(PoolType, Size, VIOGPUTAG);

    if (pObject != NULL)
    {
#if DBG
        RtlFillMemory(pObject, Size, 0xCD);
#else
        RtlZeroMemory(pObject, Size);
#endif // DBG
    }
    return pObject;
}

_When_((PoolType & NonPagedPoolMustSucceed) != 0,
       __drv_reportError("Must succeed pool allocations are forbidden. "
                         "Allocation failures cause a system crash")) void *__cdecl
operator new[](size_t Size, POOL_TYPE PoolType)
{

    Size = (Size != 0) ? Size : 1;

    void *pObject = VioGpuAllocatePool(PoolType, Size, VIOGPUTAG);

    if (pObject != NULL)
    {
#if DBG
        RtlFillMemory(pObject, Size, 0xCD);
#else
        RtlZeroMemory(pObject, Size);
#endif
    }
    return pObject;
}

void __cdecl operator delete(void *pObject)
{

    if (pObject != NULL)
    {
        ExFreePoolWithTag(pObject, VIOGPUTAG);
    }
}

void __cdecl operator delete[](void *pObject)
{

    if (pObject != NULL)
    {
        ExFreePoolWithTag(pObject, VIOGPUTAG);
    }
}

void __cdecl operator delete(void *pObject, size_t Size)
{

    UNREFERENCED_PARAMETER(Size);
    ::operator delete(pObject);
}

#if defined(VIOGPU_TARGET_VISTA) && defined(_M_AMD64)
// The Vista WDK has the kernel headers and import libraries needed by this
// target, but it does not provide the C++ vector iterator objects expected by
// a current MSVC compiler.  Linking the contemporary CRT to obtain them would
// add user-mode API imports to a native KMD.  Driver constructors do not throw,
// so the kernel-safe iterator implementations only need to construct in order
// and destroy in reverse order.
typedef void(__cdecl *VIOGPU_VECTOR_CTOR)(void *);
typedef void(__cdecl *VIOGPU_VECTOR_DTOR)(void *);

extern "C" void __cdecl VioGpuVistaVectorCtor(void *storage,
                                                SIZE_T elementSize,
                                                SIZE_T count,
                                                VIOGPU_VECTOR_CTOR ctor,
                                                VIOGPU_VECTOR_DTOR /* dtor */)
{
    PUCHAR element = static_cast<PUCHAR>(storage);
    for (SIZE_T index = 0; index < count; ++index, element += elementSize)
    {
        ctor(element);
    }
}

extern "C" void __cdecl VioGpuVistaVectorDtor(void *storage,
                                                SIZE_T elementSize,
                                                SIZE_T count,
                                                VIOGPU_VECTOR_DTOR dtor)
{
    PUCHAR element = static_cast<PUCHAR>(storage) + elementSize * count;
    while (count-- != 0)
    {
        element -= elementSize;
        dtor(element);
    }
}

// MSVC emits these two ABI helper names for array members with non-trivial
// constructors/destructors.  Alternate them to the two small implementations
// above instead of accepting a modern C++ runtime dependency.
#pragma comment(linker, "/alternatename:??_L@YAXPEAX_K1P6AX0@Z2@Z=VioGpuVistaVectorCtor")
#pragma comment(linker, "/alternatename:??_M@YAXPEAX_K1P6AX0@Z@Z=VioGpuVistaVectorDtor")
#endif

// Placement-delete forwarders that match the placement operator new
// overloads above. The compiler invokes these when a constructor throws
// inside a placement-new expression; without them the linker would
// emit "unresolved external" the moment any constructor learns to
// throw. None of the driver's constructors currently throw, but the
// matching pair must exist for the language to be well-formed.
void __cdecl operator delete(void *pObject, POOL_TYPE PoolType)
{
    UNREFERENCED_PARAMETER(PoolType);
    ::operator delete(pObject);
}

void __cdecl operator delete[](void *pObject, POOL_TYPE PoolType)
{
    UNREFERENCED_PARAMETER(PoolType);
    ::operator delete[](pObject);
}
