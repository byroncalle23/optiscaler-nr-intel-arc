#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cmath>
using Microsoft::WRL::ComPtr;
extern "C" {__declspec(dllexport) extern const UINT D3D12SDKVersion=717;__declspec(dllexport) extern const char* D3D12SDKPath=".\\D3D12\\";}
void ck(HRESULT h,const char* label){printf("%s HRESULT 0x%08X\n",label,UINT(h));if(FAILED(h))throw std::runtime_error(label);}
ComPtr<ID3D12Resource> buf(ID3D12Device* d,UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(type==D3D12_HEAP_TYPE_DEFAULT)rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;ck(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)),"Buffer");return r;
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: coop_vector_probe shader.dxil");
    UUID features[]={D3D12ExperimentalShaderModels,D3D12CooperativeVectorExperiment};
    ck(D3D12EnableExperimentalFeatures(2,features,nullptr,nullptr),"EnableExperimentalFeatures");
    ComPtr<IDXGIFactory6> factory;ck(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Factory");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);
        if(desc.VendorId==0x8086&&desc.DeviceId==0xe20b){adapter=a;printf("ADAPTER B580 PCI 8086:E20B VRAM %llu\n",UINT64(desc.DedicatedVideoMemory));break;}}
    if(!adapter)throw std::runtime_error("B580 missing");
    ComPtr<ID3D12Device> device;ck(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&device)),"CreateDevice");
    D3D12_FEATURE_DATA_D3D12_OPTIONS_EXPERIMENTAL options{};ck(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL,&options,sizeof(options)),"CooperativeVectorFeature");
    printf("COOPERATIVE_VECTOR_TIER %u\n",UINT(options.CooperativeVectorTier));if(options.CooperativeVectorTier==D3D12_COOPERATIVE_VECTOR_TIER_NOT_SUPPORTED)throw std::runtime_error("Cooperative vectors unsupported");
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_9};ck(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm)),"ShaderModel");printf("SHADER_MODEL 0x%X\n",sm.HighestShaderModel);
    D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=p;ComPtr<ID3DBlob> blob,error;ck(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"RootSerialize");
    ComPtr<ID3D12RootSignature> root;ck(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Root");
    std::ifstream file(argv[1],std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Shader missing");std::vector<char> shader(size_t(file.tellg()));file.seekg(0);file.read(shader.data(),shader.size());
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader.data(),shader.size()};ComPtr<ID3D12PipelineState> pipeline;ck(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"CooperativeVectorPSO");
    // Identity matrix is exactly representable in FP16, giving an independent exact reference.
    auto weights=buf(device.Get(),512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped;D3D12_RANGE empty{0,0};ck(weights->Map(0,&empty,&mapped),"WeightMap");
    auto half=static_cast<unsigned short*>(mapped);for(UINT i=0;i<256;++i)half[i]=(i/16==i%16)?0x3c00:0;weights->Unmap(0,&empty);
    auto output=buf(device.Get(),4096*16*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);auto readback=buf(device.Get(),4096*16*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> queue;ck(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
    ComPtr<ID3D12CommandAllocator> allocator;ck(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");ComPtr<ID3D12GraphicsCommandList> cmd;ck(device->CreateCommandList(0,q.Type,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&cmd)),"List");
    cmd->SetComputeRootSignature(root.Get());cmd->SetComputeRootShaderResourceView(0,weights->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());cmd->Dispatch(128,1,1);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};cmd->ResourceBarrier(1,&barrier);
    cmd->CopyResource(readback.Get(),output.Get());ck(cmd->Close(),"Close");ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;ck(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Event");
    ck(queue->Signal(fence.Get(),1),"Signal");ck(fence->SetEventOnCompletion(1,event),"Completion");DWORD wait=WaitForSingleObject(event,30000);CloseHandle(event);if(wait!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");ck(device->GetDeviceRemovedReason(),"DeviceState");
    D3D12_RANGE range{0,4096*16*4};ck(readback->Map(0,&range,&mapped),"Readback");auto result=static_cast<float*>(mapped);UINT mismatch=0;
    for(UINT row=0;row<4096;++row)for(UINT k=0;k<16;++k)if(result[row*16+k]!=float(k+row%7)/16)++mismatch;
    readback->Unmap(0,&empty);printf("FP16_IDENTITY_VALUES 65536 MISMATCHES %u\n",mismatch);if(mismatch)throw std::runtime_error("Numeric mismatch");return 0;
}catch(const std::exception& e){fprintf(stderr,"PROBE_FAILED %s\n",e.what());return 1;}}
