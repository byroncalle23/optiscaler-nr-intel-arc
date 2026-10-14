#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx_params.h>
#if defined(NATIVE_NR_API_IMPLEMENTATION)
#define NATIVE_NR_API extern "C" __declspec(dllexport)
#else
#define NATIVE_NR_API extern "C"
#endif
// Versioned test/initialization boundary. No STL objects cross the DLL ABI.
// Initialize/Stop require quiescent caller threads; keep the caller's module
// reference until Stop succeeds. Never unload while Stop reports pending work.
struct NativeNrApiStats {
    UINT size,version,active,pending,recorded,inFlight,frames,faulted;
    UINT64 selectedSessionBytes;
};
struct NativeNrPreparationStats {UINT size,version,queued,preparing,attempts,prepared,rejected;HRESULT lastResult;};
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Initialize(ID3D12CommandQueue* queue);
// Explicit Prepare blocks. Automatic discovery uses an owned preparation worker.
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Prepare(UINT handle,ID3D12Resource* output);
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Evaluate(UINT handle,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* params,BOOL hdr,BOOL knownState,UINT state,UINT* result);
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Reset(ID3D12GraphicsCommandList* list,HRESULT resetResult);
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Retire(UINT handle);
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Collect();
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Stats(UINT handle,NativeNrApiStats* stats);
// Diagnostic readback only after caller completion; never invoke within a frame.
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Capture(UINT handle,const wchar_t* folder);
// Returns DXGI_ERROR_WAS_STILL_DRAWING while GPU work or preparation is retained.
// Retry on a quiescent control thread; successful Stop joins the worker.
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_Stop();
NATIVE_NR_API const char* WINAPI OptiScalerNR_LastError();
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_PreparationStats(NativeNrPreparationStats* stats);
// Called at captured-queue setup, never from Evaluate/DllMain. Opt-in via
// native_nr.ini beside this DLL: [NativeNR] Enabled=1. Menu request overrides
// the startup setting for this process only; other settings are not hot-reloaded.
// S_FALSE: disabled or already initialized on this device; error: keep NR off.
NATIVE_NR_API HRESULT WINAPI OptiScalerNR_ConfigureQueue(ID3D12CommandQueue* queue);
// Internal C++ bridge, not a new exported test ABI. Runs after game tone mapping.
void NativeNrPresentSdr(ID3D12CommandQueue* queue,IDXGISwapChain* swapchain,bool sdrOutput,UINT processingWidth=0,UINT processingHeight=0);
HRESULT NativeNrReleasePresent(IDXGISwapChain* swapchain);
bool NativeNrUsesPresentSdr();
