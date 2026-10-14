#pragma once
#include "dml_runtime.h"
#include <d3dcompiler.h>
#include <map>

class EndpointHarness : public NativeRuntime {
    UINT kind,shape[4]{};
    Tensor *source,*output,*gainInput=nullptr,*convInput=nullptr,*gain=nullptr,*conv=nullptr;
    Tensor *skip=nullptr,*sine=nullptr,*cosine=nullptr;
    Tensor* adapterWeight=nullptr;
    bool compensatedAdapter=false;
    ComPtr<ID3D12RootSignature> root;
    std::map<std::string,ComPtr<ID3D12PipelineState>> shaders;
    void compile(const char* entry){auto blob=compileBatchedShader("endpoint_ops.hlsl",entry);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();desc.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&shaders[entry])),entry);}
    void dispatch(const char* entry,Tensor* a,Tensor* b,Tensor* s,Tensor* c,Tensor* target){
        Tensor* inputs[]={a,b,s,c};for(auto t:inputs)t->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(shaders.at(entry).Get());
        for(UINT i=0;i<4;++i)cmd->SetComputeRootShaderResourceView(i,inputs[i]->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(4,target->resource->GetGPUVirtualAddress());cmd->SetComputeRoot32BitConstants(5,4,shape,0);
        UINT groups=static_cast<UINT>((target->elements+127)/128);bindBatch(6,{a,b,s,target,c});cmd->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,context->tileBatch);uavBarrier(cmd.Get());}
    void gemm(size_t index){recordGemm(*jobs[index]);}
public:
    explicit EndpointHarness(const std::string& folder,std::shared_ptr<NativeContext> existing=nullptr,Tensor* residentInput=nullptr,Tensor* residentSkip=nullptr) : NativeRuntime(existing){
        char precision[4]{};compensatedAdapter=GetEnvironmentVariableA("NR_ADAPTER_COMPENSATED_PROJECTION",precision,sizeof(precision))==1 && precision[0]=='1';
        if(NativeRuntime::context->acceptedGameGlobalPolicy)compensatedAdapter=false;
        std::ifstream config(nativePath(folder+"/shape.txt"));if(!(config>>kind>>shape[0]>>shape[1]>>shape[2]>>shape[3]))throw std::runtime_error("Invalid endpoint config");
        UINT h=shape[0],w=shape[1],th=shape[2],tw=shape[3];
        if(kind<1 || kind>3 || !h || !w || h>1152 || w>1920 || !th || !tw)throw std::runtime_error("Invalid endpoint geometry");
        if(kind!=3 && (th!=h || tw!=w))throw std::runtime_error("Endpoint target mismatch");
        if(kind==3 && (th>2*h || tw>2*w))throw std::runtime_error("Final merge target exceeds doubled source");
        source=residentInput ? residentInput : load(folder,"input",UINT64(h)*w*(kind==1 ? 16 : 32));
        if(source->elements!=UINT64(h)*w*(kind==1 ? 16 : 32))throw std::runtime_error("Resident input extent mismatch");
        if(kind==1){auto weight=load(folder,"adapter",16*32);adapterWeight=weight;output=make(UINT64(h)*w*32);addGemm(source,weight,output,h*w,32,16);}
        else if(kind==2){auto gainWeight=load(folder,"gain",16*4),convWeight=load(folder,"conv",16*4);
            gainInput=make(UINT64(h)*w*16);convInput=make(gainInput->elements);gain=make(UINT64(h)*w*4);conv=make(gain->elements);output=make(gain->elements);
            addGemm(gainInput,gainWeight,gain,h*w,4,16);addGemm(convInput,convWeight,conv,h*w,4,16);
        }else{skip=residentSkip ? residentSkip : load(folder,"skip",UINT64(th)*tw*32);if(skip->elements!=UINT64(th)*tw*32)throw std::runtime_error("Resident final skip extent mismatch");sine=load(folder,"sine",32);cosine=load(folder,"cosine",32);output=make(skip->elements);}
        D3D12_ROOT_PARAMETER params[7]{};for(UINT i=0;i<4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[5].Constants.Num32BitValues=4;
        D3D12_ROOT_SIGNATURE_DESC desc{};batchRoot(params[6]);desc.NumParameters=7;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Endpoint root serialization");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Endpoint root");
        if(kind==2){compile("PackGain");compile("PackConv");compile("AddHead");}if(kind==3)compile("FinalInputMerge");
        if(kind==1&&compensatedAdapter)compile("CompensatedAdapter");
        submit();uploads.clear();
    }
    Tensor* resultTensor() const{return output;}
    void record(){
        if(kind==1){if(compensatedAdapter)dispatch("CompensatedAdapter",source,adapterWeight,source,source,output);else gemm(0);}
        else if(kind==2){dispatch("PackGain",source,source,source,source,gainInput);dispatch("PackConv",source,source,source,source,convInput);
            gemm(0);gemm(1);dispatch("AddHead",gain,conv,source,source,output);
        }else dispatch("FinalInputMerge",source,skip,sine,cosine,output);
    }
    std::vector<float> run(){record();return download(output);}
    void capture(const std::string& folder){if(kind!=2)return;
        const std::pair<const char*,Tensor*> stages[]={{"gain_input",gainInput},{"conv_input",convInput},{"gain",gain},{"conv",conv}};
        for(auto& stage:stages){auto data=download(stage.second);std::ofstream file(folder+"/native_"+stage.first+".bin",std::ios::binary);
            file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*4));if(!file)throw std::runtime_error("Endpoint capture write failed");}}
};
