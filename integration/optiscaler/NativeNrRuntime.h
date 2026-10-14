#pragma once
#include <windows.h>
#include <d3d12.h>
// Private factory setup; never changes the global SDK or existing devices.
HRESULT NativeNrInitializeRuntime();
bool NativeNrCreateDevice(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** device,HRESULT* result);
