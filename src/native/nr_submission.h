#pragma once
#include "dml_common.h"
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <algorithm>

// The caller serializes track/execute/discard/collect, and excludes concurrent
// submission on the same queue until execute returns. original must call the
// real ExecuteCommandLists exactly once, without recursing through this wrapper.
class NativeNrSubmissions {
    struct Entry {
        ComPtr<IUnknown> listIdentity;
        std::shared_ptr<void> owner;
        std::function<void(ID3D12Fence*,UINT64)> submitted;
        std::function<void()> cancelled;
        ComPtr<ID3D12Fence> fence;
    };
    ComPtr<ID3D12Device> device;
    std::map<IUnknown*,std::vector<std::shared_ptr<Entry>>> recorded;
    std::vector<std::shared_ptr<Entry>> inFlight;
    ComPtr<IUnknown> identity(IUnknown* object) {
        if(!object)throw std::runtime_error("Submission object missing");
        return nativeObjectIdentity(object);
    }
    void sameDevice(ID3D12DeviceChild* child) {
        if(!child)throw std::runtime_error("Submission device child missing");
        ComPtr<ID3D12Device> other;check(child->GetDevice(IID_PPV_ARGS(&other)),"Submission device");
        if(identity(other.Get()).Get()!=identity(device.Get()).Get())throw std::runtime_error("Submission device mismatch");
    }
public:
    explicit NativeNrSubmissions(ID3D12Device* borrowed):device(borrowed){if(!borrowed)throw std::runtime_error("Submission device missing");}
    NativeNrSubmissions(const NativeNrSubmissions&)=delete;
    NativeNrSubmissions& operator=(const NativeNrSubmissions&)=delete;
    bool contains(ID3D12CommandList* list,const void* owner){auto key=identity(list);auto found=recorded.find(key.Get());
        if(found==recorded.end())return false;for(auto& entry:found->second)if(entry->owner.get()==owner)return true;return false;}
    bool hasRecorded(UINT count,ID3D12CommandList* const* lists){if(count&&!lists)return false;
        for(UINT i=0;i<count;++i){auto key=identity(lists[i]);if(recorded.count(key.Get()))return true;}return false;}
    template<class Session> void track(ID3D12CommandList* list,std::shared_ptr<Session> session) {
        sameDevice(list);if(!session)throw std::runtime_error("Submission session missing");
        if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE)throw std::runtime_error("Unsupported NR submission list");
        // A ready session may have a completed marker still retained in this
        // registry. Capture happens before Present's collect() in a real game.
        // Remove only fence-completed entries before rejecting concurrent use.
        collect();
        auto key=identity(list);
        for(auto& pair:recorded)for(auto& entry:pair.second)if(entry->owner.get()==session.get())throw std::runtime_error("Session tracked twice");
        for(auto& entry:inFlight)if(entry->owner.get()==session.get())throw std::runtime_error("Session still in flight");
        auto entry=std::make_shared<Entry>();entry->listIdentity=key;entry->owner=session;
        entry->submitted=[session](ID3D12Fence* fence,UINT64 value){session->notifySubmitted(fence,value);};
        entry->cancelled=[session](){session->cancelUnsubmitted();};
        recorded[key.Get()].push_back(entry);
    }
    // This owns only submission bookkeeping; it does not close/reset lists,
    // wait on the CPU, read textures, or touch unrelated game commands.
    size_t execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,
                   const std::function<void()>& original) {
        sameDevice(queue);if(!original||(count&&!lists))throw std::runtime_error("Invalid submission call");
        std::vector<IUnknown*> keys;std::vector<std::shared_ptr<Entry>> batch;
        for(UINT i=0;i<count;++i){sameDevice(lists[i]);auto key=identity(lists[i]);
            if(queue->GetDesc().Type!=lists[i]->GetType())throw std::runtime_error("Submission queue/list type mismatch");
            if(std::find(keys.begin(),keys.end(),key.Get())!=keys.end())throw std::runtime_error("Duplicate submitted list");
            keys.push_back(key.Get());auto found=recorded.find(key.Get());
            if(found!=recorded.end())batch.insert(batch.end(),found->second.begin(),found->second.end());
        }
        // Reserve/create before execution: allocation failure leaves recorded
        // ownership intact. One private fence per batch prevents value collisions
        // and accidental completion by an unrelated queue.
        ComPtr<ID3D12Fence> marker;
        if(!batch.empty()){
            check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&marker)),"Submission marker");
            inFlight.reserve(inFlight.size()+batch.size());
        }
        original();
        if(batch.empty())return 0;
        for(auto& entry:batch){entry->fence=marker;inFlight.push_back(entry);}
        for(auto key:keys)recorded.erase(key);
        // Preserve owners even if notification or Signal fails after Execute.
        // An unsignalled marker is never collected or interpreted as cancelled.
        for(auto& entry:batch)entry->submitted(marker.Get(),1);
        check(queue->Signal(marker.Get(),1),"Submission completion signal");return batch.size();
    }
    // Only after a successful Reset/discard, with a guarantee the old recording
    // will never execute. ReleaseFeature alone is NOT evidence of discard.
    size_t discard(ID3D12CommandList* list){sameDevice(list);auto key=identity(list);auto found=recorded.find(key.Get());
        if(found==recorded.end())return 0;size_t count=found->second.size();
        for(auto& entry:found->second)entry->cancelled();recorded.erase(found);return count;
    }
    size_t collect(){size_t before=inFlight.size();
        inFlight.erase(std::remove_if(inFlight.begin(),inFlight.end(),[](auto& entry){auto done=entry->fence->GetCompletedValue();return done!=~UINT64(0)&&done>=1;}),inFlight.end());return before-inFlight.size();
    }
    size_t recordedCount()const{size_t count=0;for(auto& pair:recorded)count+=pair.second.size();return count;}
    size_t inFlightCount()const{return inFlight.size();}
    ~NativeNrSubmissions(){collect();if(!recorded.empty()||!inFlight.empty())std::terminate();}
};
