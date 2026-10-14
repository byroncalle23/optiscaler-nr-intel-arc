#include "dml_graph_pipeline.h"
#include "dml_frame.h"
#include "nr_tile_batch.h"
#include <filesystem>
#include <chrono>

// Standalone validation only. CPU setup/readback do not belong to the inference pass.
class BatchGraphProbe : public NativeRuntime {
public:
    using NativeRuntime::NativeRuntime;
    void packingTest(){
        Tensor* source=make(64*3072),*target=make(64*3072),*parameter=make(32,false);
        std::vector<float> input(size_t(source->elements*source->lanes));for(size_t i=0;i<input.size();++i)input[i]=float(i%4096+(i/source->elements)*4096);
        uploadLanes(source,input);
        D3D12_ROOT_PARAMETER p[6]{};for(UINT i=0;i<3;++i){p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[i].Descriptor.ShaderRegister=i;}
        p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[4].Constants.Num32BitValues=4;batchRoot(p[5]);
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=6;rd.pParameters=p;ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Pack probe root serialize");
        ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Pack probe root");
        blob=compileBatchedShader("block_ops.hlsl","PackHeads");D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"Pack probe PSO");
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipeline.Get());
        cmd->SetComputeRootShaderResourceView(0,source->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,source->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(2,parameter->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(3,target->resource->GetGPUVirtualAddress());
        UINT constants[]={64,1024,64*3072,0};cmd->SetComputeRoot32BitConstants(4,4,constants,0);bindBatch(5,{source,source,parameter,target});cmd->Dispatch(1536,1,context->tileBatch);submit();auto output=downloadLanes(target);
        for(UINT lane=0;lane<context->tileBatch;++lane)for(UINT i=0;i<64*3072;++i){UINT part=i/(64*1024),local=i%(64*1024),head=local/(64*32),token=(local/32)%64,c=local%32;
            if(output[size_t(lane)*64*3072+i]!=input[size_t(lane)*64*3072+token*3072+part*1024+head*32+c])throw std::runtime_error("Batch pack mismatch");}
        puts("PACK_PROBE_EXACT");
    }
    void uploadLanes(Tensor* tensor,const std::vector<float>& values){
        UINT64 bytes=tensor->elements*tensor->lanes*4;
        if(values.size()*4!=bytes)throw std::runtime_error("Batch upload size mismatch");
        auto upload=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p;D3D12_RANGE empty{0,0};check(upload->Map(0,&empty,&p),"Batch setup upload");memcpy(p,values.data(),size_t(bytes));upload->Unmap(0,nullptr);
        tensor->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);cmd->CopyBufferRegion(tensor->resource.Get(),0,upload.Get(),0,bytes);submit();
    }
    std::vector<float> downloadLanes(Tensor* tensor){
        UINT64 bytes=tensor->elements*tensor->lanes*4;auto rb=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto before=tensor->state;tensor->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(rb.Get(),0,tensor->resource.Get(),0,bytes);tensor->to(cmd.Get(),before);submit();
        void* p;D3D12_RANGE range{0,size_t(bytes)},empty{0,0};check(rb->Map(0,&range,&p),"Batch validation readback");
        std::vector<float> result(size_t(bytes/4));memcpy(result.data(),p,size_t(bytes));rb->Unmap(0,&empty);return result;
    }
    void executeClosed(ID3D12GraphicsCommandList* list){ID3D12CommandList* lists[]={list};queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),++serial),"Replay signal");check(fence->SetEventOnCompletion(serial,event),"Replay completion");
        if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("Replay timeout");check(device->GetDeviceRemovedReason(),"Replay device state");}
};

int main(int argc,char** argv){try{
    if(argc!=5)throw std::runtime_error("Usage: dml_batch_graph assets output B lanePatternBase");
    UINT batch=std::stoul(argv[3]),base=std::stoul(argv[4]);std::string patterns=argv[4];auto separator=patterns.find(':');
    UINT second=separator==std::string::npos?base+1:std::stoul(patterns.substr(separator+1));
    if(batch!=1&&batch!=2&&batch!=4)throw std::runtime_error("Only B=1/2/4 supported");
    TileBatchPlan p1080(1920,1080,2),p1440(2560,1440,2);
    if(p1080.batches.size()!=23||p1080.batches.back().size()!=1||p1440.batches.size()!=42)throw std::runtime_error("Tile batch tail validation failed");
    std::filesystem::create_directories(argv[2]);BatchGraphProbe probe;auto context=probe.sharedContext();context->tileBatch=batch;
#ifdef NR_EXPERIMENTAL_LINALG
    context->linAlg=GetEnvironmentVariableA("NR_GRAPH_LINALG",nullptr,0)!=0;
    context->linAlgQkvOnly=GetEnvironmentVariableA("NR_GRAPH_LINALG_QKV_ONLY",nullptr,0)!=0;
    context->linAlgFfnOnly=GetEnvironmentVariableA("NR_GRAPH_LINALG_FFN_ONLY",nullptr,0)!=0;
    context->linAlgGrouped=GetEnvironmentVariableA("NR_GRAPH_LINALG_GROUPED",nullptr,0)!=0;
    context->linAlgAttention=GetEnvironmentVariableA("NR_GRAPH_LINALG_ATTENTION",nullptr,0)!=0;
    context->linAlgAttentionInline=GetEnvironmentVariableA("NR_GRAPH_LINALG_ATTENTION_INLINE",nullptr,0)!=0;
    context->linAlgWindowFused=GetEnvironmentVariableA("NR_GRAPH_LINALG_WINDOW_FUSED",nullptr,0)!=0;
    context->linAlgWindowNormalizeFused=context->linAlgWindowFused&&GetEnvironmentVariableA("NR_GRAPH_LINALG_WINDOW_NORMALIZE_FUSED",nullptr,0)!=0;
#endif
    if(GetEnvironmentVariableA("NR_BATCH_PACK_ONLY",nullptr,0)){probe.packingTest();return 0;}
    probe.constrainToGpuBudget();context->reuseScratch=true;
    std::ifstream config(nativePath(std::string(argv[1])+"/frame_input/shape.txt"));UINT h,w,th,tw;if(!(config>>h>>w>>th>>tw))throw std::runtime_error("Missing feature shape");
    Tensor* colour=probe.make(UINT64(h)*w*3);std::vector<float> input(size_t(colour->elements*batch));
    for(UINT lane=0;lane<batch;++lane)for(UINT64 i=0;i<colour->elements;++i){UINT pattern=lane?second:base;
        if(batch==4)pattern=lane==3?second:base+lane;
        input[size_t(lane*colour->elements+i)]=float((i*17+(i/3/w)*31+pattern*73)%257)/256.0f;}
    probe.uploadLanes(colour,input);fprintf(stderr,"SETUP features\n");FrameHarness features(std::string(argv[1])+"/frame_input",context,nullptr,nullptr,colour);
    std::unique_ptr<GraphPipeline> graph;if(batch>1)graph=BatchedGraph::create(argv[1],context,features.resultTensor());else graph=std::make_unique<GraphPipeline>(argv[1],context,features.resultTensor());
    fprintf(stderr,"SETUP graph complete\n");FrameHarness compose(std::string(argv[1])+"/frame_output",context,graph->resultTensor(),colour);
#ifdef NR_EXPERIMENTAL_LINALG
    printf("LINALG_JOBS %llu OPTIMAL_WEIGHT_BYTES %llu\n",context->linAlgJobs,context->linAlgWeightBytes);
    printf("LINALG_GROUPED_JOBS %llu\n",context->linAlgGroupedJobs);
    printf("LINALG_ATTENTION_JOBS %llu\n",context->linAlgAttentionJobs);
    printf("LINALG_FUSED_WINDOWS %llu\n",context->linAlgFusedWindows);
    printf("LINALG_NORMALIZED_WINDOWS %llu\n",context->linAlgWindowNormalizeFused?context->linAlgFusedWindows:0);
#endif
    D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;ComPtr<ID3D12QueryHeap> queries;
    check(context->device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)),"Batch timer");
    auto ticks=buffer(context->device.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12CommandAllocator> cacheAllocator;ComPtr<ID3D12GraphicsCommandList> cache;
    bool reuse=GetEnvironmentVariableA("NR_BATCH_REPLAY",nullptr,0)!=0;
    char runSetting[16]{};UINT runs=3;
    if(GetEnvironmentVariableA("NR_BATCH_RUNS",runSetting,sizeof(runSetting)))runs=std::stoul(runSetting);
    if(runs<3||runs>32)throw std::runtime_error("Batch run count must be 3..32");
    for(UINT run=0;run<runs;++run){auto start=std::chrono::steady_clock::now();
        if(run>=2&&reuse){probe.executeClosed(cache.Get());
            UINT64 frequency;check(context->queue->GetTimestampFrequency(&frequency),"Replay batch clock");
            void* mapped;D3D12_RANGE range{0,16},empty{0,0};check(ticks->Map(0,&range,&mapped),"Replay ticks");auto t=static_cast<UINT64*>(mapped);
            double gpu=double(t[1]-t[0])*1000/frequency;ticks->Unmap(0,&empty);
            printf("RUN %u B %u CPU 0.000000 GPU %.6f BUFFER_BYTES %llu\n",run,batch,gpu,context->defaultBufferBytes);
            auto output=probe.downloadLanes(compose.resultTensor());
            std::ofstream file(std::string(argv[2])+"/rgb"+std::to_string(run)+".bin",std::ios::binary);file.write(reinterpret_cast<const char*>(output.data()),std::streamsize(output.size()*4));
            puts("BATCH_CLOSED_LIST_REPLAY_COMPLETED");continue;}
        std::vector<NativeTrackedState> states;std::vector<D3D12_RESOURCE_STATES> before;
        std::unique_ptr<ExternalCommandRecording> borrowed;
        if(run==1&&reuse){probe.appendTrackedStates(states);features.appendTrackedStates(states);compose.appendTrackedStates(states);
            for(auto& node:graph->publicNodes())node->runtime()->appendTrackedStates(states);for(auto& scratch:context->tensorScratch)states.push_back({scratch.second->resource.Get(),&scratch.second->state});
            for(auto s:states)before.push_back(*s.state);
            check(context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&cacheAllocator)),"Cache allocator");
            check(context->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,cacheAllocator.Get(),nullptr,IID_PPV_ARGS(&cache)),"Cache list");
            borrowed=std::make_unique<ExternalCommandRecording>(context,cache.Get());}
        context->cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        features.record();
        if(GetEnvironmentVariableA("NR_BATCH_ISOLATE",nullptr,0)){probe.submit();for(auto& node:graph->publicNodes()){
            fprintf(stderr,"NODE %s\n",node->name.c_str());node->record();probe.submit();}}
        else graph->record();
        compose.record();context->cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        context->cmd->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,ticks.Get(),0);
        if(borrowed){for(size_t i=0;i<states.size();++i){auto& s=states[i];if(*s.state!=before[i])transition(context->cmd.Get(),s.resource,*s.state,before[i]);*s.state=before[i];}
            check(cache->Close(),"Cache close");borrowed.reset();}
        auto recorded=std::chrono::steady_clock::now();if(run==1&&reuse)probe.executeClosed(cache.Get());else probe.submit();UINT64 frequency;check(context->queue->GetTimestampFrequency(&frequency),"Batch clock");
        void* p;D3D12_RANGE range{0,16},empty{0,0};check(ticks->Map(0,&range,&p),"Batch ticks");auto t=static_cast<UINT64*>(p);double gpu=double(t[1]-t[0])*1000/frequency;ticks->Unmap(0,&empty);
        printf("RUN %u B %u CPU %.6f GPU %.6f BUFFER_BYTES %llu\n",run,batch,std::chrono::duration<double,std::milli>(recorded-start).count(),gpu,context->defaultBufferBytes);
        auto output=probe.downloadLanes(compose.resultTensor());for(float f:output)if(!std::isfinite(f))throw std::runtime_error("Nonfinite batch output");
        std::ofstream file(std::string(argv[2])+"/rgb"+std::to_string(run)+".bin",std::ios::binary);file.write(reinterpret_cast<const char*>(output.data()),std::streamsize(output.size()*4));if(!file)throw std::runtime_error("Batch result write failed");
    }
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
