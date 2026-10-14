#include "dml_graph_pipeline.h"
#include "dml_texture.h"
#include "dml_frame.h"
#include "nr_tile_batch.h"
#include "nr_gpu_profile.h"
#include <chrono>

// Full-frame experimental scheduler. Extraction and ordered overlap composition
// stay on GPU. Extra buffer copies are deliberate until direct per-lane views land.
class BatchFrameProbe : public NativeRuntime {
public:
    void executeClosed(ID3D12GraphicsCommandList* list){
        ID3D12CommandList* lists[]={list};queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),++serial),"Frame replay fence");
        check(fence->SetEventOnCompletion(serial,event),"Frame replay event");
        if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("Frame replay timeout");
        check(device->GetDeviceRemovedReason(),"Frame replay device");
        // submit also drains and validates the debug queue after replay.
        submit();
    }
    void copyLane(Tensor* source,UINT lane,Tensor* target,UINT targetLane){
        if(source->elements!=target->elements||lane>=source->lanes||targetLane>=target->lanes)throw std::runtime_error("Tile copy lane/extent mismatch");
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);target->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(target->resource.Get(),UINT64(targetLane)*target->elements*4,source->resource.Get(),UINT64(lane)*source->elements*4,source->elements*4);
    }
};
int main(int argc,char** argv){try{
    if(argc!=7)throw std::runtime_error("Usage: dml_batch_frame assets fixture output B width height");
    UINT b=std::stoul(argv[4]),width=std::stoul(argv[5]),height=std::stoul(argv[6]);if(b!=1&&b!=2&&b!=4)throw std::runtime_error("B must be 1, 2 or 4");
    std::filesystem::create_directories(argv[3]);BatchFrameProbe probe;auto ctx=probe.sharedContext();probe.constrainToGpuBudget();
    TextureHarness textures(2560,1440,ctx,DXGI_FORMAT_R10G10B10A2_UNORM,nullptr,nullptr,320,320,true);
    textures.setProcessingExtent(width,height);textures.uploadPacked(argv[2]);auto tileResult=probe.make(320*320*3);
    ctx->tileBatch=b;ctx->reuseScratch=true;auto colour=probe.make(320*320*3);
#ifdef NR_EXPERIMENTAL_LINALG
    ctx->linAlg=GetEnvironmentVariableA("NR_GRAPH_LINALG",nullptr,0)!=0;
    ctx->linAlgQkvOnly=GetEnvironmentVariableA("NR_GRAPH_LINALG_QKV_ONLY",nullptr,0)!=0;
    ctx->linAlgFfnOnly=GetEnvironmentVariableA("NR_GRAPH_LINALG_FFN_ONLY",nullptr,0)!=0;
    ctx->linAlgGrouped=GetEnvironmentVariableA("NR_GRAPH_LINALG_GROUPED",nullptr,0)!=0;
    ctx->linAlgAttention=GetEnvironmentVariableA("NR_GRAPH_LINALG_ATTENTION",nullptr,0)!=0;
    ctx->linAlgAttentionInline=GetEnvironmentVariableA("NR_GRAPH_LINALG_ATTENTION_INLINE",nullptr,0)!=0;
    ctx->linAlgWindowFused=GetEnvironmentVariableA("NR_GRAPH_LINALG_WINDOW_FUSED",nullptr,0)!=0;
    ctx->linAlgWindowNormalizeFused=ctx->linAlgWindowFused&&GetEnvironmentVariableA("NR_GRAPH_LINALG_WINDOW_NORMALIZE_FUSED",nullptr,0)!=0;
#endif
    FrameHarness features(std::string(argv[1])+"/frame_input",ctx,nullptr,nullptr,colour);
    std::unique_ptr<GraphPipeline> graph;if(b>1)graph=BatchedGraph::create(argv[1],ctx,features.resultTensor());else graph=std::make_unique<GraphPipeline>(argv[1],ctx,features.resultTensor());
    FrameHarness composition(std::string(argv[1])+"/frame_output",ctx,graph->resultTensor(),colour);
#ifdef NR_EXPERIMENTAL_LINALG
    printf("LINALG_JOBS %llu OPTIMAL_WEIGHT_BYTES %llu\n",ctx->linAlgJobs,ctx->linAlgWeightBytes);
    printf("LINALG_ATTENTION_JOBS %llu LINALG_FUSED_WINDOWS %llu\n",ctx->linAlgAttentionJobs,ctx->linAlgFusedWindows);
#endif
    TileBatchPlan plan(width,height,b);auto profiler=std::make_shared<NativeGpuProfile>(ctx->device.Get());
    bool detailed=GetEnvironmentVariableA("NR_FRAME_PROFILE",nullptr,0)!=0;
    if(detailed)ctx->profiler=profiler;
    auto printProfile=[&](UINT run,const std::map<std::string,NativeGpuProfile::Total>& times){
        if(detailed)for(const auto& item:times)printf("PROFILE run=%u path=%s ms=%.9f count=%u\n",run,item.first.c_str(),item.second.ms,item.second.count);
    };
    char runSetting[16]{};UINT runs=3;if(GetEnvironmentVariableA("NR_FRAME_RUNS",runSetting,sizeof(runSetting)))runs=std::stoul(runSetting);
    if(runs<3||runs>32)throw std::runtime_error("Invalid frame sample count");
    bool replay=GetEnvironmentVariableA("NR_FRAME_REPLAY",nullptr,0)!=0;
    ComPtr<ID3D12CommandAllocator> frameAllocator;ComPtr<ID3D12GraphicsCommandList> frameList;
    for(UINT run=0;run<runs;++run){
        if(run>=2&&replay){probe.executeClosed(frameList.Get());
            auto times=profiler->readAfterFence(ctx->queue.Get());
            printProfile(run,times);
            textures.downloadTexture(std::string(argv[3])+"/frame"+std::to_string(run)+".bin");
            printf("RUN %u B %u WIDTH %u HEIGHT %u TILES %u BATCHES %zu CPU 0.000000 GPU %.6f BUFFER_BYTES %llu\n",run,b,width,height,textures.tileCount(),plan.batches.size(),times.at("frame").ms,ctx->defaultBufferBytes);
            puts("FRAME_CLOSED_LIST_REPLAY_COMPLETED");fflush(stdout);continue;}
        profiler->reset();auto start=std::chrono::steady_clock::now();
        std::vector<NativeTrackedState> states;std::vector<D3D12_RESOURCE_STATES> before;
        std::unique_ptr<ExternalCommandRecording> borrowed;
        if(run==1&&replay){probe.appendTrackedStates(states);textures.appendTextureStates(states);
            features.appendTrackedStates(states);composition.appendTrackedStates(states);
            for(auto& node:graph->publicNodes())node->runtime()->appendTrackedStates(states);
            for(auto& scratch:ctx->tensorScratch)states.push_back({scratch.second->resource.Get(),&scratch.second->state});
            for(auto s:states)before.push_back(*s.state);
            check(ctx->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&frameAllocator)),"Frame cache allocator");
            check(ctx->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,frameAllocator.Get(),nullptr,IID_PPV_ARGS(&frameList)),"Frame cache list");
            borrowed=std::make_unique<ExternalCommandRecording>(ctx,frameList.Get());}
        {
        NativeProfileRange range(profiler.get(),ctx->cmd.Get(),"frame");
        {NativeProfileRange stage(detailed?profiler.get():nullptr,ctx->cmd.Get(),"clear_tiles");textures.clearTiles();}
        for(const auto& batch:plan.batches){
            {
            NativeProfileRange stage(detailed?profiler.get():nullptr,ctx->cmd.Get(),"extract_and_copy");
            for(UINT lane=0;lane<b;++lane){auto tile=batch[std::min<size_t>(lane,batch.size()-1)];textures.selectTile(tile.x,tile.y);textures.recordInput();probe.copyLane(textures.colourTensor(),0,colour,lane);}
            }
            {NativeProfileRange stage(detailed?profiler.get():nullptr,ctx->cmd.Get(),"features");features.record();}
            {NativeProfileRange stage(detailed?profiler.get():nullptr,ctx->cmd.Get(),"graph");graph->record();}
            {NativeProfileRange stage(detailed?profiler.get():nullptr,ctx->cmd.Get(),"composition");composition.record();}
            {
            NativeProfileRange stage(detailed?profiler.get():nullptr,ctx->cmd.Get(),"copy_and_accumulate");
            for(UINT lane=0;lane<batch.size();++lane){
                probe.copyLane(composition.resultTensor(),lane,tileResult,0);
                // Accumulation subtracts this tile's original colour. Restore the
                // matching lane, rather than the last colour extracted in the batch.
                probe.copyLane(colour,lane,textures.colourTensor(),0);
                textures.selectTile(batch[lane].x,batch[lane].y);textures.accumulateTile(tileResult);
            }
            }
        }
        {NativeProfileRange stage(detailed?profiler.get():nullptr,ctx->cmd.Get(),"texture_output");textures.recordOutput(tileResult);}
    }
        profiler->resolve(ctx->cmd.Get());
        if(borrowed){for(size_t i=0;i<states.size();++i){auto& s=states[i];if(*s.state!=before[i])transition(ctx->cmd.Get(),s.resource,*s.state,before[i]);*s.state=before[i];}
            check(frameList->Close(),"Frame cache close");borrowed.reset();}
        auto recorded=std::chrono::steady_clock::now();
        if(run==1&&replay)probe.executeClosed(frameList.Get());else probe.submit();auto times=profiler->readAfterFence(ctx->queue.Get());
        printProfile(run,times);
        textures.downloadTexture(std::string(argv[3])+"/frame"+std::to_string(run)+".bin");
        printf("RUN %u B %u WIDTH %u HEIGHT %u TILES %u BATCHES %zu CPU %.6f GPU %.6f BUFFER_BYTES %llu\n",run,b,width,height,textures.tileCount(),plan.batches.size(),std::chrono::duration<double,std::milli>(recorded-start).count(),times.at("frame").ms,ctx->defaultBufferBytes);fflush(stdout);
    }
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
