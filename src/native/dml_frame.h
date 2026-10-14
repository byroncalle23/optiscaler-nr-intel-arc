#pragma once
#include "dml_runtime.h"
#include <d3dcompiler.h>

class FrameHarness : public NativeRuntime {
    UINT shape[4]{};
    Tensor *source,*other,*output;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> shader;
public:
    // Constructor uploads are setup only. record() has no CPU reads or submissions.
    FrameHarness(const std::string& folder,std::shared_ptr<NativeContext> existing=nullptr,
                 Tensor* head=nullptr,Tensor* colour=nullptr,Tensor* residentColour=nullptr):NativeRuntime(existing){
        std::ifstream config(nativePath(folder+"/shape.txt"));
        if(!(config>>shape[0]>>shape[1]>>shape[2]>>shape[3]))throw std::runtime_error("Invalid frame geometry");
        for(UINT n:shape)if(!n||n>1920)throw std::runtime_error("Frame extent out of range");
        const bool compose=head!=nullptr;
        if(compose){
            if(!colour||shape[2]>shape[0]||shape[3]>shape[1])throw std::runtime_error("Invalid frame crop");
            source=head;other=colour;
            if(source->elements!=UINT64(shape[0])*shape[1]*4||other->elements!=UINT64(shape[2])*shape[3]*3)throw std::runtime_error("Frame tensor mismatch");
        }else{
            source=residentColour?residentColour:load(folder,"colour",UINT64(shape[0])*shape[1]*3);
            if(source->elements!=UINT64(shape[0])*shape[1]*3)throw std::runtime_error("Resident frame colour mismatch");
            other=load(folder,"noise",UINT64(shape[2])*shape[3]*3);
        }
        output=make(UINT64(shape[2])*shape[3]*(compose?3:16));
        D3D12_ROOT_PARAMETER params[5]{};
        for(UINT i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[3].Constants.Num32BitValues=4;
        D3D12_ROOT_SIGNATURE_DESC desc{};batchRoot(params[4]);desc.NumParameters=5;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;
        check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Frame root serialization");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Frame root");
        blob=compileBatchedShader("frame_ops.hlsl",compose?"Compose":"Features");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();pipeline.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&shader)),"Frame pipeline");
        submit();uploads.clear();
    }
    Tensor* resultTensor() const{return output;}
    Tensor* noiseTensor() const{return other;}
    UINT paddedWidth() const{return shape[3];}
    UINT paddedHeight() const{return shape[2];}
    Tensor* colourTensor() const{return source;}
    void record(){
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);other->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        output->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(shader.Get());
        cmd->SetComputeRootShaderResourceView(0,source->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(1,other->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2,output->resource->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstants(3,4,shape,0);
        UINT groups=UINT((output->elements+127)/128);bindBatch(4,{source,other,nullptr,output});cmd->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,context->tileBatch);uavBarrier(cmd.Get());
    }
};
