#pragma once
#include <d3d12.h>
#include <memory>
#include <mutex>
#include "NativeNrFrameInput.h"

// Disabled experimental boundary. Provider owns its controller and serializes
// record/submit/reset/retire; callbacks must retain GPU objects on any failure.
namespace NativeNrLifecycle {
using Forward = void (*)(void*);
struct Provider {
    std::shared_ptr<void> owner;
    void* user=nullptr;
    bool (*execute)(void*,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,Forward,void*)=nullptr;
    bool (*recorded)(void*,unsigned,ID3D12GraphicsCommandList*)=nullptr;
    void (*reset)(void*,ID3D12GraphicsCommandList*)=nullptr;
    void (*retire)(void*,unsigned)=nullptr;
    void (*shutdown)(void*)=nullptr; // Request retirement; not proof of GPU completion.
    void (*observe)(void*,unsigned,ID3D12Resource*)=nullptr; // Metadata only; no preparation here.
    // Optional temporal capture before the upscaler consumes/reuses engine inputs.
    // An absent/failed temporal observer must never fail the game's upscaler.
    bool (*frameInputs)(void*,ID3D12GraphicsCommandList*,const NativeNrTemporal::FrameInput&)=nullptr;
};
inline std::mutex mutex;
inline std::shared_ptr<const Provider> provider;
inline void Register(std::shared_ptr<const Provider> value){std::lock_guard<std::mutex> lock(mutex);provider=std::move(value);}
inline std::shared_ptr<const Provider> Snapshot(){std::lock_guard<std::mutex> lock(mutex);return provider;}
// Forward the game's call exactly once even when the provider fails before or
// after Execute. Never replay the list to recover from a missing NR marker.
inline bool Execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,Forward original,void* user) noexcept {
    if(!original)return false;
    struct Call {Forward original;void* user;bool called=false,duplicate=false;} call{original,user};
    auto once=[](void* pointer){auto& value=*static_cast<Call*>(pointer);
        if(value.called){value.duplicate=true;return;}value.called=true;value.original(value.user);};
    bool success=true;
    try{auto value=Snapshot();if(value&&value->execute)success=value->execute(value->user,queue,count,lists,once,&call);}
    catch(...){success=false;}
    if(!call.called){try{once(&call);}catch(...){success=false;}}
    return success&&!call.duplicate;
}
inline bool Reset(ID3D12GraphicsCommandList* list,HRESULT result) noexcept {
    if(FAILED(result))return true;
    try{auto value=Snapshot();if(value&&value->reset)value->reset(value->user,list);return true;}catch(...){return false;}
}
inline bool Recorded(unsigned handle,ID3D12GraphicsCommandList* list) noexcept {
    try{auto value=Snapshot();return !value||!value->recorded||value->recorded(value->user,handle,list);}catch(...){return false;}
}
inline bool Retire(unsigned handle) noexcept {
    try{auto value=Snapshot();if(value&&value->retire)value->retire(value->user,handle);return true;}catch(...){return false;}
}
inline bool Shutdown() noexcept {
    try{auto value=Snapshot();if(value&&value->shutdown)value->shutdown(value->user);return true;}catch(...){return false;}
}
inline bool Observe(unsigned handle,ID3D12Resource* output) noexcept {
    try{auto value=Snapshot();if(value&&value->observe)value->observe(value->user,handle,output);return true;}catch(...){return false;}
}
inline bool FrameInputs(ID3D12GraphicsCommandList* list,const NativeNrTemporal::FrameInput& input) noexcept {
    if(!list||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
    try{auto value=Snapshot();return value&&value->frameInputs&&value->frameInputs(value->user,list,input);}catch(...){return false;}
}
}
