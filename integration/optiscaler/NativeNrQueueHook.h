#pragma once
#include "NativeNrLifecycle.h"
#include <detours/detours.h>
#include <atomic>

// Installation/removal require quiescent caller threads. This experiment only
// covers the captured DIRECT queue implementation; do not enable NR on another
// implementation or queue without installing and validating its observer.
class NativeNrQueueHook {
    using ExecuteFn=void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    inline static ExecuteFn original=nullptr;
    inline static PVOID target=nullptr;
    inline static std::mutex mutex;
    inline static std::atomic<UINT64> failures{0};
    static void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
        struct Call {ID3D12CommandQueue* queue;UINT count;ID3D12CommandList* const* lists;} call{queue,count,lists};
        if(!NativeNrLifecycle::Execute(queue,count,lists,[](void* user){auto& args=*static_cast<Call*>(user);original(args.queue,args.count,args.lists);},&call))++failures;
    }
public:
    static bool Install(ID3D12CommandQueue* queue){
        if(!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
        std::lock_guard<std::mutex> lock(mutex);auto address=(*(PVOID**)queue)[10];
        if(original)return address==target;
        original=reinterpret_cast<ExecuteFn>(address);
        if(DetourTransactionBegin()!=NO_ERROR){original=nullptr;return false;}
        if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR||DetourAttach(&(PVOID&)original,Execute)!=NO_ERROR){
            DetourTransactionAbort();original=nullptr;return false;}
        if(DetourTransactionCommit()!=NO_ERROR){original=nullptr;return false;}
        target=address;return true;
    }
    static bool Remove(){
        std::lock_guard<std::mutex> lock(mutex);if(!original)return true;
        if(DetourTransactionBegin()!=NO_ERROR)return false;
        if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR||DetourDetach(&(PVOID&)original,Execute)!=NO_ERROR){DetourTransactionAbort();return false;}
        if(DetourTransactionCommit()!=NO_ERROR)return false;
        original=nullptr;target=nullptr;return true;
    }
    static UINT64 FailureCount(){return failures.load();}
};
