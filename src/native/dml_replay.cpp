#include "nr_command_cache.h"
#include <chrono>
int main(int argc,char** argv){try{
    if(argc!=5)throw std::runtime_error("Usage: dml_replay.exe assets fixture processing-width processing-height");
    std::string folder=argv[2];UINT w=UINT(std::stoul(argv[3])),h=UINT(std::stoul(argv[4]));
    TextureHarness caller(2560,1440,nullptr,DXGI_FORMAT_R10G10B10A2_UNORM,nullptr,nullptr,320,320);
    auto ctx=caller.sharedContext();auto session=std::make_shared<NativeNrSession>(caller.inputTexture(),argv[1],true,true,true);session->setProcessingExtent(w,h);
    std::vector<float> baseline,alternate;double freshCpu=0,replayCpu=0,cacheBuildCpu=0;std::unique_ptr<NativeNrCommandCache> cache;
    auto profile=std::make_shared<NativeGpuProfile>(ctx->device.Get(),8);double gpu[6]{},wall[6]{};
    for(UINT run=0;run<6;++run){
        bool alt=run==3||run==4;bool replay=run==2||run==3||run==5;
        caller.uploadPacked(folder+(alt?"/alternate.bin":"/colour_texture.bin"));
        auto start=std::chrono::steady_clock::now();
        if(replay){
            if(!cache){profile->reset();cache=std::make_unique<NativeNrCommandCache>(session,caller.inputTexture(),profile);cacheBuildCpu=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();start=std::chrono::steady_clock::now();}
            if(!cache->markReadyForExecute())throw std::runtime_error("Cache request rejected");
            if(cache->markReadyForExecute())throw std::runtime_error("Cache accepted a second unsubmitted use");
            replayCpu+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            transition(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);caller.submit();
            start=std::chrono::steady_clock::now(); // fixture-only transition excluded
            ID3D12CommandList* lists[]={cache->commandList()};ctx->queue->ExecuteCommandLists(1,lists);
            session->notifySubmitted(ctx->fence.Get(),ctx->serial+1);
            if(cache->markReadyForExecute())throw std::runtime_error("Cache reused before completion signal");
            check(ctx->queue->Signal(ctx->fence.Get(),++ctx->serial),"Replay fence");
            check(ctx->fence->SetEventOnCompletion(ctx->serial,ctx->event),"Replay event");if(WaitForSingleObject(ctx->event,120000)!=WAIT_OBJECT_0)throw std::runtime_error("Replay timeout");
            wall[run]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            transition(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
            caller.submit(); // collect debug diagnostics on completed work, outside timing
        }else{
            profile->reset();
            {NativeProfileRange frame(profile.get(),ctx->cmd.Get(),"frame");
            transition(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
            if(!session->record(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_PRESENT)){session->cancelUnsubmitted();throw std::runtime_error("Fresh record failed");}
            transition(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
            }
            if(run==1)freshCpu=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            profile->resolve(ctx->cmd.Get());session->notifySubmitted(ctx->fence.Get(),ctx->serial+1);caller.submit();
            wall[run]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        }
        if(!session->ready())throw std::runtime_error("Completion missing");
        gpu[run]=profile->readAfterFence(ctx->queue.Get()).at("frame").ms;
        auto out=caller.downloadTexture(folder+"/replay"+std::to_string(run)+".bin",true);
        if(run==1)baseline=out;
        if(run==2||run==5)if(out!=baseline)throw std::runtime_error("Replay differs from fresh original");
        if(run==3)alternate=out;
        if(run==4&&out!=alternate)throw std::runtime_error("Changed-input cached result differs from fresh");
        // A fresh recording may change tracked states: explicitly verify reject,
        // rather than attempting an old cache after an incompatible mutation.
    }
    if(baseline==alternate)throw std::runtime_error("Replay ignored changed input");
    session->setProcessingExtent(w==1920?2560:1920,w==1920?1440:1080);
    if(cache->matches(session,caller.inputTexture())||cache->markReadyForExecute())throw std::runtime_error("Stale resolution cache accepted");
    cache.reset();session->requestRetire();session.reset();
    printf("{\"passed\":true,\"processing_width\":%u,\"processing_height\":%u,\"fresh_cpu_record_ms\":%.6f,\"cache_build_cpu_ms\":%.6f,\"mean_cached_cpu_ms\":%.6f,\"fresh_gpu_ms\":%.6f,\"cached_gpu_ms\":[%.6f,%.6f],\"fresh_record_submit_wait_ms\":%.6f,\"cached_submit_wait_ms\":[%.6f,%.6f],\"changed_input_matches_fresh\":true,\"original_matches_fresh\":true,\"busy_reuse_rejected\":true,\"stale_resolution_rejected\":true,\"runs\":6,\"game_test\":false}\n",w,h,freshCpu,cacheBuildCpu,replayCpu/3,gpu[1],gpu[3],gpu[5],wall[1],wall[3],wall[5]);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
