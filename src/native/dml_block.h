#pragma once
// Complete block 31 on D3D12. Standalone harness, 1..640 tokens.
#include "dml_runtime.h"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <fstream>
#include <memory>
#include <cstdlib>

struct BlockBinding {
    size_t expand,ffn,qkv,projection;
    Tensor *source,*cosine,*scale,*attentionCosine;
};
class BlockHarness : public NativeRuntime {
    ComPtr<ID3D12RootSignature> shaderRoot;
    ComPtr<ID3D12PipelineState> gate, residual, normalize, packHeads, softmaxWeights, mergeContext, scalar, publishState;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> timestampReadback;
    UINT tokens = 64;
    UINT queryOffset=0;
    bool scalarAttention=false;
    bool chain=false,captureChainStates=false,profileRecording=true;
    UINT queryCount=14;
    Tensor* chainValue=nullptr;
    ComPtr<ID3D12Resource> chainSnapshots;
    std::vector<BlockBinding> blocks;
public:
    Tensor *input, *hidden, *branch, *feed, *qkv, *ready, *context, *projected, *output;
    Tensor *ffnCos, *scale, *attnCos;
    std::vector<double> lastStageMs = std::vector<double>(13);
    UINT tokenCount() const { return tokens; }
    bool usesScalarAttention() const { return scalarAttention; }
    UINT profilingQueryCount() const {return queryCount;}
    Tensor* resultTensor() const {return chain ? chainValue : output;}
#ifdef NR_EXPERIMENTAL_LINALG
    Tensor* attentionUnitInput() const{return qkv;}
    Tensor* attentionUnitResult() const{return context;}
    Tensor* attentionUnitScores() const{return scores;}
    Tensor* attentionUnitProbabilities() const{return probabilities;}
    Tensor* attentionUnitPacked() const{return packed;}
    Tensor* attentionUnitAttended() const{return attended;}
    void recordAttentionUnit(){
        auto& b=blocks[0];
        {NativeProfileRange r(NativeRuntime::context->profiler.get(),cmd.Get(),"NormalizeQkv");shader(normalize.Get(),qkv,b.source,b.scale,ready,tokens*32,0,32);}
        {NativeProfileRange r(NativeRuntime::context->profiler.get(),cmd.Get(),"PackHeads");shader(packHeads.Get(),ready,b.source,b.scale,packed,static_cast<UINT>(packed->elements),0,128);}
        gemm(4);
        {NativeProfileRange r(NativeRuntime::context->profiler.get(),cmd.Get(),"SoftmaxWeights");shader(softmaxWeights.Get(),scores,b.source,b.scale,probabilities,tokens*32*64,0,64);}
        gemm(5);
        {NativeProfileRange r(NativeRuntime::context->profiler.get(),cmd.Get(),"MergeContext");shader(mergeContext.Get(),attended,b.source,probabilities,context,tokens*1024,0,128);}
    }
#endif
    explicit BlockHarness(const std::string& folder,bool chainMode=false,std::shared_ptr<NativeContext> existing=nullptr,Tensor* residentInput=nullptr) : NativeRuntime(existing),chain(chainMode) {
        if(residentInput){
            if(residentInput->elements%1024 || !residentInput->elements || residentInput->elements>640*1024)throw std::runtime_error("Invalid resident bottleneck size");
            tokens=static_cast<UINT>(residentInput->elements/1024);
        }else{
            std::ifstream inputFile(nativePath(folder+"/input.bin"),std::ios::binary|std::ios::ate);
            if(!inputFile)throw std::runtime_error("Input fixture not found");
            auto inputBytes=static_cast<std::streamoff>(inputFile.tellg());
            if(inputBytes<4096 || inputBytes%4096 || inputBytes>640*4096)throw std::runtime_error("Invalid token count");
            tokens=static_cast<UINT>(inputBytes/4096);
        }
        char setting[4]{};
        scalarAttention=GetEnvironmentVariableA("NR_ATTENTION_SCALAR",setting,sizeof(setting))==1 && setting[0]=='1';
        if(NativeRuntime::context->acceptedGameGlobalPolicy)scalarAttention=false;
        input=residentInput ? residentInput : load(folder,"input",tokens*1024);
        const std::string firstFolder=chain ? folder+"/block31" : folder;
        auto expand=loadWeight(firstFolder,"expand",1024*4096);
        auto ffnProjection=loadWeight(firstFolder,"ffn_projection",4096*1024);
        ffnCos=load(firstFolder,"ffn_cosine",1024);
        auto qkvWeight=loadWeight(firstFolder,"qkv",1024*3072);
        scale=load(firstFolder,"scale",32);
        auto projection=loadWeight(firstFolder,"projection",1024*1024);
        attnCos=load(firstFolder,"attn_cosine",1024);
        hidden=make(tokens*4096); gatedHidden=make(tokens*4096); branch=make(tokens*1024); feed=make(tokens*1024);
        qkv=make(tokens*3072); ready=make(tokens*3072); context=make(tokens*1024);
        projected=make(tokens*1024); output=make(tokens*1024);
        addGemm(input,expand,hidden,tokens,4096,1024);
        // The gate writes a separate published buffer, read by the next GEMM.
        addGemm(gatedHidden,ffnProjection,branch,tokens,1024,4096);
        addGemm(feed,qkvWeight,qkv,tokens,3072,1024);
        addGemm(context,projection,projected,tokens,1024,1024);
        blocks.push_back({0,1,2,3,input,ffnCos,scale,attnCos});
        UINT padded=(tokens+63)/64*64;
        packed=make(UINT64(tokens)*1024+UINT64(padded)*2048);
        scores=make(UINT64(32)*tokens*padded);
        probabilities=make(scores->elements+UINT64(32)*tokens);
        attended=make(UINT64(tokens)*1024);
        addGemm(packed,packed,scores,tokens,padded,32,32,true,0,UINT64(tokens)*1024*4);
        addGemm(probabilities,packed,attended,tokens,32,padded,32,false,0,(UINT64(tokens)*1024+UINT64(padded)*1024)*4);
        if(chain){
            chainValue=make(UINT64(tokens)*1024);
            for(UINT index=32;index<=38;++index){
                std::string path=folder+"/block"+std::to_string(index);
                auto e=loadWeight(path,"expand",1024*4096),f=loadWeight(path,"ffn_projection",4096*1024);
                auto fc=load(path,"ffn_cosine",1024),qkvW=loadWeight(path,"qkv",1024*3072);
                auto sc=load(path,"scale",32),p=loadWeight(path,"projection",1024*1024),ac=load(path,"attn_cosine",1024);
                size_t base=jobs.size();
                addGemm(chainValue,e,hidden,tokens,4096,1024);addGemm(gatedHidden,f,branch,tokens,1024,4096);
                addGemm(feed,qkvW,qkv,tokens,3072,1024);addGemm(context,p,projected,tokens,1024,1024);
                blocks.push_back({base,base+1,base+2,base+3,chainValue,fc,sc,ac});
            }
            queryCount=15*8;
            chainSnapshots=buffer(device.Get(),UINT64(tokens)*1024*4*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        }
#ifdef NR_GLOBAL_INT8
        if(NativeRuntime::context->globalInt8Segment){
            if(NativeRuntime::context->globalInt8Policy!="both")throw std::runtime_error("Compact FFN needs both INT8 jobs");
            if(!NativeRuntime::context->globalInt8GatePack)NativeRuntime::context->globalInt8GatePack=linAlgPso(device.Get(),NativeRuntime::context->globalInt8Root.Get(),"shape-GatePack.dxil");
            for(auto& block:blocks){
                if(!jobs[block.expand]->int8||!jobs[block.ffn]->int8)throw std::runtime_error("Incomplete compact FFN pair");
                jobs[block.expand]->int8->compactOutput=true;jobs[block.ffn]->int8->preparedInput=true;++NativeRuntime::context->globalInt8Segments;
            }
        }
#endif
        createShaders();
        D3D12_QUERY_HEAP_DESC query{}; query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count=queryCount*12;
        check(device->CreateQueryHeap(&query,IID_PPV_ARGS(&queries)),"Timestamp queries");
        timestampReadback=buffer(device.Get(),UINT64(queryCount)*8*12,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        submit(); uploads.clear();
    }
    void createShaders() {
        D3D12_ROOT_PARAMETER params[6]{};
        for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[4].Constants.Num32BitValues=4;
        D3D12_ROOT_SIGNATURE_DESC desc{};batchRoot(params[5]);desc.NumParameters=6;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Shader root signature");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&shaderRoot)),"Shader root");
        auto compile=[&](const char* entry,ComPtr<ID3D12PipelineState>& pipeline){
            blob=compileBatchedShader("block_ops.hlsl",entry);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=shaderRoot.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),entry);
        };
        compile("Gate",gate);compile("Residual",residual);compile("NormalizeQkv",normalize);
        compile("PackHeads",packHeads);compile("SoftmaxWeights",softmaxWeights);compile("MergeContext",mergeContext);
        if(scalarAttention)compile("Attention",scalar);
        if(chain)compile("PublishState",publishState);
    }
    void gemm(size_t index){recordGemm(*jobs[index]);
        if(NativeRuntime::context->tileBatch>1&&GetEnvironmentVariableA("NR_BATCH_ISOLATE",nullptr,0)){
            fprintf(stderr,"BOTTLENECK GEMM %zu\n",index);submit();}}
    void shader(ID3D12PipelineState* pipeline,Tensor* source,Tensor* skip,Tensor* parameter,Tensor* target,UINT count,UINT publish,UINT threads){
        const char* name=pipeline==gate.Get()?"BottleneckGate":pipeline==residual.Get()?"BottleneckResidual":pipeline==normalize.Get()?"BottleneckNormalize":pipeline==packHeads.Get()?"BottleneckPackHeads":pipeline==softmaxWeights.Get()?"BottleneckSoftmax":pipeline==mergeContext.Get()?"BottleneckMerge":pipeline==publishState.Get()?"BottleneckPublish":"BottleneckShader";
        NativeProfileRange range(NativeRuntime::context->profiler.get(),cmd.Get(),name);
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);skip->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);parameter->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(shaderRoot.Get());cmd->SetPipelineState(pipeline);
        cmd->SetComputeRootShaderResourceView(0,source->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,skip->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(2,parameter->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3,target->resource->GetGPUVirtualAddress());UINT constants[]={tokens,1024,count,publish};cmd->SetComputeRoot32BitConstants(4,4,constants,0);
        bindBatch(5,{source,skip,parameter,target});cmd->Dispatch((count+threads-1)/threads,1,NativeRuntime::context->tileBatch);uavBarrier(cmd.Get());
#ifdef NR_GRAPH_DIAGNOSTICS
        if(NativeRuntime::context->capture)NativeRuntime::context->capture(NativeRuntime::context->diagnosticNode+"_shader"+std::to_string(NativeRuntime::context->diagnosticShader++),target);
#endif
        if(NativeRuntime::context->tileBatch>1&&GetEnvironmentVariableA("NR_BATCH_ISOLATE",nullptr,0)){
            fprintf(stderr,"BOTTLENECK SHADER count %u threads %u\n",count,threads);submit();}
    }
    void stamp(UINT index){if(!profileRecording)return;cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,queryOffset+index);}
    void recordBlock(const BlockBinding& b){
        gemm(b.expand);stamp(1);
#ifdef NR_GLOBAL_INT8
        if(NativeRuntime::context->globalInt8Segment)recordInt8GatePack(*jobs[b.expand],*jobs[b.ffn]);else
#endif
        shader(gate.Get(),hidden,b.source,b.scale,gatedHidden,tokens*4096,0,128);stamp(2);
        gemm(b.ffn);stamp(3);
        shader(residual.Get(),branch,b.source,b.cosine,feed,tokens*1024,1,128);stamp(4);
        gemm(b.qkv);stamp(5);
        shader(normalize.Get(),qkv,b.source,b.scale,ready,tokens*32,0,32);stamp(6);
        if(scalarAttention){
            shader(scalar.Get(),ready,b.source,b.scale,context,tokens*32,0,32);
            for(UINT i=7;i<=11;++i)stamp(i);
        }else{
            shader(packHeads.Get(),ready,b.source,b.scale,packed,static_cast<UINT>(packed->elements),0,128);stamp(7);
            gemm(4);stamp(8);
            shader(softmaxWeights.Get(),scores,b.source,b.scale,probabilities,tokens*32*64,0,64);stamp(9);
            gemm(5);stamp(10);
            shader(mergeContext.Get(),attended,b.source,probabilities,context,tokens*1024,0,128);stamp(11);
        }
        gemm(b.projection);stamp(12);
        shader(residual.Get(),projected,feed,b.attentionCosine,output,tokens*1024,0,128);stamp(13);
    }
    void recordResident(){profileRecording=false;record();profileRecording=true;}
    void record(){
        if(!chain){recordBlock(blocks[0]);return;}
        UINT base=queryOffset;
        for(UINT index=0;index<8;++index){
            queryOffset=base+index*15;stamp(0);recordBlock(blocks[index]);
            shader(publishState.Get(),output,input,blocks[index].scale,chainValue,tokens*1024,0,128);stamp(14);
            if(captureChainStates){
                chainValue->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
                const UINT64 bytes=UINT64(tokens)*1024*4;
                cmd->CopyBufferRegion(chainSnapshots.Get(),bytes*index,chainValue->resource.Get(),0,bytes);
            }
        }
        queryOffset=base;
    }
    Tensor* gatedHidden=nullptr;
    Tensor *packed=nullptr,*scores=nullptr,*probabilities=nullptr,*attended=nullptr;
    double run(){if(!chain)stamp(0);record();
        cmd->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,queryCount,timestampReadback.Get(),0);submit();
        UINT64 frequency;check(queue->GetTimestampFrequency(&frequency),"GPU clock");void* p;D3D12_RANGE range{0,static_cast<SIZE_T>(queryCount)*8};check(timestampReadback->Map(0,&range,&p),"Read block timing");
        auto ticks=static_cast<UINT64*>(p);double ms=double(ticks[queryCount-1]-ticks[0])*1000/frequency;
        std::fill(lastStageMs.begin(),lastStageMs.end(),0);
        for(size_t block=0;block<(chain ? 8u : 1u);++block)
            for(size_t i=0;i<13;++i)lastStageMs[i]+=double(ticks[block*15+i+1]-ticks[block*15+i])*1000/frequency;
        D3D12_RANGE empty{0,0};timestampReadback->Unmap(0,&empty);return ms;}
    std::vector<double> measure(std::vector<double>& stageTimes){
        if(chain)throw std::runtime_error("Batch12 mode is not supported for the chain");
        constexpr UINT frames=12;
        const UINT64 bytes=output->elements*4;
        auto snapshots=buffer(device.Get(),bytes*frames,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        for(UINT frame=0;frame<frames;++frame){queryOffset=frame*14;stamp(0);record();
            output->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(snapshots.Get(),bytes*frame,output->resource.Get(),0,bytes);
        }
        cmd->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,frames*14,timestampReadback.Get(),0);submit();queryOffset=0;
        UINT64 frequency;check(queue->GetTimestampFrequency(&frequency),"GPU clock");void* mapped;
        D3D12_RANGE range{0,112*frames},empty{0,0};check(timestampReadback->Map(0,&range,&mapped),"Read batch timings");
        auto ticks=static_cast<UINT64*>(mapped);std::vector<double> result;stageTimes.assign(13,0);
        for(UINT frame=0;frame<frames;++frame){UINT offset=frame*14;
            result.push_back(double(ticks[offset+13]-ticks[offset])*1000/frequency);
            for(UINT i=0;i<13;++i)stageTimes[i]+=double(ticks[offset+i+1]-ticks[offset+i])*1000/frequency;
        }
        timestampReadback->Unmap(0,&empty);range={0,static_cast<SIZE_T>(bytes*frames)};
        check(snapshots->Map(0,&range,&mapped),"Read repeated outputs");
        bool equal=true;for(UINT frame=1;frame<frames;++frame)
            equal=equal && memcmp(mapped,static_cast<char*>(mapped)+bytes*frame,static_cast<size_t>(bytes))==0;
        snapshots->Unmap(0,&empty);if(!equal)throw std::runtime_error("Native batch output is not repeatable");return result;
    }
    void saveChainStates(const std::string& folder){
        captureChainStates=true;run();captureChainStates=false;
        const UINT64 bytes=UINT64(tokens)*1024*4;void* mapped;D3D12_RANGE range{0,static_cast<SIZE_T>(bytes*8)},empty{0,0};
        check(chainSnapshots->Map(0,&range,&mapped),"Read diagnostic chain states");
        bool valid=true;
        for(UINT i=0;i<8;++i){std::ofstream file(folder+"/native_block"+std::to_string(31+i)+".bin",std::ios::binary);
            file.write(static_cast<char*>(mapped)+bytes*i,static_cast<std::streamsize>(bytes));valid=valid && bool(file);}
        chainSnapshots->Unmap(0,&empty);if(!valid)throw std::runtime_error("Cannot save chain snapshots");
    }
};
