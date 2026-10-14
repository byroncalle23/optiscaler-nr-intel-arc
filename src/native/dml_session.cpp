#include "nr_session.h"
#include "ngx_test_parameters.h"
#include "nr_submission.h"
#include "../../integration/optiscaler/NativeNrQueueHook.h"
#include "native_assets.h"
static void save(const std::string& file,const std::vector<float>& values){std::ofstream stream(file,std::ios::binary);
    stream.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*4));if(!stream)throw std::runtime_error("Session capture write failed");}
int main(int argc,char** argv){try{
    if(argc<3||argc>5)throw std::runtime_error("Usage: dml_session.exe fixture-folder rgba32f|rgba16f|rgba8 [--multi|--submission|--detour] [--module-assets]");
    std::string folder=argv[1],formatName=argv[2];bool multi=false,detour=false,tracked=false,moduleAssets=false;
    for(int i=3;i<argc;++i){std::string option=argv[i];if(option=="--multi")multi=true;else if(option=="--detour")detour=true;
        else if(option=="--submission")tracked=true;else if(option=="--module-assets")moduleAssets=true;else throw std::runtime_error("Unknown session option");}
    tracked|=detour;if(multi&&tracked)throw std::runtime_error("Multi and submission modes cannot be combined");
    DXGI_FORMAT format=formatName=="rgba32f"?DXGI_FORMAT_R32G32B32A32_FLOAT:formatName=="rgba16f"?DXGI_FORMAT_R16G16B16A16_FLOAT:formatName=="rgba8"?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_UNKNOWN;
    UINT h,w,nh,nw;std::ifstream config(folder+"/frame_input/shape.txt");if(!(config>>h>>w>>nh>>nw))throw std::runtime_error("Session fixture geometry missing");
    std::string modelFolder=folder;
    if(moduleAssets){auto assets=NativeNrAssets::fromModule(nullptr);if(assets.width()!=w||assets.height()!=h)throw std::runtime_error("Package target extent mismatch");modelFolder=assets.folder();}
    TextureHarness caller(w,h,nullptr,format);auto owner=caller.sharedContext();
    NativeNrSessions registry;auto session=registry.prepare(17,caller.inputTexture(),modelFolder);std::weak_ptr<NativeNrSession> weak=session;
    NativeNrSubmissions submissions(owner->device.Get());UINT submissionCollections=0;
    struct Lifecycle {NativeNrSubmissions* submissions;NativeNrSessions* registry;} lifecycle{&submissions,&registry};
    if(detour){auto provider=std::make_shared<NativeNrLifecycle::Provider>();provider->user=&lifecycle;
        provider->execute=[](void* user,ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,NativeNrLifecycle::Forward forward,void* args){
            static_cast<Lifecycle*>(user)->submissions->execute(queue,count,lists,[&](){forward(args);});return true;};
        provider->reset=[](void* user,ID3D12GraphicsCommandList* list){static_cast<Lifecycle*>(user)->submissions->discard(list);};
        provider->recorded=[](void* user,unsigned handle,ID3D12GraphicsCommandList* list){auto& state=*static_cast<Lifecycle*>(user);
            auto session=state.registry->lookup(handle);if(!session)return false;state.submissions->track(list,session);return true;};
        provider->retire=[](void* user,unsigned handle){static_cast<Lifecycle*>(user)->registry->retire(handle);};
        provider->shutdown=[](void* user){static_cast<Lifecycle*>(user)->registry->requestShutdown();};
        NativeNrLifecycle::Register(provider);
        if(!NativeNrQueueHook::Install(owner->queue.Get()))throw std::runtime_error("Session queue detour install failed");}
    UINT64 allocated=session->allocatedBytes();UINT rejects=0;
    auto reject=[&](auto action){bool caught=false;try{action();}catch(const std::runtime_error&){caught=true;}if(!caught)throw std::runtime_error("Session contract rejection failed");++rejects;};
    reject([&](){registry.prepare(17,caller.inputTexture(),folder);});
    reject([&](){session->notifySubmitted(owner->fence.Get(),1);});
    if(multi){auto second=registry.prepare(18,caller.inputTexture(),modelFolder);
        if(registry.activeCount()!=2)throw std::runtime_error("Multiple features not retained");
        FixtureNgxParameters extra;extra.output=caller.inputTexture();
        if(NativeNrHook::Evaluate(18,owner->cmd.Get(),&extra,false,true,D3D12_RESOURCE_STATE_COMMON)!=NativeNrHook::Result::Recorded)throw std::runtime_error("Second feature not recorded");
        registry.retire(18);if(registry.collect()!=0||registry.pendingCount()!=1||registry.activeCount()!=1)throw std::runtime_error("Unsubmitted feature released early");
        check(owner->cmd->Close(),"Discard test list close");check(owner->allocator->Reset(),"Discard test allocator reset");check(owner->cmd->Reset(owner->allocator.Get(),nullptr),"Discard test list reset");
        second->cancelUnsubmitted();if(registry.collect()!=1||registry.pendingCount())throw std::runtime_error("Discarded feature retirement failed");second.reset();}
    ComPtr<ID3D12Fence> marker;check(owner->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&marker)),"Caller marker");
    FixtureNgxParameters params;params.output=caller.inputTexture();std::vector<float> first,last;
    for(UINT run=0;run<2;++run){
        if(format==DXGI_FORMAT_R32G32B32A32_FLOAT)caller.upload(folder+"/frame_input/colour.bin");else caller.uploadPacked(folder+"/frame_input/colour_texture.bin");
        params.voidOnly=run!=0;
        auto evaluate=[&](){return NativeNrHook::Evaluate(17,owner->cmd.Get(),&params,false,true,D3D12_RESOURCE_STATE_COPY_DEST);};
        if(evaluate()!=NativeNrHook::Result::Recorded||evaluate()!=NativeNrHook::Result::Skipped)throw std::runtime_error("Recorded-session reuse guard failed");
        reject([&](){session->captureFeatures();});
        reject([&](){session->notifySubmitted(marker.Get(),0);});
        if(run&&!tracked)reject([&](){session->notifySubmitted(marker.Get(),1);});
        if(tracked&&!detour)submissions.track(owner->cmd.Get(),session);
        check(owner->cmd->Close(),"Caller close");ID3D12CommandList* lists[]={owner->cmd.Get()};
        if(detour){owner->queue->ExecuteCommandLists(1,lists);if(NativeNrQueueHook::FailureCount()||submissions.recordedCount()||submissions.inFlightCount()!=1)throw std::runtime_error("Session queue detour association failed");}
        else if(tracked){if(submissions.execute(owner->queue.Get(),1,lists,[&](){owner->queue->ExecuteCommandLists(1,lists);})!=1)throw std::runtime_error("Session submission association failed");}
        else owner->queue->ExecuteCommandLists(1,lists);
        // Announce the caller's completion marker before queuing Signal. This
        // deterministically exercises a pending fence without blocking a GPU queue.
        if(!tracked){session->notifySubmitted(marker.Get(),run+1);
            if(evaluate()!=NativeNrHook::Result::Skipped||session->ready())throw std::runtime_error("In-flight session reused");}
        reject([&](){session->cancelUnsubmitted();});
        if(run==1){if(detour){if(!NativeNrLifecycle::Retire(17))throw std::runtime_error("Session release observer failed");}else registry.retire(17);
            if(!tracked&&(registry.collect()!=0||registry.pendingCount()!=1))throw std::runtime_error("Session retired before caller fence");}
        check(owner->queue->Signal(marker.Get(),run+1),"Caller completion signal");check(marker->SetEventOnCompletion(run+1,owner->event),"Caller completion event");
        if(WaitForSingleObject(owner->event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("Caller marker timeout");
        if(tracked){submissionCollections+=UINT(submissions.collect());if(submissions.inFlightCount()||submissions.recordedCount())throw std::runtime_error("Completed session submission retained");}
        check(owner->allocator->Reset(),"Caller allocator reset");check(owner->cmd->Reset(owner->allocator.Get(),nullptr),"Caller list reset");
        if(detour&&!NativeNrLifecycle::Reset(owner->cmd.Get(),S_OK))throw std::runtime_error("Session reset observer failed");
        caller.submit(); // Empty validation submission after complete inference.
        if(!run&&!session->ready())throw std::runtime_error("Session not reusable after completion");
        last=session->captureTexture(folder+"/native_texture_packed.bin");
        if(!run)first=last;else if(first.size()!=last.size()||memcmp(first.data(),last.data(),last.size()*4))throw std::runtime_error("Persistent session not repeatable");
    }
    for(float value:last)if(!std::isfinite(value))throw std::runtime_error("Non-finite session texture");
    if(session->allocatedBytes()!=allocated||session->recordedFrames()!=2)throw std::runtime_error("Session reinitialized during frames");
    save(folder+"/native_texture.bin",last);save(folder+"/native_features.bin",session->captureFeatures());save(folder+"/native_rgb.bin",session->captureRgb());
    auto& graph=session->captureGraph();save(folder+"/native.bin",graph.publicNodes().back()->runtime()->download(graph.resultTensor()));
    for(auto& node:graph.publicNodes()){save(folder+"/native_"+node->name+".bin",node->runtime()->download(node->output()));
        if(node->window&&node->window->skipTensor())save(folder+"/native_skip_"+node->name+".bin",node->runtime()->download(node->window->skipTensor()));}
    if(registry.collect()!=1||registry.pendingCount())throw std::runtime_error("Completed session retirement failed");
    session.reset();if(!weak.expired())throw std::runtime_error("Retired session ownership leaked");
    if(detour){if(!NativeNrLifecycle::Shutdown()||registry.activeCount()||registry.pendingCount())throw std::runtime_error("Completed lifecycle shutdown failed");
        if(NativeNrQueueHook::FailureCount()||!NativeNrQueueHook::Remove())throw std::runtime_error("Session queue detour removal failed");NativeNrLifecycle::Register(nullptr);}
    if(tracked&&submissionCollections!=2)throw std::runtime_error("Submission collection count mismatch");
    printf("{\"passed\":true,\"texture_pipeline\":true,\"cpu_intermediate_reads\":0,\"runs\":2,\"model_preparations\":1,\"recorded_frames\":2,\"hot_default_allocations\":0,\"owned_default_buffer_bytes\":%llu,\"held_until_caller_fence\":%s,\"retired_session_released\":true,\"rejections_verified\":%u,\"multiple_handles_tested\":%s,\"total_model_preparations\":%u,\"repeat_byte_identical\":true,\"submission_tracker\":%s,\"submission_collections\":%u,\"real_queue_detour\":%s,\"module_assets\":%s}\n",allocated,tracked?"false":"true",rejects,multi?"true":"false",multi?2u:1u,tracked?"true":"false",submissionCollections,detour?"true":"false",moduleAssets?"true":"false");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
