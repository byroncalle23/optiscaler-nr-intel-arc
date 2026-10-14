#include "dml_runtime.h"
#include "nr_submission.h"
#include "../../integration/optiscaler/NativeNrLifecycle.h"
#include "../../integration/optiscaler/NativeNrQueueHook.h"
// Small real-GPU lifetime test. Session callbacks stand in for the full model;
// this test establishes submission ordering, not model correctness/performance.
struct ProbeSession {
    ComPtr<ID3D12Resource> resident;
    ComPtr<ID3D12Fence>* marker;
    UINT* notifications;
    UINT* cancellations;
    void notifySubmitted(ID3D12Fence* fence,UINT64 value){
        if(value!=1)throw std::runtime_error("Unexpected marker value");
        *marker=fence;++*notifications;
    }
    void cancelUnsubmitted(){++*cancellations;}
};
int main(int argc,char** argv){try{
    bool detour=argc==2&&std::string(argv[1])=="--detour";
    if(argc>1&&!detour)throw std::runtime_error("Usage: dml_submission.exe [--detour]");
    NativeRuntime runtime;auto ctx=runtime.sharedContext();NativeNrSubmissions tracker(ctx->device.Get());
    UINT notifications=0,cancellations=0,originalCalls=0,rejections=0;ComPtr<ID3D12Fence> marker;
    auto provider=std::make_shared<NativeNrLifecycle::Provider>();provider->user=&tracker;
    provider->execute=[](void* user,ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,NativeNrLifecycle::Forward forward,void* args){
        static_cast<NativeNrSubmissions*>(user)->execute(queue,count,lists,[&](){forward(args);});return true;};
    provider->reset=[](void* user,ID3D12GraphicsCommandList* list){static_cast<NativeNrSubmissions*>(user)->discard(list);};
    NativeNrLifecycle::Register(provider);
    auto reject=[&](auto action){bool caught=false;try{action();}catch(const std::runtime_error&){caught=true;}
        if(!caught)throw std::runtime_error("Submission rejection missing");++rejections;};
    auto create=[&](){auto s=std::make_shared<ProbeSession>();s->marker=&marker;s->notifications=&notifications;s->cancellations=&cancellations;
        s->resident=buffer(ctx->device.Get(),256,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);return s;};
    ComPtr<ID3D12CommandAllocator> secondAllocator;ComPtr<ID3D12GraphicsCommandList> second;
    check(ctx->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&secondAllocator)),"Second allocator");
    check(ctx->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,secondAllocator.Get(),nullptr,IID_PPV_ARGS(&second)),"Second list");
    auto discarded=create();std::weak_ptr<ProbeSession> discardedWeak=discarded;tracker.track(second.Get(),discarded);discarded.reset();
    check(second->Close(),"Discard close");check(secondAllocator->Reset(),"Discard allocator reset");check(second->Reset(secondAllocator.Get(),nullptr),"Discard successful reset");
    if(!NativeNrLifecycle::Reset(second.Get(),E_FAIL)||discardedWeak.expired()||cancellations)throw std::runtime_error("Failed reset discarded owners");
    if(!NativeNrLifecycle::Reset(second.Get(),S_OK)||!discardedWeak.expired()||cancellations!=1)throw std::runtime_error("Discard ownership contract failed");
    auto first=create(),other=create();std::weak_ptr<ProbeSession> firstWeak=first,otherWeak=other;
    auto upload=buffer(ctx->device.Get(),256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback=buffer(ctx->device.Get(),256,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped=nullptr;D3D12_RANGE empty{0,0};check(upload->Map(0,&empty,&mapped),"Probe upload");
    for(UINT i=0;i<64;++i)static_cast<UINT*>(mapped)[i]=0x12340000u+i;upload->Unmap(0,nullptr);
    ctx->cmd->CopyBufferRegion(first->resident.Get(),0,upload.Get(),0,256);
    transition(ctx->cmd.Get(),first->resident.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx->cmd->CopyBufferRegion(readback.Get(),0,first->resident.Get(),0,256);
    tracker.track(ctx->cmd.Get(),first);tracker.track(second.Get(),other);
    reject([&](){tracker.track(second.Get(),first);});
    reject([&](){tracker.track(nullptr,first);});
    check(ctx->cmd->Close(),"First close");check(second->Close(),"Second close");first.reset();other.reset();
    ID3D12CommandList* duplicate[]={ctx->cmd.Get(),ctx->cmd.Get()};
    reject([&](){tracker.execute(ctx->queue.Get(),2,duplicate,[&](){++originalCalls;});});
    reject([&](){tracker.execute(ctx->queue.Get(),1,nullptr,[&](){++originalCalls;});});
    if(originalCalls||tracker.recordedCount()!=2)throw std::runtime_error("Rejected submission changed ownership");
    // Unrelated submission forwards once and never attaches an NR marker.
    if(tracker.execute(ctx->queue.Get(),0,nullptr,[&](){++originalCalls;ctx->queue->ExecuteCommandLists(0,nullptr);})!=0||notifications)throw std::runtime_error("Unrelated submission marked");
    ID3D12CommandList* lists[]={ctx->cmd.Get(),second.Get()};
    struct Batch {decltype(ctx) context;ID3D12CommandList** lists;UINT* calls;UINT* notifications;std::weak_ptr<ProbeSession> first,other;} batch{ctx,lists,&originalCalls,&notifications,firstWeak,otherWeak};
    bool executed=true;
    if(detour){
        if(!NativeNrQueueHook::Install(ctx->queue.Get())||!NativeNrQueueHook::Install(ctx->queue.Get()))throw std::runtime_error("Real queue detour install failed");
        ++originalCalls;ctx->queue->ExecuteCommandLists(2,lists);
        executed=NativeNrQueueHook::FailureCount()==0;
    }else executed=NativeNrLifecycle::Execute(ctx->queue.Get(),2,lists,[](void* user){
        auto& args=*static_cast<Batch*>(user);
        if(args.first.expired()||args.other.expired()||*args.notifications)throw std::runtime_error("Marker precedes Execute");
        ++*args.calls;args.context->queue->ExecuteCommandLists(2,args.lists);
    },&batch);
    if(!executed||notifications!=2||originalCalls!=2||tracker.recordedCount()||tracker.inFlightCount()!=2)throw std::runtime_error("Batch association failed");
    if(firstWeak.expired()||otherWeak.expired()||tracker.discard(ctx->cmd.Get()))throw std::runtime_error("Submitted owner lost or cancelled");
    check(marker->SetEventOnCompletion(1,ctx->event),"Probe completion event");
    if(WaitForSingleObject(ctx->event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("Probe fence timeout");
    if(tracker.collect()!=2||!firstWeak.expired()||!otherWeak.expired())throw std::runtime_error("Completed ownership not released");
    if(detour&&!NativeNrQueueHook::Remove())throw std::runtime_error("Real queue detour removal failed");
    D3D12_RANGE range{0,256};check(readback->Map(0,&range,&mapped),"Probe readback");
    for(UINT i=0;i<64;++i)if(static_cast<UINT*>(mapped)[i]!=0x12340000u+i)throw std::runtime_error("GPU submission copy mismatch");readback->Unmap(0,&empty);
    check(ctx->allocator->Reset(),"Probe allocator reset");check(ctx->cmd->Reset(ctx->allocator.Get(),nullptr),"Probe list reset");runtime.submit();
    NativeNrLifecycle::Register(nullptr);provider.reset();
    printf("{\"passed\":true,\"batch_sessions\":2,\"gpu_copy_exact\":true,\"submitted_notifications\":%u,\"discard_cancellations\":%u,\"original_calls\":%u,\"rejections_verified\":%u,\"owners_released_after_fence\":true,\"real_queue_detour\":%s}\n",notifications,cancellations,originalCalls,rejections,detour?"true":"false");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
