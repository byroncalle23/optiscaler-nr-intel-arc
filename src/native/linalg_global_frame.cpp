#ifdef NR_GLOBAL_INT8_CAPTURE
#define NR_GRAPH_DIAGNOSTICS
#include <regex>
#endif
#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main

class GlobalFrameProbe : public BatchGraphProbe {
    ComPtr<ID3D12RootSignature> root;
    std::map<std::string,ComPtr<ID3D12PipelineState>> pipelines;
    UINT shape[6];
public:
    Tensor *packed,*base,*low,*output;
#ifdef NR_GLOBAL_INT8_CAPTURE
    struct Snapshot{std::string name;UINT64 bytes;ComPtr<ID3D12Resource> readback;};
    std::vector<Snapshot> snapshots;
    void captureFfn(const std::string& name,Tensor* tensor){
        std::smatch match;
        if(!std::regex_match(name,match,std::regex("bottleneck_gemm([0-9]+)_(a|w|out)")))return;
        UINT index=std::stoul(match[1].str());if(index%6>1)return;
        UINT64 bytes=tensor->elements*tensor->lanes*4;
        auto rb=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto before=tensor->state;tensor->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(rb.Get(),0,tensor->resource.Get(),0,bytes);tensor->to(cmd.Get(),before);
        snapshots.push_back({name,bytes,rb});
    }
    void saveSnapshots(const std::string& folder){
        std::ofstream manifest(nativePath(folder+"/snapshots.tsv"));
        if(snapshots.size()!=48)throw std::runtime_error("Expected16 FFN jobs times3 snapshots");
        for(auto& s:snapshots){void* data;D3D12_RANGE range{0,size_t(s.bytes)},empty{0,0};
            check(s.readback->Map(0,&range,&data),"Global FFN readback");
            std::ofstream f(nativePath(folder+"/"+s.name+".bin"),std::ios::binary);f.write(static_cast<const char*>(data),std::streamsize(s.bytes));
            s.readback->Unmap(0,&empty);if(!f)throw std::runtime_error("Global FFN snapshot write");manifest<<s.name<<'\t'<<s.bytes<<'\n';
        }
        if(!manifest)throw std::runtime_error("Global FFN manifest write");
        snapshots.clear();puts("GLOBAL_FFN_CAPTURE_PASS jobs=16 snapshots=48");
    }
#endif
    GlobalFrameProbe(UINT width,UINT height,UINT lowWidth,UINT lowHeight):shape{2560,1440,width,height,lowWidth,lowHeight}{
        if(!lowWidth||!lowHeight||width%lowWidth||height%lowHeight)throw std::runtime_error("Global box reduction requires integral ratios");
        constrainToGpuBudget();packed=make(2560*1440);base=make(UINT64(width)*height*3);
        low=make(UINT64(lowWidth)*lowHeight*3);output=make(UINT64(width)*height);
        D3D12_ROOT_PARAMETER p[6]{};
        for(UINT i=0;i<4;++i){p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[i].Descriptor.ShaderRegister=i;}
        p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[5].Constants.Num32BitValues=6;
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=6;desc.pParameters=p;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Global root serialize");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Global root");
        auto source=linAlgShader("global_frame.hlsl");
        for(const char* entry:{"PrepareBase","Reduce","ComposeHigh"}){
            auto hr=D3DCompile(source.data(),source.size(),"global_frame.hlsl",nullptr,nullptr,entry,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
            if(FAILED(hr)&&error)fprintf(stderr,"%s\n",static_cast<char*>(error->GetBufferPointer()));check(hr,entry);
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=root.Get();d.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            check(device->CreateComputePipelineState(&d,IID_PPV_ARGS(&pipelines[entry])),entry);
        }
    }
    void record(const char* entry,Tensor* composed){
        Tensor* target=strcmp(entry,"PrepareBase")==0?base:strcmp(entry,"Reduce")==0?low:output;
        packed->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(target!=base)base->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(target==output){low->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);composed->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipelines.at(entry).Get());
        Tensor* views[]={packed,target!=base?base:packed,target==output?low:packed,target==output?composed:packed};
        for(UINT i=0;i<4;++i)cmd->SetComputeRootShaderResourceView(i,views[i]->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(4,target->resource->GetGPUVirtualAddress());cmd->SetComputeRoot32BitConstants(5,6,shape,0);
        UINT count=target==output?UINT(output->elements):UINT(target->elements/3),groups=(count+127)/128;
        cmd->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,1);uavBarrier(cmd.Get());
    }
    void saveRaw(Tensor* tensor,const std::string& path){auto values=downloadLanes(tensor);std::ofstream f(nativePath(path),std::ios::binary);f.write(reinterpret_cast<const char*>(values.data()),values.size()*4);if(!f)throw std::runtime_error("Global capture write failed");}
};
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--shader-check"){
        auto source=linAlgShader("global_frame.hlsl");
        for(const char* entry:{"PrepareBase","Reduce","ComposeHigh"}){ComPtr<ID3DBlob> blob,error;
            auto hr=D3DCompile(source.data(),source.size(),"global_frame.hlsl",nullptr,nullptr,entry,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
            if(FAILED(hr)&&error)fprintf(stderr,"%s\n",static_cast<char*>(error->GetBufferPointer()));check(hr,entry);printf("SHADER_CHECK_PASS %s\n",entry);}
        return 0;
    }
    if(argc!=7)throw std::runtime_error("Usage: global_frame assets packedfixture output lowWidth lowHeight native");
    UINT lw=std::stoul(argv[4]),lh=std::stoul(argv[5]);bool native=std::stoul(argv[6])!=0;
    std::filesystem::create_directories(nativePath(argv[3]));GlobalFrameProbe probe(1920,1080,lw,lh);auto ctx=probe.sharedContext();ctx->tileBatch=1;ctx->reuseScratch=true;
    ctx->linAlg=native;ctx->linAlgAttention=native;ctx->linAlgWindowFused=native;
#ifdef NR_C32_PROJECTION
#ifdef NR_ATTENTION_REGISTERS
    char registersPolicy[24]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_ATTENTION_REGISTERS",registersPolicy,sizeof(registersPolicy)))ctx->attentionRegistersPolicy=registersPolicy;
    if(ctx->attentionRegistersPolicy!="baseline"&&ctx->attentionRegistersPolicy!="fused")throw std::runtime_error("Unknown attention registers policy");
    if(!native&&ctx->attentionRegistersPolicy!="baseline")throw std::runtime_error("Attention registers require native graph");
#endif
#ifdef NR_DENSE_WAVE
    char denseWavePolicy[24]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_DENSE_WAVE",denseWavePolicy,sizeof(denseWavePolicy)))ctx->denseWavePolicy=denseWavePolicy;
    if(ctx->denseWavePolicy!="baseline"&&ctx->denseWavePolicy!="wave64")throw std::runtime_error("Unknown dense Wave policy");
    if(!native&&ctx->denseWavePolicy!="baseline")throw std::runtime_error("Dense Wave requires native graph");
#endif
#ifdef NR_C32_FULL_FFN
    char fullFfnPolicy[24]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_C32_FULL_FFN",fullFfnPolicy,sizeof(fullFfnPolicy)))ctx->c32FullFfnPolicy=fullFfnPolicy;
    if(ctx->c32FullFfnPolicy!="baseline"&&ctx->c32FullFfnPolicy!="fused")throw std::runtime_error("Unknown C32 FFN policy");
    if(!native&&ctx->c32FullFfnPolicy!="baseline")throw std::runtime_error("Full FFN requires native graph");
#endif
    char projectionPolicy[32]{};
#ifdef NR_WINDOW_SEGMENT
    char segmentPolicy[24]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_WINDOW_SEGMENT",segmentPolicy,sizeof(segmentPolicy)))ctx->windowSegmentPolicy=segmentPolicy;
    if(ctx->windowSegmentPolicy!="baseline"&&ctx->windowSegmentPolicy!="fused")throw std::runtime_error("Unknown window segment policy");
    if(!native&&ctx->windowSegmentPolicy!="baseline")throw std::runtime_error("Window segment requires native graph");
#endif
    if(GetEnvironmentVariableA("NR_GLOBAL_C32_PROJECTION",projectionPolicy,sizeof(projectionPolicy)))ctx->c32ProjectionPolicy=projectionPolicy;
    if(ctx->c32ProjectionPolicy!="baseline"&&ctx->c32ProjectionPolicy!="qkv"&&ctx->c32ProjectionPolicy!="context"&&ctx->c32ProjectionPolicy!="both")throw std::runtime_error("Unknown C32 projection policy");
    if(!native&&ctx->c32ProjectionPolicy!="baseline")throw std::runtime_error("C32 projection requires native graph");
#endif
#ifdef NR_SOFTMAX_WAVE
    char softmaxWavePolicy[32]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_SOFTMAX_WAVE",softmaxWavePolicy,sizeof(softmaxWavePolicy)))ctx->softmaxWavePolicy=softmaxWavePolicy;
    if(ctx->softmaxWavePolicy!="baseline"&&ctx->softmaxWavePolicy!="wave8")throw std::runtime_error("Unknown softmax Wave policy");
    if(!native&&ctx->softmaxWavePolicy!="baseline")throw std::runtime_error("Softmax Wave requires native graph");
#endif
#ifdef NR_NORM_WAVE
    char normWavePolicy[32]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_NORM_WAVE",normWavePolicy,sizeof(normWavePolicy)))ctx->normWavePolicy=normWavePolicy;
    if(ctx->normWavePolicy!="baseline"&&ctx->normWavePolicy!="wave8")throw std::runtime_error("Unknown Wave normalization policy");
    if(!native&&ctx->normWavePolicy!="baseline")throw std::runtime_error("Wave normalization requires native graph");
#endif
#ifdef NR_DENSE_TILE
    char denseTile[32]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_DENSE_TILE",denseTile,sizeof(denseTile))){
        std::string value=denseTile;
        if(value!="32"&&value!="64")throw std::runtime_error("Unknown dense tile policy");
        ctx->denseTileN=std::stoul(value);
    }
    if(!native&&ctx->denseTileN!=64)throw std::runtime_error("Dense tile requires native graph");
#endif
#ifdef NR_ATTENTION_PARTITION
    char partitionPolicy[64]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_ATTENTION_PARTITION",partitionPolicy,sizeof(partitionPolicy)))ctx->attentionPartitionPolicy=partitionPolicy;
    if(ctx->attentionPartitionPolicy!="baseline"&&ctx->attentionPartitionPolicy!="wg64"&&ctx->attentionPartitionPolicy!="wg128"&&ctx->attentionPartitionPolicy!="wg256")throw std::runtime_error("Unknown attention partition policy");
    if(!native&&ctx->attentionPartitionPolicy!="baseline")throw std::runtime_error("Attention partition requires native graph");
#endif
#ifdef NR_C32_COMPACT
    char c32Policy[64]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_C32_POLICY",c32Policy,sizeof(c32Policy)))ctx->c32CompactPolicy=c32Policy;
    if(ctx->c32CompactPolicy!="baseline"&&ctx->c32CompactPolicy!="compact-f16"&&ctx->c32CompactPolicy!="compact-partials")throw std::runtime_error("Unknown C32 policy");
    if(!native&&ctx->c32CompactPolicy!="baseline")throw std::runtime_error("C32 compact requires native attention");
#endif
#ifdef NR_GLOBAL_NORMALIZATION
    char normalizationPolicy[64]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_NORMALIZATION_POLICY",normalizationPolicy,sizeof(normalizationPolicy)))ctx->globalNormalizationPolicy=normalizationPolicy;
    if(ctx->globalNormalizationPolicy!="baseline"&&ctx->globalNormalizationPolicy!="fused"&&ctx->globalNormalizationPolicy!="fused-native-round")throw std::runtime_error("Unknown global normalization policy");
    if(!native&&ctx->globalNormalizationPolicy!="baseline")throw std::runtime_error("Global normalization requires native window attention");
    ctx->linAlgWindowNormalizeFused=ctx->globalNormalizationPolicy!="baseline";
#endif
#ifdef NR_BRANCH_COMPACT
    ctx->branchCompact=GetEnvironmentVariableA("NR_GLOBAL_BRANCH_COMPACT",nullptr,0)!=0;
#endif
#ifdef NR_GLOBAL_INT8
    char int8Policy[64]{},int8Root[32768]{},int8Calibration[32768]{};
    if(!GetEnvironmentVariableA("NR_GLOBAL_INT8_POLICY",int8Policy,sizeof(int8Policy)))throw std::runtime_error("Global INT8 policy missing");
    ctx->globalInt8Policy=int8Policy;
    ctx->globalInt8Segment=GetEnvironmentVariableA("NR_GLOBAL_INT8_SEGMENT",nullptr,0)!=0;
    if(ctx->globalInt8Segment&&ctx->globalInt8Policy!="both")throw std::runtime_error("Compact FFN policy must be both");
    if(ctx->globalInt8Policy!="none"&&ctx->globalInt8Policy!="expand"&&ctx->globalInt8Policy!="contract"&&ctx->globalInt8Policy!="both")throw std::runtime_error("Invalid global INT8 policy");
    if(!native)throw std::runtime_error("Global INT8 suite requires native reference");
    if(ctx->globalInt8Policy!="none"){
        if(!GetEnvironmentVariableA("NR_GLOBAL_INT8_WEIGHTS",int8Root,sizeof(int8Root))||!GetEnvironmentVariableA("NR_GLOBAL_INT8_CALIBRATION",int8Calibration,sizeof(int8Calibration)))throw std::runtime_error("Global INT8 assets missing");
        ctx->globalInt8WeightRoot=int8Root;std::ifstream calibration(nativePath(int8Calibration));std::string key;float scale;
        while(calibration>>key>>scale)if(!std::isfinite(scale)||scale<=0||!ctx->globalInt8Scales.emplace(key,scale).second)throw std::runtime_error("Invalid global INT8 calibration");
        if(!calibration.eof()||ctx->globalInt8Scales.size()!=16)throw std::runtime_error("Incomplete global INT8 calibration");
    }
#endif
    std::ifstream f(nativePath(argv[2]),std::ios::binary|std::ios::ate);if(!f||f.tellg()!=std::streamoff(2560*1440*4))throw std::runtime_error("Global fixture size mismatch");
    f.seekg(0);std::vector<float> raw(2560*1440);f.read(reinterpret_cast<char*>(raw.data()),raw.size()*4);probe.uploadLanes(probe.packed,raw);
    FrameHarness features(std::string(argv[1])+"/frame_input",ctx,nullptr,nullptr,probe.low);
    GraphPipeline graph(argv[1],ctx,features.resultTensor());FrameHarness composition(std::string(argv[1])+"/frame_output",ctx,graph.resultTensor(),probe.low);
    bool detailed=GetEnvironmentVariableA("NR_GLOBAL_DETAILED_PROFILE",nullptr,0)!=0;
#ifdef NR_GLOBAL_INT8_CAPTURE
    bool captureRun=GetEnvironmentVariableA("NR_GLOBAL_INT8_SKIP_CAPTURE",nullptr,0)==0;
#endif
    auto profiler=std::make_shared<NativeGpuProfile>(ctx->device.Get(),detailed?65536:128);
    if(detailed)ctx->profiler=profiler;
    ComPtr<ID3D12CommandAllocator> cacheAllocator;ComPtr<ID3D12GraphicsCommandList> cache;
#ifdef NR_DENSE_TILE
    printf("GLOBAL_DENSE_TILE %u\n",ctx->denseTileN);
#endif
#ifdef NR_NORM_WAVE
    printf("GLOBAL_NORM_WAVE %s\n",ctx->normWavePolicy.c_str());
#endif
#ifdef NR_SOFTMAX_WAVE
    printf("GLOBAL_SOFTMAX_WAVE %s\n",ctx->softmaxWavePolicy.c_str());
#endif
#ifdef NR_C32_PROJECTION
#ifdef NR_WINDOW_SEGMENT
    printf("GLOBAL_WINDOW_SEGMENT %s REUSED %u C32_FUSED %u\n",ctx->windowSegmentPolicy.c_str(),ctx->windowFeedReused,ctx->windowC32Fused);
#endif
#ifdef NR_ATTENTION_REGISTERS
    printf("GLOBAL_ATTENTION_REGISTERS %s WINDOWS %llu\n",ctx->attentionRegistersPolicy.c_str(),ctx->linAlgFusedWindows);
#endif
#ifdef NR_DENSE_WAVE
    printf("GLOBAL_DENSE_WAVE %s JOBS %u\n",ctx->denseWavePolicy.c_str(),ctx->denseWaveJobs);
#endif
#ifdef NR_C32_FULL_FFN
    printf("GLOBAL_C32_FULL_FFN %s JOBS %u\n",ctx->c32FullFfnPolicy.c_str(),ctx->c32FullFfnJobs);
#endif
    printf("GLOBAL_C32_PROJECTION %s QKV %u CONTEXT %u\n",ctx->c32ProjectionPolicy.c_str(),ctx->c32QkvJobs,ctx->c32ContextJobs);
#endif
    printf("GLOBAL B 1 GRAPH_PASSES 1 LOW %u %u LINALG_JOBS %llu FUSED_WINDOWS %llu\n",lw,lh,ctx->linAlgJobs,ctx->linAlgFusedWindows);
#ifdef NR_BRANCH_COMPACT
    printf("GLOBAL_COMPACT_BRANCH_WINDOWS %u\n",ctx->branchCompactWindows);
#endif
#ifdef NR_C32_COMPACT
    printf("GLOBAL_C32_POLICY %s WINDOWS %u\n",ctx->c32CompactPolicy.c_str(),ctx->c32CompactWindows);
#endif
#ifdef NR_GLOBAL_NORMALIZATION
    printf("GLOBAL_NORMALIZATION_POLICY %s WINDOWS %llu\n",ctx->globalNormalizationPolicy.c_str(),ctx->linAlgFusedWindows);
#endif
#ifdef NR_ATTENTION_PARTITION
    printf("GLOBAL_ATTENTION_PARTITION %s WINDOWS %llu\n",ctx->attentionPartitionPolicy.c_str(),ctx->linAlgFusedWindows);
#endif
#ifdef NR_GLOBAL_INT8
    UINT expected=ctx->globalInt8Policy=="none"?0:ctx->globalInt8Policy=="both"?16:8;
    if(ctx->globalInt8Jobs!=expected)throw std::runtime_error("Global INT8 substituted job count mismatch");
    printf("GLOBAL_INT8_POLICY %s JOBS %u\n",ctx->globalInt8Policy.c_str(),ctx->globalInt8Jobs);
    if(ctx->globalInt8Segment&&ctx->globalInt8Segments!=8)throw std::runtime_error("Compact FFN segment count mismatch");
    printf("GLOBAL_INT8_SEGMENTS %u\n",ctx->globalInt8Segments);
#endif
    for(auto& node:graph.publicNodes())if(node->bottleneck)printf("GLOBAL_TOKENS %u\n",node->bottleneck->tokenCount());
    UINT runs=8;char runSetting[16]{};
    if(GetEnvironmentVariableA("NR_GLOBAL_RUNS",runSetting,sizeof(runSetting)))runs=std::stoul(runSetting);
    if(runs<8||runs>256)throw std::runtime_error("Global frame run count outside8..256");
    for(UINT run=0;run<runs;++run){
        auto start=std::chrono::steady_clock::now();double recordMs=0;
        if(run>=2)probe.executeClosed(cache.Get());else{
            profiler->reset();std::vector<NativeTrackedState> states;std::vector<D3D12_RESOURCE_STATES> before;
            std::unique_ptr<ExternalCommandRecording> borrowed;
            if(run==1){probe.appendTrackedStates(states);features.appendTrackedStates(states);composition.appendTrackedStates(states);
                for(auto& node:graph.publicNodes())node->runtime()->appendTrackedStates(states);
                for(auto& scratch:ctx->tensorScratch)states.push_back({scratch.second->resource.Get(),&scratch.second->state});
                for(auto s:states)before.push_back(*s.state);
                check(ctx->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&cacheAllocator)),"Global replay allocator");
                check(ctx->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,cacheAllocator.Get(),nullptr,IID_PPV_ARGS(&cache)),"Global replay list");
                borrowed=std::make_unique<ExternalCommandRecording>(ctx,cache.Get());}
            {NativeProfileRange all(profiler.get(),ctx->cmd.Get(),"frame");
                {NativeProfileRange stage(profiler.get(),ctx->cmd.Get(),"prepare_base");probe.record("PrepareBase",composition.resultTensor());}
                {NativeProfileRange stage(profiler.get(),ctx->cmd.Get(),"reduce");probe.record("Reduce",composition.resultTensor());}
                {NativeProfileRange stage(profiler.get(),ctx->cmd.Get(),"features");features.record();}
                {NativeProfileRange stage(profiler.get(),ctx->cmd.Get(),"graph");
#ifdef NR_GLOBAL_INT8_CAPTURE
                    if(run==0&&captureRun){for(auto& node:graph.publicNodes()){
                        ctx->diagnosticNode=node->name;ctx->diagnosticJob=0;ctx->diagnosticShader=0;
                        ctx->capture=node->bottleneck?std::function<void(const std::string&,Tensor*)>([&](const std::string& name,Tensor* tensor){probe.captureFfn(name,tensor);}):std::function<void(const std::string&,Tensor*)>{};
                        node->record();ctx->capture={};
                    }}else graph.record();
#else
                    graph.record();
#endif
                }
                {NativeProfileRange stage(profiler.get(),ctx->cmd.Get(),"compose_low");composition.record();}
                {NativeProfileRange stage(profiler.get(),ctx->cmd.Get(),"upsample_and_compose");probe.record("ComposeHigh",composition.resultTensor());}
            }
            profiler->resolve(ctx->cmd.Get());
            if(borrowed){for(size_t i=0;i<states.size();++i){auto s=states[i];if(*s.state!=before[i])transition(ctx->cmd.Get(),s.resource,*s.state,before[i]);*s.state=before[i];}
                check(cache->Close(),"Global close replay");borrowed.reset();}
            recordMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(run==1)probe.executeClosed(cache.Get());else probe.submit();
        }
        auto times=profiler->readAfterFence(ctx->queue.Get());for(auto& row:times)printf("TIME run=%u name=%s ms=%.9f\n",run,row.first.c_str(),row.second.ms);
        printf("RUN %u CPU %.6f GPU %.9f BUFFER_BYTES %llu\n",run,recordMs,times.at("frame").ms,ctx->defaultBufferBytes);
        probe.saveRaw(probe.output,std::string(argv[3])+"/frame"+std::to_string(run)+".bin");
#ifdef NR_GLOBAL_INT8_CAPTURE
        if(run==0&&captureRun)probe.saveSnapshots(argv[3]);
#endif
        if(run==0){probe.saveRaw(probe.base,std::string(argv[3])+"/base.bin");probe.saveRaw(probe.low,std::string(argv[3])+"/low.bin");probe.saveRaw(composition.resultTensor(),std::string(argv[3])+"/composed.bin");}
        fflush(stdout);
    }
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
