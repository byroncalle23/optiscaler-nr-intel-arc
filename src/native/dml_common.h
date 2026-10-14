#pragma once
// Standalone numerical test. Upload/readback belong to this test harness only.
// Production integration will bind resident buffers and record on the game's list.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <directml.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <atomic>

using Microsoft::WRL::ComPtr;
// Optional integration adapter. Success returns an owned COM reference, never
// an adapter-LUID equivalence. Standalone/native applications leave it unset.
using NativeInterfaceResolver=HRESULT (*)(IUnknown*,IUnknown**);
inline std::atomic<NativeInterfaceResolver> nrNativeInterfaceResolver{nullptr};
inline ComPtr<IUnknown> nativeObjectIdentity(IUnknown* object){
    if(!object)throw std::runtime_error("Native identity object missing");
    ComPtr<IUnknown> native;
    if(auto resolve=nrNativeInterfaceResolver.load()){
        IUnknown* raw=nullptr;HRESULT hr=resolve(object,&raw);
        if(SUCCEEDED(hr)&&raw)native.Attach(raw);
        else if(raw)raw->Release();
    }
    if(!native)native=object;
    ComPtr<IUnknown> identity;
    HRESULT hr=native->QueryInterface(IID_PPV_ARGS(&identity));
    if(FAILED(hr))throw std::runtime_error("Native COM identity unavailable");
    return identity;
}
inline bool sameNativeDevice(ID3D12Device* a,ID3D12Device* b){return nativeObjectIdentity(a).Get()==nativeObjectIdentity(b).Get();}
inline std::filesystem::path nativePath(const std::string& utf8){
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(),utf8.end()));
#else
    return std::filesystem::u8path(utf8);
#endif
}
static void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char message[200];
        snprintf(message, sizeof(message), "%s failed: 0x%08lx", operation, (unsigned long)hr);
        throw std::runtime_error(message);
    }
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes,
    D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap; props.CreationNodeMask = props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (heap == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state,
        nullptr, IID_PPV_ARGS(&result)), "Create buffer");
    return result;
}
static void uavBarrier(ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &b);
}
static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}

