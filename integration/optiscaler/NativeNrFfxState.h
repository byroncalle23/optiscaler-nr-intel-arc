#pragma once
#include <d3d12.h>
// Values are the FfxApiResourceState ABI from the bundled FidelityFX SDK.
// Exact cases match its dx12 backend; unsupported combinations stay unknown.
inline bool NativeNrFfxState(unsigned state,D3D12_RESOURCE_STATES& output) {
    switch(state) {
    case 1:output=D3D12_RESOURCE_STATE_COMMON;return true;
    case 2:output=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;return true;
    case 4:output=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;return true;
    case 8:output=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;return true;
    case 12:output=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;return true;
    case 16:output=D3D12_RESOURCE_STATE_COPY_SOURCE;return true;
    case 32:output=D3D12_RESOURCE_STATE_COPY_DEST;return true;
    case 20:output=D3D12_RESOURCE_STATE_GENERIC_READ;return true;
    case 64:output=D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;return true;
    case 128:output=D3D12_RESOURCE_STATE_PRESENT;return true;
    case 256:output=D3D12_RESOURCE_STATE_RENDER_TARGET;return true;
    case 512:output=D3D12_RESOURCE_STATE_DEPTH_WRITE;return true;
    default:return false;
    }
}
