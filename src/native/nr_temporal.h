#pragma once
#include "dml_runtime.h"

// GPU producer interface for motion/depth validation. Allocation is setup only;
// production producers write these buffers on the associated frame's command list.
class NativeTemporalInputs : public NativeRuntime {
    Tensor *motion,*confidence;
    UINT width,height;
public:
    NativeTemporalInputs(std::shared_ptr<NativeContext> ctx,UINT w,UINT h):NativeRuntime(ctx),width(w),height(h){
        if(!w||!h||w>1920||h>1152)throw std::runtime_error("Invalid temporal input geometry");
        motion=make(UINT64(w)*h*2,false);confidence=make(UINT64(w)*h,false);
    }
    Tensor* motionTensor(){return motion;}Tensor* confidenceTensor(){return confidence;}
    UINT lowWidth()const{return width;}UINT lowHeight()const{return height;}
    void seedTestFixture(const std::string& folder){
        // Test setup only; forbidden by NativeRuntime while externally recording.
        auto m=load(folder,"motion",motion->elements),c=load(folder,"confidence",confidence->elements);
        for(auto pair:{std::make_pair(m,motion),std::make_pair(c,confidence)}){
            pair.first->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);pair.second->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(pair.second->resource.Get(),0,pair.first->resource.Get(),0,pair.first->elements*4);
        }
        submit();
    }
};

// Persistent history is deliberately separate from the aliased graph scratch.
// The owner must ensure same-DIRECT-queue order and fence-safe rebinds. This pass
// neither submits a list nor downloads a tensor during external recording.
class NativeTemporalPass : public NativeRuntime {
    Tensor *colour,*noise,*head,*motion,*confidence;
    Tensor *history[2],*baseHistory[2],*residualHistory[2],*reprojected,*features,*composed,*stabilityTrace;
    UINT shape[12]; UINT parity=0;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> reprojectionShader,featuresShader,compositionShader;
    void bind(ID3D12PipelineState* shader) {
        Tensor* input[]={colour,noise,head,history[parity],motion,confidence,baseHistory[parity],residualHistory[parity]};
        Tensor* output[]={reprojected,features,history[parity^1],composed,baseHistory[parity^1],residualHistory[parity^1],stabilityTrace};
        for(auto p:input)p->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto p:output)p->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(shader);
        for(UINT i=0;i<8;++i)cmd->SetComputeRootShaderResourceView(i,input[i]->resource->GetGPUVirtualAddress());
        for(UINT i=0;i<7;++i)cmd->SetComputeRootUnorderedAccessView(i+8,output[i]->resource->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstants(15,12,shape,0);
    }
public:
    NativeTemporalPass(UINT w,UINT h,UINT pw,UINT ph,std::shared_ptr<NativeContext> ctx,
                       Tensor* current,Tensor* noiseInput,Tensor* headInput,Tensor* normalizedMotion,Tensor* validity)
        :NativeRuntime(ctx),colour(current),noise(noiseInput),head(headInput),motion(normalizedMotion),confidence(validity),
         shape{w,h,pw,ph,0,1,0,1,0,0,0,0} {
        if(!w||!h||pw<w||ph<h||pw>1920||ph>1152||ctx->tileBatch!=1)throw std::runtime_error("Invalid temporal extent/batch");
        Tensor* inputs[]={colour,noise,head,motion,confidence};
        UINT64 counts[]={UINT64(w)*h*3,UINT64(pw)*ph*3,UINT64(pw)*ph*4,UINT64(w)*h*2,UINT64(w)*h};
        for(UINT i=0;i<5;++i)if(!inputs[i]||inputs[i]->elements!=counts[i])throw std::runtime_error("Temporal tensor mismatch");
        history[0]=make(UINT64(w)*h*3,false);history[1]=make(UINT64(w)*h*3,false);
        for(UINT i=0;i<2;++i){baseHistory[i]=make(UINT64(w)*h*3,false);residualHistory[i]=make(UINT64(w)*h*3,false);}
        reprojected=make(UINT64(w)*h*4,false);features=make(UINT64(pw)*ph*16,false);composed=make(UINT64(w)*h*3,false);stabilityTrace=make(UINT64(w)*h*4,false);
        D3D12_ROOT_PARAMETER params[16]{};
        for(UINT i=0;i<15;++i){params[i].ParameterType=i<8?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV;params[i].Descriptor.ShaderRegister=i<8?i:i-8;}
        params[15].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[15].Constants.Num32BitValues=12;
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=16;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Temporal root serialize");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Temporal root");
        auto compile=[&](const char* entry,ComPtr<ID3D12PipelineState>& pso){
#ifdef NR_HISTORY_EXPERIMENTS
            const D3D_SHADER_MACRO experimental[]={{"NR_HISTORY_EXPERIMENTS","1"},{nullptr,nullptr}};
            blob=compileNativeShader("temporal_ops.hlsl",entry,experimental);
#else
            blob=compileNativeShader("temporal_ops.hlsl",entry);
#endif
            D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&pso)),entry);};
        compile("Reproject",reprojectionShader);compile("BuildFeatures",featuresShader);compile("ComposeTemporal",compositionShader);
    }
    // Setup-only fixture import. Production seeds by Reset + GPU composition.
    Tensor* testHistory(){return history[parity];}
    Tensor* featureTensor(){return features;} Tensor* reprojectedTensor(){return reprojected;}
    Tensor* composedTensor(){return composed;}
    void diagnostic(bool enabled){shape[10]=enabled;}
    Tensor* diagnosticTensor(){return stabilityTrace;}
    void stability(float value,bool bounded=false,UINT historyMode=0,bool adaptive=false,bool motionAware=false){if(!std::isfinite(value)||value<0||value>.4f||historyMode>3)throw std::runtime_error("Invalid residual stability");memcpy(&shape[8],&value,4);shape[9]=motionAware?3:adaptive?2:UINT(bounded);shape[11]=historyMode;}
    void settings(bool valid,bool reset,float margin,bool clamp=true) {
        if(!std::isfinite(margin)||margin<0)throw std::runtime_error("Invalid temporal clamp margin");
        shape[4]=valid;shape[5]=reset;memcpy(&shape[6],&margin,4);shape[7]=clamp;
    }
    void recordFeatures() {
        bind(reprojectionShader.Get());cmd->Dispatch((shape[0]+7)/8,(shape[1]+7)/8,1);uavBarrier(cmd.Get());
        bind(featuresShader.Get());cmd->Dispatch((shape[2]+7)/8,(shape[3]+7)/8,1);uavBarrier(cmd.Get());
    }
    void recordComposition() {
        bind(compositionShader.Get());cmd->Dispatch((shape[0]+7)/8,(shape[1]+7)/8,1);uavBarrier(cmd.Get());
    }
    // Call only after the recorded sequence was submitted successfully. A failed
    // or reset recording must not advance history. No per-backbuffer histories.
    void advanceSubmittedFrame(){parity^=1;shape[4]=1;shape[5]=0;}
};
