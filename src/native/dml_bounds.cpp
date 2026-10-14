// Regression: a 2D dispatch must leave an intentionally larger buffer's tail untouched.
#include "dml_runtime.h"
#include <d3dcompiler.h>
class BoundsProbe : public NativeRuntime {
    void fill(Tensor* target,float value){
        auto upload=buffer(device.Get(),target->elements*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped;D3D12_RANGE empty{0,0};check(upload->Map(0,&empty,&mapped),"Canary upload");
        std::fill_n(static_cast<float*>(mapped),static_cast<size_t>(target->elements),value);upload->Unmap(0,nullptr);
        target->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);cmd->CopyBufferRegion(target->resource.Get(),0,upload.Get(),0,target->elements*4);
        target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);uploads.push_back(upload);
    }
public:
    void test(){
        constexpr UINT count=320*320*128,groupsX=65535,groupsY=2;
        constexpr UINT capacity=groupsX*groupsY*128;
        auto source=make(capacity),target=make(capacity);fill(source,.1f);submit();uploads.clear();
        D3D12_ROOT_PARAMETER params[5]{};
        for(UINT i=0;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[4].Constants.ShaderRegister=1;params[4].Constants.Num32BitValues=10;
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=5;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Canary root serialization");
        ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Canary root");
        for(const char* entry:{"WindowPublish","WindowGate"}){
            blob=compileNativeShader("window_ops.hlsl",entry);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"Canary pipeline");
            fill(target,12345.0f);source->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pipeline.Get());
            for(UINT i=0;i<3;++i)cmd->SetComputeRootShaderResourceView(i,source->resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(3,target->resource->GetGPUVirtualAddress());
            UINT constants[]={320,320,0,0,320,320,0,128,32,count};cmd->SetComputeRoot32BitConstants(4,10,constants,0);
            cmd->Dispatch(groupsX,groupsY,1);uavBarrier(cmd.Get());auto data=download(target);uploads.clear();
            float expected=strcmp(entry,"WindowGate")==0 ? .09375f : .1015625f;
            for(UINT i=0;i<count;++i)if(data[i]!=expected)throw std::runtime_error("Incorrect bounds probe result");
            for(UINT i=count;i<capacity;++i)if(data[i]!=12345.0f)throw std::runtime_error("Shader overwrote the protected tail");
            printf("{\"shader\":\"%s\",\"logical_elements\":%u,\"guard_elements\":%u,\"guard_unchanged\":true}\n",entry,count,capacity-count);
        }
    }
};
int main(){try{BoundsProbe probe;probe.test();return 0;}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
