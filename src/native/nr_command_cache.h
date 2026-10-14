#pragma once
#include "nr_session.h"

// Owns the immutable recording, its buffers and model until caller completion.
// Caller must serialize all executions through the same session fence lifecycle.
class NativeNrCommandCache {
    std::shared_ptr<NativeNrSession> session;
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::vector<NativeNrSession::CacheState> states;
    std::shared_ptr<NativeGpuProfile> profile;
    UINT width,height;
    bool first=true;
public:
    NativeNrCommandCache(std::shared_ptr<NativeNrSession> owner,ID3D12Resource* output,std::shared_ptr<NativeGpuProfile> diagnostic=nullptr):session(std::move(owner)),target(output),profile(std::move(diagnostic)),width(session->processingWidth()),height(session->processingHeight()){
        states=session->cacheState();ComPtr<ID3D12Device> device;check(target->GetDevice(IID_PPV_ARGS(&device)),"Cache target device");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Cache allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Cache command list");
        try{{NativeProfileRange frame(profile.get(),list.Get(),"frame");
            if(!session->record(list.Get(),target.Get(),D3D12_RESOURCE_STATE_PRESENT))throw std::runtime_error("Cache model recording failed");
            session->closeCacheCycle(list.Get(),states);}
        if(profile)profile->resolve(list.Get());check(list->Close(),"Cache list close");
        }catch(...){if(!session->ready())session->cancelUnsubmitted();throw;}
    }
    bool matches(const std::shared_ptr<NativeNrSession>& owner,ID3D12Resource* output)const{return session==owner&&target.Get()==output&&width==session->processingWidth()&&height==session->processingHeight();}
    bool markReadyForExecute(){if(first){first=false;return true;}return session->markCacheReplay(target.Get(),width,height,states);}
    ID3D12GraphicsCommandList* commandList()const{return list.Get();}
    std::shared_ptr<NativeGpuProfile> gpuProfile()const{return profile;}
};
