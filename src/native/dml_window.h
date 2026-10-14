#pragma once
#include "dml_runtime.h"
#include <d3dcompiler.h>
#include <map>

class WindowHarness : public NativeRuntime {
#ifdef NR_CLEAN_PROFILE
    std::map<std::string,ComPtr<ID3D12PipelineState>> denseProducerShaders;
    bool denseShadowEnabled(){return GetEnvironmentVariableA("NR_STUDY_FP16_DENSE_DIR",nullptr,0)!=0;}
    Tensor* denseView(const char* role,UINT64 count,UINT channels,UINT rows){
        if(!denseShadowEnabled()||channels<128||rows<128)return makeScratch(role,count);
        if(NativeRuntime::context->tileBatch!=1||count%2)throw std::runtime_error("Dense FP16 view extent");
        auto t=makeScratch((std::string(role)+".denseHalf").c_str(),count+count/2);
        t->elements=count;t->denseHalfShadow=true;return t;
    }
#else
    Tensor* denseView(const char* role,UINT64 count,UINT,UINT){return makeScratch(role,count);}
#endif
#ifdef NR_C32_COMPACT
    bool compactC32=false;
#endif
#ifdef NR_BRANCH_COMPACT
    bool compactBranch=false;
#ifdef NR_CLEAN_PROFILE
    bool studyC32Int8=false;
    ComPtr<ID3D12PipelineState> studyC32Publish,studyC32WeightBytes;
#endif
    Tensor *compactInput=nullptr,*compactGate=nullptr,*compactExpand=nullptr,*compactProjection=nullptr;
    void branchDispatch(ID3D12PipelineState* pipeline,Tensor* source,Tensor* target,UINT groups,UINT x,UINT y){
        Tensor* expandView=target==compactExpand?source:compactExpand;Tensor* projectView=target==compactProjection?source:compactProjection;
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);expandView->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);projectView->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);headsOutput->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto& ctx=*NativeRuntime::context;cmd->SetComputeRootSignature(ctx.branchCompactRoot.Get());cmd->SetPipelineState(pipeline);
        cmd->SetComputeRootShaderResourceView(0,source->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,expandView->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(2,projectView->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3,target->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(4,headsOutput->resource->GetGPUVirtualAddress());
        UINT constants[]={shape[0]*shape[1],shape[8]/32,groups,0};cmd->SetComputeRoot32BitConstants(5,4,constants,0);cmd->Dispatch(x,y,1);uavBarrier(cmd.Get());
    }
    void initBranchRoot(){
        auto& ctx=*NativeRuntime::context;
        if(!ctx.branchCompactRoot){
            linAlgCheckWaveFP16(device.Get());D3D12_ROOT_PARAMETER p[6]{};
            for(UINT i=0;i<5;++i){p[i].ParameterType=i<3?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV;p[i].Descriptor.ShaderRegister=i<3?i:i-3;}
            p[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[5].Constants.Num32BitValues=4;
            D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=6;desc.pParameters=p;ComPtr<ID3DBlob> blob,error;
            check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Compact branch root serialize");
            check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&ctx.branchCompactRoot)),"Compact branch root");
            ctx.branchWeightPack=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"branch-PackWeights.dxil");ctx.branchPublishPack=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"branch-PublishPack.dxil");
            ctx.branchExpandGate=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),
#ifdef NR_CLEAN_PROFILE
                GetEnvironmentVariableA("NR_STUDY_BRANCH_SEGMENT_DIR",nullptr,0)?"branch-segment-pair.dxil":
#endif
                "branch-ExpandGate.dxil");ctx.branchProjectSum=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"branch-ProjectSum.dxil");
        }
    }
    void initCompactBranch(Tensor* expandWeight,Tensor* projectionWeight){
        auto& ctx=*NativeRuntime::context;UINT heads=shape[8]/32,pixels=shape[0]*shape[1];
        if(ctx.tileBatch!=1||pixels%8||heads<2||heads>8)throw std::runtime_error("Compact branch shape mismatch");
        initBranchRoot();
        compactInput=makeScratch("branch.compactInput",UINT64(heads)*pixels*16);compactGate=makeScratch("branch.compactGate",UINT64(heads)*4*pixels*16);
        compactExpand=make(UINT64(heads)*4*heads*512,false);compactProjection=make(UINT64(heads)*4*512,false);
        auto pack=[&](Tensor* source,Tensor* target,UINT groups){branchDispatch(ctx.branchWeightPack.Get(),source,target,groups,(groups*512+127)/128,1);target->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);};
        // Both tensors exist before binding; setup-only conversion shares the same root.
        pack(expandWeight,compactExpand,heads*4*heads);pack(projectionWeight,compactProjection,heads*4);
        ++ctx.branchCompactWindows;
#ifdef NR_CLEAN_PROFILE
        if(GetEnvironmentVariableA("NR_STUDY_BRANCH_SEGMENT_DIR",nullptr,0))printf("NR_BRANCH_SEGMENT_PREPARED channels=%u pixels=%u\n",heads*32,pixels);
#endif
    }
    void recordCompactBranch(){
        auto& ctx=*NativeRuntime::context;UINT pixels=shape[0]*shape[1],heads=shape[8]/32,groups=(heads*pixels*16+127)/128;
#ifdef NR_CLEAN_PROFILE
        if(GetEnvironmentVariableA("NR_STUDY_BRANCH_SEGMENT_DIR",nullptr,0)){
            {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"BranchSegmentPublishExpandGate");branchDispatch(ctx.branchExpandGate.Get(),input,compactGate,0,pixels/8,heads*2);}
            {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"BranchCompactProjectSum");branchDispatch(ctx.branchProjectSum.Get(),compactGate,compactInput,0,pixels/8,heads*2);}
            return;
        }
#endif
        {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"BranchCompactPublishPack");branchDispatch(ctx.branchPublishPack.Get(),input,compactInput,0,std::min(groups,4096u),(groups+4095)/4096);}
        {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"BranchCompactExpandGate");branchDispatch(ctx.branchExpandGate.Get(),compactInput,compactGate,0,pixels/8,heads*8);}
        {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"BranchCompactProjectSum");branchDispatch(ctx.branchProjectSum.Get(),compactGate,compactInput,0,pixels/8,heads*2);}
    }
#ifdef NR_C32_COMPACT
    void initCompactC32(Tensor* expandWeight,Tensor* projectionWeight){
        auto& ctx=*NativeRuntime::context;UINT pixels=shape[0]*shape[1];
#ifdef NR_CLEAN_PROFILE
        studyC32Int8=GetEnvironmentVariableA("NR_STUDY_C32_INT8",nullptr,0)!=0;
        if(studyC32Int8&&GetEnvironmentVariableA("NR_STUDY_FFN",nullptr,0))throw std::runtime_error("C32 INT8 cannot mix with full FFN study");
        if(studyC32Int8&&!fuseC32Feed)throw std::runtime_error("C32 INT8 requires accepted fused window feed");
#endif
        if(ctx.tileBatch!=1||pixels%8||shape[8]!=32)throw std::runtime_error("Compact C32 geometry");
        initBranchRoot();
        if(!ctx.c32WeightPack){
#ifdef NR_CLEAN_PROFILE
            if(studyC32Int8){
                ctx.c32WeightPack=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"c32i8-WeightScale.dxil");
                ctx.c32ExpandGate=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"c32i8-Expand.dxil");
            }else
#endif
            {
            ctx.c32WeightPack=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"c32-PackWeights.dxil");
            ctx.c32ExpandGate=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"c32-ExpandGate.dxil");
            ctx.c32Project=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),ctx.c32CompactPolicy=="compact-partials"?"c32-ProjectPartials.dxil":"c32-ProjectF16.dxil");
            }
        }
#ifdef NR_CLEAN_PROFILE
        if(studyC32Int8){
            studyC32Publish=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"c32i8-Publish.dxil");
            studyC32WeightBytes=linAlgPso(device.Get(),ctx.branchCompactRoot.Get(),"c32i8-WeightBytes.dxil");
        }
#endif
#ifdef NR_C32_FULL_FFN
        if(fullC32Ffn){compactInput=compactGate=nullptr;}else
#endif
        {compactInput=makeScratch("c32.compactInput",UINT64(pixels)*16);compactGate=makeScratch("c32.compactGate",UINT64(pixels)*64);}
        compactExpand=make(2048,false);compactProjection=make(2048,false);headsOutput=branch;
        branchDispatch(ctx.c32WeightPack.Get(),expandWeight,compactExpand,128,16,1);
        branchDispatch(ctx.c32WeightPack.Get(),projectionWeight,compactProjection,32,16,1);
#ifdef NR_CLEAN_PROFILE
        if(studyC32Int8){
            branchDispatch(studyC32WeightBytes.Get(),expandWeight,compactExpand,128,8,1);
            branchDispatch(studyC32WeightBytes.Get(),projectionWeight,compactProjection,32,8,1);
            printf("NR_STUDY_C32_INT8_SEGMENT pixels=%u\n",pixels);
        }
#endif
        ++ctx.c32CompactWindows;
    }
    void recordCompactC32(){
#ifdef NR_C32_FULL_FFN
        if(fullC32Ffn){recordC32Feed();return;}
#endif
        auto& ctx=*NativeRuntime::context;UINT pixels=shape[0]*shape[1],words=(pixels*16+127)/128,rows=pixels/8;
#ifdef NR_CLEAN_PROFILE
        if(studyC32Int8){
            {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"C32INT8Publish");branchDispatch(studyC32Publish.Get(),input,compactInput,0,std::min(rows,4096u),(rows+4095)/4096);}
            {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"C32INT8ExpandGate");branchDispatch(ctx.c32ExpandGate.Get(),compactInput,compactGate,0,std::min(rows,4096u),((rows+4095)/4096)*4);}
            recordC32Feed();return;
        }
#endif
        {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"C32CompactPublishPack");branchDispatch(ctx.branchPublishPack.Get(),input,compactInput,0,std::min(words,4096u),(words+4095)/4096);}
        {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"C32CompactExpandGate");branchDispatch(ctx.c32ExpandGate.Get(),compactInput,compactGate,0,std::min(rows,4096u),((rows+4095)/4096)*8);}
#ifdef NR_WINDOW_SEGMENT
        if(fuseC32Feed){recordC32Feed();return;}
#endif
        {NativeProfileRange p(ctx.profiler.get(),cmd.Get(),"C32CompactProject");branchDispatch(ctx.c32Project.Get(),compactGate,compactInput,0,std::min(rows,4096u),((rows+4095)/4096)*2);}
    }
#endif
#endif
#ifdef NR_WINDOW_SEGMENT
    void initC32Feed(){
        auto& ctx=*NativeRuntime::context;
        if(!ctx.windowC32Root){
            D3D12_ROOT_PARAMETER p[8]{};
            for(UINT i=0;i<3;++i){p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[i].Descriptor.ShaderRegister=i;}
            for(UINT i=3;i<5;++i){p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[i].Descriptor.ShaderRegister=i-3;}
            p[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[5].Constants.Num32BitValues=4;
            p[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[6].Descriptor.ShaderRegister=3;
            p[7].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[7].Constants.ShaderRegister=1;p[7].Constants.Num32BitValues=5;
            D3D12_ROOT_SIGNATURE_DESC d{};d.NumParameters=8;d.pParameters=p;ComPtr<ID3DBlob> blob,error;
            check(D3D12SerializeRootSignature(&d,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"C32 feed root serialize");
            check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&ctx.windowC32Root)),"C32 feed root");
            ctx.windowC32Project=linAlgPso(device.Get(),ctx.windowC32Root.Get(),
#ifdef NR_CLEAN_PROFILE
                studyC32Int8?"c32i8-ProjectFeed.dxil":
#endif
#ifdef NR_C32_FULL_FFN
                fullC32Ffn?"c32-FullFFN.dxil":
#endif
                "c32-ProjectFeed.dxil");
        }
    }
    void recordC32Feed(){
        auto& ctx=*NativeRuntime::context;
        if(!ctx.windowC32Root)throw std::runtime_error("C32 feed pipeline must be initialized before recording");
        const char* label="C32CompactProjectFeedPack";Tensor* first=compactGate;
#ifdef NR_C32_FULL_FFN
        if(fullC32Ffn){label="C32FullFFNFeedPack";first=compactExpand;}
#endif
        NativeProfileRange profile(ctx.profiler.get(),cmd.Get(),label);
        first->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        input->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);ffnCos->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compactProjection->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        feed->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);windows->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(ctx.windowC32Root.Get());cmd->SetPipelineState(ctx.windowC32Project.Get());
        cmd->SetComputeRootShaderResourceView(0,first->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(1,input->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(2,compactProjection->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3,feed->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(4,windows->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(6,ffnCos->resource->GetGPUVirtualAddress());
        UINT settings[]={shape[0]*shape[1],1,0,shape[4]*shape[5]},geometry[]={shape[1],shape[0],shape[2],shape[3],shape[5]};
        cmd->SetComputeRoot32BitConstants(5,4,settings,0);cmd->SetComputeRoot32BitConstants(7,5,geometry,0);
        UINT rows=shape[4]*shape[5]/8,factor=2;
#ifdef NR_C32_FULL_FFN
        if(fullC32Ffn)factor=1;
#ifdef NR_CLEAN_PROFILE
        if(fullC32Ffn&&GetEnvironmentVariableA("NR_STUDY_FFN",nullptr,0))rows=shape[4]*shape[5]/16;
#endif
#endif
        cmd->Dispatch(std::min(rows,4096u),((rows+4095)/4096)*factor,1);uavBarrier(cmd.Get());
    }
#endif
    UINT shape[10]{};
    UINT attentionBase=2;
    Tensor *branchInput=nullptr,*expanded=nullptr,*branchHeads=nullptr,*headsOutput=nullptr;
    Tensor* splitFirst=nullptr;
    Tensor* retainedSkip=nullptr;
    Tensor *input,*published,*hidden,*gated,*branch,*feed,*windows,*qkv,*packed,*scores,*probabilities,*attended,*context,*projected,*output;
    Tensor *ffnCos,*scale,*bias,*attnCos;
    ComPtr<ID3D12RootSignature> root;
    std::map<std::string,ComPtr<ID3D12PipelineState>> shaders;
    bool isolate=false;
    bool compensatedProjection=false;
    bool parallelNormalize=false,nativeHalf=false;
    bool fusedGate=false;
    bool fusedFeedPack=false;
#ifdef NR_WINDOW_SEGMENT
    bool reuseWindowFeed=false,fuseC32Feed=false;
#ifdef NR_C32_FULL_FFN
    bool fullC32Ffn=false;
#endif
#endif
    Tensor* splitProjectionWeight=nullptr;
    void compile(const char* entry){D3D_SHADER_MACRO macros[]={{"NR_WINDOW_PARALLEL_NORM",parallelNormalize?"1":"0"},{"NR_NATIVE_HALF",nativeHalf?"1":"0"},
#ifdef NR_WINDOW_SEGMENT
        {"NR_WINDOW_REUSE_FEED",reuseWindowFeed?"1":"0"},
#endif
        {nullptr,nullptr}};
#ifdef NR_CLEAN_PROFILE
        const std::string denseEntry=entry;
        bool useDense=(denseEntry=="WindowPublish"&&published&&published->denseHalfShadow)||
            (denseEntry=="WindowFeedPack"&&windows->denseHalfShadow)||(denseEntry=="SplitMergeOutput"&&headsOutput&&headsOutput->denseHalfShadow);
        if(useDense)denseProducerShaders[entry]=linAlgPso(device.Get(),root.Get(),("fp16dense-"+denseEntry+".dxil").c_str());
#endif
        auto blob=compileBatchedShader("window_ops.hlsl",entry,macros);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();desc.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&shaders[entry])),entry);}
    void dispatch(const char* entry,Tensor* a,Tensor* b,Tensor* parameter,Tensor* target,UINT64 count,Tensor* extra=nullptr){
        NativeProfileRange profile(NativeRuntime::context->profiler.get(),cmd.Get(),entry);
        a->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        parameter->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(extra)extra->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(shaders.at(entry).Get());
#ifdef NR_CLEAN_PROFILE
        auto denseProducer=denseProducerShaders.find(entry);
        if(target->denseHalfShadow&&denseProducer!=denseProducerShaders.end()){
            if(target->lanes!=1||target->resource->GetDesc().Width<target->elements*6)throw std::runtime_error("Dense producer destination capacity mismatch");
            cmd->SetPipelineState(denseProducer->second.Get());
        }
#endif
        cmd->SetComputeRootShaderResourceView(0,a->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,b->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(2,parameter->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(3,target->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(5,(extra?extra:target)->resource->GetGPUVirtualAddress());
        if(target->elements>~UINT(0))throw std::runtime_error("Shader buffer exceeds addressable element count");
        shape[9]=static_cast<UINT>(target->elements);
        cmd->SetComputeRoot32BitConstants(4,10,shape,0);UINT groups=static_cast<UINT>((count+127)/128);
        bindBatch(6,{a,b,parameter,target,extra?extra:target});cmd->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,NativeRuntime::context->tileBatch);uavBarrier(cmd.Get());
#ifdef NR_GRAPH_DIAGNOSTICS
        if(NativeRuntime::context->capture)NativeRuntime::context->capture(NativeRuntime::context->diagnosticNode+"_shader"+std::to_string(NativeRuntime::context->diagnosticShader++)+"_"+entry,target);
#endif
        if(isolate){fprintf(stderr,"Diagnostic submit: %s\n",entry);submit();}}
    void gemm(size_t index){recordGemm(*jobs[index]);
        if(isolate){fprintf(stderr,"Diagnostic submit: GEMM %zu\n",index);submit();}}
#ifdef NR_EXPERIMENTAL_LINALG
    void fusedAttention(){
        NativeProfileRange profile(NativeRuntime::context->profiler.get(),cmd.Get(),"WindowFusedAttentionFP16");
        auto& ctx=*NativeRuntime::context;
        auto source=ctx.linAlgWindowNormalizeFused?qkv:packed;
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);bias->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        scale->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(ctx.linAlgFusedWindowRoot.Get());cmd->SetPipelineState(ctx.linAlgFusedWindowPipeline.Get());
        cmd->SetComputeRootShaderResourceView(0,source->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,bias->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(4,scale->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2,context->resource->GetGPUVirtualAddress());
        UINT heads=shape[8]/32,groups=shape[4]*shape[5]*heads/64;
        UINT constants[]={groups,heads,UINT(source->elements),UINT(context->elements)};cmd->SetComputeRoot32BitConstants(3,4,constants,0);
        cmd->Dispatch(groups,1,ctx.tileBatch);uavBarrier(cmd.Get());
    }
    void reserveFusedGemm(Tensor* a,Tensor* b,Tensor* out){auto job=std::make_unique<GemmJob>();job->a=a;job->b=b;job->out=out;job->fusedPlaceholder=true;jobs.push_back(std::move(job));}
#endif
public:
#ifdef NR_COMPACT_GEOMETRY
    // Only materialized FP32 views, sampled by the offline single-frame oracle.
    auto firstBlockStudyViews() const {
        if(shape[8]!=32)throw std::runtime_error("First-block study requires C32");
        return std::vector<std::pair<std::string,Tensor*>>{{"feed",feed},{"windows",windows},{"qkv",qkv},{"context",context},{"projected",projected}};
    }
#endif
#ifdef NR_EXPERIMENTAL_LINALG
    Tensor* attentionUnitInput() const{return qkv;}
    Tensor* attentionUnitResult() const{return context;}
    Tensor* attentionUnitScores() const{return scores;}
    Tensor* attentionUnitProbabilities() const{return probabilities;}
    Tensor* attentionUnitPacked() const{return packed;}
    Tensor* attentionUnitAttended() const{return attended;}
    void recordAttentionUnit(){
        if(NativeRuntime::context->linAlgWindowFused){if(!NativeRuntime::context->linAlgWindowNormalizeFused)dispatch("WindowNormalize",qkv,input,scale,packed,windows->elements);fusedAttention();return;}
        dispatch("WindowNormalize",qkv,input,scale,packed,windows->elements);gemm(attentionBase+1);
        dispatch("WindowSoftmax",scores,input,bias,probabilities,probabilities->elements);gemm(attentionBase+2);
        dispatch("WindowMergeContext",attended,input,scale,context,context->elements);
    }
#endif
    bool diagnosticIsolation() const{return isolate;}
    bool diagnosticCompensatedProjection() const{return compensatedProjection&&shape[8]==512;}
    UINT channels() const{return shape[8];}
    explicit WindowHarness(const std::string& folder,std::shared_ptr<NativeContext> existing=nullptr,Tensor* residentInput=nullptr,bool keepSkip=false) : NativeRuntime(existing){
        char mode[4]{};isolate=GetEnvironmentVariableA("NR_WINDOW_ISOLATE",mode,sizeof(mode))==1 && mode[0]=='1';
        char precision[4]{};compensatedProjection=GetEnvironmentVariableA("NR_SPLIT_COMPENSATED_PROJECTION",precision,sizeof(precision))==1 && precision[0]=='1';
        auto normSetting=GetEnvironmentVariableA("NR_WINDOW_PARALLEL_NORM",precision,sizeof(precision));
        parallelNormalize=normSetting==0||(normSetting==1&&precision[0]=='1');
        nativeHalf=GetEnvironmentVariableA("NR_WINDOW_NATIVE_HALF",precision,sizeof(precision))==1&&precision[0]=='1';
        auto gateSetting=GetEnvironmentVariableA("NR_WINDOW_FUSED_GATE",precision,sizeof(precision));
        fusedGate=gateSetting==0||(gateSetting==1&&precision[0]=='1');
        auto feedPackSetting=GetEnvironmentVariableA("NR_WINDOW_FUSED_FEED_PACK",precision,sizeof(precision));
        fusedFeedPack=feedPackSetting==0||(feedPackSetting==1&&precision[0]=='1');
        if(NativeRuntime::context->acceptedGameGlobalPolicy){isolate=false;compensatedProjection=false;nativeHalf=false;parallelNormalize=fusedGate=fusedFeedPack=true;}
        std::ifstream config(nativePath(folder+"/shape.txt"));for(UINT i=0;i<8;++i)if(!(config>>shape[i]))throw std::runtime_error("Invalid window shape");
        if(!(config>>shape[8]))shape[8]=32;
        UINT h=shape[0],w=shape[1],ph=shape[4],pw=shape[5],hiddenC=shape[7],channels=shape[8],heads=channels/32;
        if(!h || !w || h>1152 || w>1920 || shape[2]>7 || shape[3]>7 || shape[6]>1 || hiddenC!=128 ||
            (channels!=32 && channels!=64 && channels!=128 && channels!=256 && channels!=512) ||
            ph!=(h+shape[2]+7)/8*8 || pw!=(w+shape[3]+7)/8*8)throw std::runtime_error("Unsupported window geometry");
        UINT pixels=h*w,windowPixels=ph*pw,batch=windowPixels/64;
#ifdef NR_WINDOW_SEGMENT
        if(NativeRuntime::context->windowSegmentPolicy=="fused"){
            if(!fusedFeedPack||NativeRuntime::context->tileBatch!=1)throw std::runtime_error("Window segment requires fusedFeedPack B1");
            reuseWindowFeed=channels>32;
            fuseC32Feed=channels==32&&NativeRuntime::context->c32CompactPolicy=="compact-f16";
            if(channels==32&&!fuseC32Feed)throw std::runtime_error("Window segment requires compact-f16 C32");
            if(reuseWindowFeed)++NativeRuntime::context->windowFeedReused;
            if(fuseC32Feed)++NativeRuntime::context->windowC32Fused;
        }
#endif
#ifdef NR_C32_FULL_FFN
        if(channels==32&&NativeRuntime::context->c32FullFfnPolicy=="fused"){
            if(!fuseC32Feed)throw std::runtime_error("Full C32 FFN requires accepted fused window segment");
            fullC32Ffn=true;++NativeRuntime::context->c32FullFfnJobs;
        }
#endif
        input=residentInput ? residentInput : load(folder,"input",UINT64(pixels)*channels);
        if(input->elements!=UINT64(pixels)*channels)throw std::runtime_error("Resident input extent mismatch");
#ifdef NR_BRANCH_COMPACT
        compactBranch=NativeRuntime::context->branchCompact&&channels>32&&channels<512&&pixels%8==0;
#ifdef NR_C32_COMPACT
        compactC32=NativeRuntime::context->c32CompactPolicy!="baseline"&&channels==32;
        published=(compactBranch||compactC32)?nullptr:denseView("window.published",input->elements,channels,pixels);
#else
        published=compactBranch?nullptr:denseView("window.published",input->elements,channels,pixels);
#endif
#else
        published=denseView("window.published",input->elements,channels,pixels);
#endif
        ffnCos=load(folder,"ffn_cosine",channels);auto qkvWeight=loadWeight(folder,"qkv",UINT64(channels)*channels*3);
        scale=load(folder,"scale",heads);bias=load(folder,"bias",UINT64(heads)*64*64);
        auto projection=loadWeight(folder,"projection",UINT64(channels)*channels);attnCos=load(folder,"attn_cosine",channels);
#ifdef NR_C32_PROJECTION
        if(channels==32){qkvWeight->c32ProjectionRole=1;projection->c32ProjectionRole=2;}
#endif
#ifdef NR_WINDOW_SEGMENT
        feed=reuseWindowFeed?nullptr:makeScratch("window.feed",input->elements);
        branch=fuseC32Feed?feed:makeScratch("window.branch",input->elements);
#else
        branch=makeScratch("window.branch",input->elements);feed=makeScratch("window.feed",input->elements);
#endif
        if(channels==32){
            auto expand=loadWeight(folder,"expand",32*hiddenC),ffn=loadWeight(folder,"ffn_projection",hiddenC*32);
#ifdef NR_C32_COMPACT
            if(compactC32){hidden=gated=nullptr;initCompactC32(expand,ffn);reserveFusedGemm(input,expand,branch);reserveFusedGemm(input,ffn,branch);}else{
#endif
            hidden=makeScratch("window.hidden",UINT64(pixels)*hiddenC);gated=makeScratch("window.gated",hidden->elements);
            addGemm(published,expand,hidden,pixels,hiddenC,32);addGemm(gated,ffn,branch,pixels,32,hiddenC);
#ifdef NR_C32_COMPACT
            }
#endif
        }else if(channels==512){
            UINT groups=channels/64;
            auto first=loadWeight(folder,"first_projection",UINT64(channels)*channels);
            auto expand=loadWeight(folder,"expand",UINT64(groups)*64*256);
            auto contraction=loadWeight(folder,"group_projection",UINT64(groups)*256*64);
            splitProjectionWeight=contraction;
            auto outProjection=loadWeight(folder,"ffn_projection",UINT64(channels)*channels);
            hidden=makeScratch("window.hidden",input->elements);splitFirst=makeScratch("window.splitFirst",input->elements);branchInput=makeScratch("window.branchInput",input->elements);
            expanded=makeScratch("window.expanded",UINT64(groups)*pixels*256);gated=makeScratch("window.gated",expanded->elements);
            branchHeads=makeScratch("window.branchHeads",input->elements);headsOutput=denseView("window.headsOutput",input->elements,channels,pixels);
            addGemm(published,first,hidden,pixels,channels,channels);
            addGemm(branchInput,expand,expanded,pixels,256,64,groups);
            addGemm(gated,contraction,branchHeads,pixels,64,256,groups);
            addGemm(headsOutput,outProjection,branch,pixels,channels,channels);attentionBase=4;
        }else{
            auto expand=loadWeight(folder,"expand",UINT64(heads)*4*heads*32*32);
            auto branchProjection=loadWeight(folder,"branch_projection",UINT64(heads)*4*32*32);
            auto outProjection=loadWeight(folder,"ffn_projection",UINT64(channels)*channels);
#ifdef NR_BRANCH_COMPACT
            if(compactBranch){
                branchInput=hidden=expanded=gated=branchHeads=nullptr;headsOutput=denseView("window.headsOutput",input->elements,channels,pixels);
                initCompactBranch(expand,branchProjection);reserveFusedGemm(input,expand,headsOutput);reserveFusedGemm(input,branchProjection,headsOutput);
            }else{
#endif
            branchInput=makeScratch("window.branchInput",UINT64(heads)*4*heads*pixels*32);hidden=makeScratch("window.hidden",branchInput->elements);
            expanded=makeScratch("window.expanded",UINT64(heads)*4*pixels*32);gated=makeScratch("window.gated",expanded->elements);branchHeads=makeScratch("window.branchHeads",gated->elements);
            headsOutput=denseView("window.headsOutput",input->elements,channels,pixels);
            addGemm(branchInput,expand,hidden,pixels,32,32,heads*4*heads);
            addGemm(gated,branchProjection,branchHeads,pixels,32,32,heads*4);
#ifdef NR_BRANCH_COMPACT
            }
#endif
            addGemm(headsOutput,outProjection,branch,pixels,channels,channels);attentionBase=3;
        }
        windows=denseView("window.windows",UINT64(windowPixels)*channels,channels,windowPixels);
#ifdef NR_WINDOW_SEGMENT
        if(reuseWindowFeed)feed=windows;
#endif
        qkv=makeScratch("window.qkv",windows->elements*3);
#ifdef NR_GLOBAL_NORMALIZATION
        // Fused attention reads qkv directly. Placeholder jobs retain a valid view,
        // but never execute; do not allocate the eliminated normalized FP32 planes.
        packed=NativeRuntime::context->linAlgWindowNormalizeFused?qkv:makeScratch("window.packed",qkv->elements);
#else
        packed=makeScratch("window.packed",qkv->elements);
#endif
        scores=makeScratch("window.scores",UINT64(windowPixels)*heads*64);probabilities=makeScratch("window.probabilities",scores->elements);
        attended=makeScratch("window.attended",windows->elements);context=denseView("window.context",windows->elements,channels,windowPixels);projected=makeScratch("window.projected",windows->elements);output=make(input->elements);
        addGemm(windows,qkvWeight,qkv,windowPixels,channels*3,channels);
#ifdef NR_EXPERIMENTAL_LINALG
        if(NativeRuntime::context->linAlgWindowFused){
            auto& ctx=*NativeRuntime::context;
            if(!ctx.linAlgFusedWindowPipeline){
                linAlgCheckWaveFP16(device.Get());ctx.linAlgFusedWindowRoot=linAlgWindowRoot(device.Get());
                const char* attentionShader=ctx.linAlgWindowNormalizeFused?"window-normalized-fused.dxil":"window-fused.dxil";
#ifdef NR_GLOBAL_NORMALIZATION
                if(ctx.globalNormalizationPolicy=="fused-native-round")attentionShader="window-normalized-native-round.dxil";
#endif
#ifdef NR_ATTENTION_PARTITION
                if(ctx.attentionPartitionPolicy!="baseline"){
                    if(!ctx.linAlgWindowNormalizeFused||ctx.globalNormalizationPolicy!="fused-native-round")throw std::runtime_error("Attention partition requires native-round normalization");
                    attentionShader=ctx.attentionPartitionPolicy=="wg64"?"attention-wg64.dxil":ctx.attentionPartitionPolicy=="wg128"?"attention-wg128.dxil":"attention-wg256.dxil";
                }
#endif
#ifdef NR_NORM_WAVE
                if(ctx.normWavePolicy=="wave8"){
                    if(ctx.attentionPartitionPolicy!="wg256"||ctx.globalNormalizationPolicy!="fused-native-round")throw std::runtime_error("Wave normalization requires native-round WG256");
                    attentionShader="attention-norm-wave8.dxil";
                }
#endif
#ifdef NR_SOFTMAX_WAVE
                if(ctx.softmaxWavePolicy=="wave8"){
                    if(ctx.normWavePolicy!="wave8"||ctx.attentionPartitionPolicy!="wg256")throw std::runtime_error("Softmax Wave requires normalized WG256");
                    attentionShader="attention-softmax-wave8.dxil";
                }
#endif
#ifdef NR_ATTENTION_REGISTERS
                if(ctx.attentionRegistersPolicy=="fused"){
                    if(ctx.softmaxWavePolicy!="wave8"||ctx.normWavePolicy!="wave8"||ctx.attentionPartitionPolicy!="wg256"||ctx.globalNormalizationPolicy!="fused-native-round")throw std::runtime_error("Attention registers require accepted Wave8 WG256 native-round");
                    attentionShader="attention-registers.dxil";
                }
#endif
                ctx.linAlgFusedWindowPipeline=linAlgPso(device.Get(),ctx.linAlgFusedWindowRoot.Get(),attentionShader);
            }
            if(UINT64(batch)*heads>65535)throw std::runtime_error("Fused window dispatch limit");
            reserveFusedGemm(packed,packed,scores);reserveFusedGemm(probabilities,packed,attended);++ctx.linAlgFusedWindows;
        }else
#endif
        {
        addGemm(packed,packed,scores,64,64,32,batch*heads,true,0,windows->elements*4);
        addGemm(probabilities,packed,attended,64,32,64,batch*heads,false,0,windows->elements*8);
        }
        addGemm(context,projection,projected,windowPixels,channels,channels);
#ifdef NR_WINDOW_SEGMENT
        if(fuseC32Feed)initC32Feed();
#endif
        if(keepSkip)retainedSkip=make(output->elements);
        D3D12_ROOT_PARAMETER params[7]{};
        for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[4].Constants.Num32BitValues=10;params[4].Constants.ShaderRegister=1;
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[5].Descriptor.ShaderRegister=1;
        D3D12_ROOT_SIGNATURE_DESC desc{};batchRoot(params[6]);desc.NumParameters=7;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Window root serialization");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Window root");
        for(const char* entry:{"WindowPublish","WindowGate","WindowFeedResidual","WindowPack","WindowNormalize","WindowSoftmax","WindowCropResidual","WindowMergeContext","BranchPackInput","BranchSumInput","BranchSumOutput","SplitPackInput","SplitMergeOutput"})compile(entry);
        if(fusedGate)compile("BranchSumGate");
        if(fusedFeedPack)compile("WindowFeedPack");
        if(diagnosticCompensatedProjection())compile("SplitCompensatedProject");
        if(isolate)fprintf(stderr,"Diagnostic submit: initialization\n");submit();uploads.clear();
    }
    Tensor* resultTensor() const{return output;}
#ifdef NR_C32_COMPACT
    void recordCompactC32Unit(){if(!compactC32)throw std::runtime_error("C32 unit policy disabled");recordCompactC32();}
    Tensor* compactC32UnitResult() const{return branch;}
#endif
#ifdef NR_BRANCH_COMPACT
    void recordCompactBranchUnit(){if(!compactBranch)throw std::runtime_error("Compact branch unit not enabled");recordCompactBranch();}
    Tensor* compactBranchUnitResult()const{return headsOutput;}
#endif
    Tensor* skipTensor() const{return retainedSkip;}
    void record(){
#ifdef NR_BRANCH_COMPACT
        if(!compactBranch
#ifdef NR_C32_COMPACT
            &&!compactC32
#endif
        )
#endif
        dispatch("WindowPublish",input,input,scale,published,published->elements);
        if(shape[8]==32){
#ifdef NR_C32_COMPACT
            if(compactC32)recordCompactC32();else
#endif
            {gemm(0);dispatch("WindowGate",hidden,input,scale,gated,gated->elements);gemm(1);}
        }
        else if(shape[8]==512){
            gemm(0);dispatch("WindowPublish",hidden,input,scale,splitFirst,splitFirst->elements);
            dispatch("SplitPackInput",splitFirst,input,scale,branchInput,branchInput->elements);gemm(1);
            dispatch("WindowGate",expanded,input,scale,gated,gated->elements);
            if(diagnosticCompensatedProjection())dispatch("SplitCompensatedProject",gated,gated,splitProjectionWeight,branchHeads,branchHeads->elements);
            else gemm(2);
            dispatch("SplitMergeOutput",branchHeads,input,scale,headsOutput,headsOutput->elements);gemm(3);
        }else{
#ifdef NR_BRANCH_COMPACT
            if(compactBranch){recordCompactBranch();gemm(2);}else{
#endif
            dispatch("BranchPackInput",published,input,scale,branchInput,branchInput->elements);gemm(0);
            if(fusedGate)dispatch("BranchSumGate",hidden,input,scale,gated,gated->elements);
            else{
                dispatch("BranchSumInput",hidden,input,scale,expanded,expanded->elements);
                dispatch("WindowGate",expanded,input,scale,gated,gated->elements);
            }
            gemm(1);
            dispatch("BranchSumOutput",branchHeads,input,scale,headsOutput,headsOutput->elements);gemm(2);
#ifdef NR_BRANCH_COMPACT
            }
#endif
        }
#ifdef NR_WINDOW_SEGMENT
        if(!fuseC32Feed){
#endif
        if(fusedFeedPack)dispatch("WindowFeedPack",branch,input,ffnCos,windows,windows->elements,
#ifdef NR_WINDOW_SEGMENT
            reuseWindowFeed?nullptr:feed
#else
            feed
#endif
        );
        else{
            dispatch("WindowFeedResidual",branch,input,ffnCos,feed,feed->elements);
            dispatch("WindowPack",feed,input,scale,windows,windows->elements);
        }
#ifdef NR_WINDOW_SEGMENT
        }
#endif
        gemm(attentionBase);
#ifdef NR_EXPERIMENTAL_LINALG
        if(!NativeRuntime::context->linAlgWindowNormalizeFused)
#endif
        dispatch("WindowNormalize",qkv,input,scale,packed,windows->elements);
#ifdef NR_EXPERIMENTAL_LINALG
        if(NativeRuntime::context->linAlgWindowFused)fusedAttention();else
#endif
        {
        gemm(attentionBase+1);
        dispatch("WindowSoftmax",scores,input,bias,probabilities,probabilities->elements);gemm(attentionBase+2);
        dispatch("WindowMergeContext",attended,input,scale,context,context->elements);
        }
        gemm(attentionBase+3);
        dispatch("WindowCropResidual",projected,feed,attnCos,output,output->elements);
        if(retainedSkip)dispatch("WindowPublish",output,input,scale,retainedSkip,retainedSkip->elements);
    }
    std::vector<float> run(){record();return download(output);}
    void capture(const std::string& folder){
#ifdef NR_WINDOW_SEGMENT
        if(reuseWindowFeed||fuseC32Feed)throw std::runtime_error("Window segment capture needs logical feed views");
#endif
#ifdef NR_C32_COMPACT
        if(compactC32)throw std::runtime_error("Compact C32 intermediates are not FP32 capture views");
#endif
#ifdef NR_BRANCH_COMPACT
        if(compactBranch)throw std::runtime_error("Compact branch intermediates are not FP32 capture views");
#endif
#ifdef NR_EXPERIMENTAL_LINALG
        if(NativeRuntime::context->linAlgWindowFused)throw std::runtime_error("Fused window intermediates are not materialized for capture");
#endif
        // The eliminated intermediate is materialized only for explicit laboratory capture.
        if(fusedGate&&shape[8]>32&&shape[8]<512)
            dispatch("BranchSumInput",hidden,input,scale,expanded,expanded->elements);
        const std::pair<const char*,Tensor*> stages[]={{"hidden",hidden},{"gate",gated},{"branch",branch},{"feed",feed},
            {"windows",windows},{"qkv",qkv},{"packed",packed},{"scores",scores},{"probabilities",probabilities},
            {"attended",attended},{"context",context},{"projected",projected}};
        for(auto& stage:stages){auto values=download(stage.second);std::ofstream file(folder+"/native_"+stage.first+".bin",std::ios::binary);
            file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*4));if(!file)throw std::runtime_error("Capture write failed");}
        if(shape[8]>32){
            const std::pair<const char*,Tensor*> branches[]={{"branch_input",branchInput},{"expanded",expanded},{"branch_heads",branchHeads},{"heads_output",headsOutput}};
            for(auto& stage:branches){auto values=download(stage.second);std::ofstream file(folder+"/native_"+stage.first+".bin",std::ios::binary);
                file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*4));if(!file)throw std::runtime_error("Branch capture write failed");}
        }
        if(splitFirst){auto values=download(splitFirst);std::ofstream file(folder+"/native_split_first.bin",std::ios::binary);
            file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*4));if(!file)throw std::runtime_error("Split capture write failed");}
    }
};
