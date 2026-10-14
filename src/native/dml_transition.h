#pragma once
// Original encoder/decoder level transitions; standalone functional validation.
#include "dml_runtime.h"
#include <d3dcompiler.h>

class TransitionHarness : public NativeRuntime {
    UINT kind,h,w,c,outC,targetH,targetW,pad,publish;
    Tensor *source,*pooled=nullptr,*projected=nullptr,*output,*skip=nullptr,*sine=nullptr;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> poolShader,mergeShader,publishShader;
    void compile(const char* entry,ComPtr<ID3D12PipelineState>& pso){
        ComPtr<ID3DBlob> blob,error;
        blob=compileBatchedShader("transition_ops.hlsl",entry);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
        desc.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pso)),"Transition shader");
    }
    void dispatch(ID3D12PipelineState* pso,Tensor* a,Tensor* b,Tensor* parameter,Tensor* target,
                  UINT x,UINT y,UINT z,UINT flag){
        a->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        parameter->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pso);
        cmd->SetComputeRootShaderResourceView(0,a->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(1,b->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(2,parameter->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3,target->resource->GetGPUVirtualAddress());
        UINT constants[]={x,y,z,flag};cmd->SetComputeRoot32BitConstants(4,4,constants,0);
        if(target->elements>~UINT(0))throw std::runtime_error("Transition exceeds shader element count");
        cmd->SetComputeRoot32BitConstant(5,static_cast<UINT>(target->elements),0);
        UINT groups=static_cast<UINT>((target->elements+127)/128);
        bindBatch(6,{a,b,parameter,target});cmd->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,context->tileBatch);uavBarrier(cmd.Get());
    }
    void gemm(){recordGemm(*jobs[0]);}
public:
    explicit TransitionHarness(const std::string& folder,std::shared_ptr<NativeContext> existing=nullptr,Tensor* residentInput=nullptr,Tensor* residentSkip=nullptr) : NativeRuntime(existing){
        std::ifstream config(nativePath(folder+"/shape.txt"));
        if(!(config>>kind>>h>>w>>c>>outC>>targetH>>targetW>>pad>>publish))throw std::runtime_error("Invalid shape config");
        if(kind<1 || kind>3 || !h || !w || !c || !outC || h>2048 || w>2048 || c>1024 || outC>1024 || publish>1)
            throw std::runtime_error("Unsupported transition dimensions");
        if(UINT64(h)*w*c>536870912ull || UINT64(targetH)*targetW*outC>536870912ull)
            throw std::runtime_error("Transition exceeds standalone buffer limit");
        if(kind!=2 && publish!=1)throw std::runtime_error("Pooling transitions require E4M3 publication");
        source=residentInput ? residentInput : load(folder,"input",UINT64(h)*w*c);
        if(source->elements!=UINT64(h)*w*c)throw std::runtime_error("Resident input extent mismatch");
        if(kind==1 || kind==3){
            if((pad!=0 && pad!=8) || (!pad && (h%2 || w%2)))throw std::runtime_error("Invalid pooling geometry");
            UINT ph=pad ? (h+pad-1)/pad*pad : h,pw=pad ? (w+pad-1)/pad*pad : w;
            if(targetH!=ph/2 || targetW!=pw/2 || (kind==3 && outC!=c))throw std::runtime_error("Pooling target mismatch");
            pooled=make(UINT64(targetH)*targetW*c);
            if(kind==1){auto weight=loadWeight(folder,"weight",UINT64(c)*outC);
                projected=make(UINT64(targetH)*targetW*outC);output=make(projected->elements);
                addGemm(pooled,weight,projected,targetH*targetW,outC,c);
            }else output=pooled;
        }else{
            if(!targetH || !targetW || targetH>2*h || targetW>2*w)throw std::runtime_error("Upsampling target mismatch");
            auto weight=loadWeight(folder,"weight",UINT64(c)*outC);projected=make(UINT64(h)*w*outC);
            skip=residentSkip ? residentSkip : load(folder,"skip",UINT64(targetH)*targetW*outC);
            if(skip->elements!=UINT64(targetH)*targetW*outC)throw std::runtime_error("Resident skip extent mismatch");sine=load(folder,"sine",outC);
            output=make(skip->elements);addGemm(source,weight,projected,h*w,outC,c);
        }
        D3D12_ROOT_PARAMETER params[7]{};
        for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[4].Constants.Num32BitValues=4;
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[5].Constants.Num32BitValues=1;params[5].Constants.ShaderRegister=1;
        D3D12_ROOT_SIGNATURE_DESC desc{};batchRoot(params[6]);desc.NumParameters=7;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Transition root serialization");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Transition root");
        if(kind!=2)compile("PoolPublish",poolShader);
        if(kind==2)compile("UpsampleMerge",mergeShader);
        if(kind==1)compile("TransitionPublish",publishShader);
        submit();uploads.clear();
    }
    Tensor* resultTensor() const{return output;}
    void record(){
        if(kind!=2){dispatch(poolShader.Get(),source,source,source,pooled,h,w,c,pad);
            if(kind==1){gemm();dispatch(publishShader.Get(),projected,source,source,output,0,0,static_cast<UINT>(output->elements),1);}
        }else{gemm();dispatch(mergeShader.Get(),projected,skip,sine,output,w,outC,targetW,publish);}
        // Only the final result is read: no CPU read or wait between compute stages.
    }
    std::vector<float> run(){record();return download(output);}
};
