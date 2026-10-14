// Reuse device/buffer helpers and Agility exports; retain the identity probe.
#define main cvIdentityMain
#include "coop_vector_probe.cpp"
#undef main
#include <d3d12sdklayers.h>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdlib>

std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error("Missing operand/shader: "+path);
    std::vector<char> b(size_t(f.tellg())); f.seekg(0);f.read(b.data(),b.size());
    if(!f)throw std::runtime_error("Failed read: "+path);return b;
}
void transition(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};c->ResourceBarrier(1,&b);
}
int main(int argc,char** argv) {try {
    if(argc!=9)throw std::runtime_error("Usage: coop_matrix shader folder M N K optimal TN TK");
    const std::string folder=argv[2];const UINT m=UINT(std::stoul(argv[3])),n=UINT(std::stoul(argv[4])),k=UINT(std::stoul(argv[5]));
    const bool optimal=std::stoi(argv[6])!=0;const UINT tn=UINT(std::stoul(argv[7])),tk=UINT(std::stoul(argv[8]));
    if(!m||!n||!k||!tn||!tk||n%tn||k%tk||UINT64(m)*n*4>256*1024*1024)throw std::runtime_error("Unsupported shape/budget");
    UUID features[]={D3D12ExperimentalShaderModels,D3D12CooperativeVectorExperiment};
    ck(D3D12EnableExperimentalFeatures(2,features,nullptr,nullptr),"EnableExperimentalFeatures");
    const bool debugEnabled=std::getenv("NR_CV_NO_DEBUG")==nullptr;
    ComPtr<ID3D12Debug> debug;if(debugEnabled){ck(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"Debug");debug->EnableDebugLayer();}
    ComPtr<IDXGIFactory6> factory;ck(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Factory");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x8086&&d.DeviceId==0xe20b){adapter=a;break;}}
    if(!adapter)throw std::runtime_error("B580 missing");
    ComPtr<ID3D12Device> device;ck(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&device)),"Device");
    ComPtr<ID3D12InfoQueue> info;if(debugEnabled)ck(device.As(&info),"InfoQueue");
    ComPtr<ID3D12DevicePreview> preview;ck(device.As(&preview),"DevicePreview");
    D3D12_FEATURE_DATA_D3D12_OPTIONS_EXPERIMENTAL options{};
    ck(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL,&options,sizeof(options)),"Options");
    printf("COOPERATIVE_VECTOR_TIER %u\n",UINT(options.CooperativeVectorTier));
    auto a=readFile(folder+"/a.bin"),w=readFile(folder+"/weights-tiles.bin");
    const UINT sourceStride=tn*tk*2,tileCount=(n/tn)*(k/tk);
    if(a.size()!=UINT64(m)*k*4||w.size()!=UINT64(tileCount)*sourceStride)throw std::runtime_error("Operand sizes");
    D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO dest{};
    dest.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL;
    dest.NumRows=tn;dest.NumColumns=tk;dest.DestDataType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
    preview->GetLinearAlgebraMatrixConversionDestinationInfo(&dest);
    printf("OPTIMAL_DEST_SIZE %u N%u K%u\n",dest.DestSize,tn,tk);
    if(optimal&&!dest.DestSize){
        for(UINT size : {16u,32u,64u,128u,256u}){auto test=dest;test.NumRows=test.NumColumns=size;
            preview->GetLinearAlgebraMatrixConversionDestinationInfo(&test);printf("SIZE_QUERY %u %u\n",size,test.DestSize);}
        for(UINT64 i=0;info&&i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);
            std::vector<char> storage(bytes);auto msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data());info->GetMessage(i,msg,&bytes);fprintf(stderr,"SIZE_QUERY_DEBUG %s\n",msg->pDescription);}
        throw std::runtime_error("Optimal layout size missing");
    }
    const UINT64 stride=optimal?(dest.DestSize+255)&~UINT64(255):sourceStride;
    if(stride>UINT32_MAX)throw std::runtime_error("Weight stride overflow");
    auto uploadA=buf(device.Get(),a.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto uploadW=buf(device.Get(),w.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RANGE empty{0,0};void* mapped=nullptr;
    ck(uploadA->Map(0,&empty,&mapped),"InputMap");memcpy(mapped,a.data(),a.size());uploadA->Unmap(0,&empty);
    ck(uploadW->Map(0,&empty,&mapped),"WeightMap");memcpy(mapped,w.data(),w.size());uploadW->Unmap(0,&empty);
    auto input=buf(device.Get(),a.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto weights=buf(device.Get(),w.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto converted=optimal?buf(device.Get(),stride*tileCount,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS):ComPtr<ID3D12Resource>{};
    const UINT64 outBytes=UINT64(m)*n*4;
    auto output=buf(device.Get(),outBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback=buf(device.Get(),outBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto times=buf(device.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_ROOT_PARAMETER p[4]{};
    for(UINT i=0;i<2;++i){p[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[i].Descriptor.ShaderRegister=i;}
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[3].Constants.Num32BitValues=4;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=4;rd.pParameters=p;
    ComPtr<ID3DBlob> blob,error;ck(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"RootSerialize");
    ComPtr<ID3D12RootSignature> root;ck(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Root");
    auto shader=readFile(argv[1]);D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader.data(),shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;ck(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"PSO");
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> queue;ck(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
    UINT64 frequency;ck(queue->GetTimestampFrequency(&frequency),"Frequency");
    D3D12_QUERY_HEAP_DESC hd{};hd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;hd.Count=2;
    ComPtr<ID3D12QueryHeap> queries;ck(device->CreateQueryHeap(&hd,IID_PPV_ARGS(&queries)),"Queries");
    ComPtr<ID3D12CommandAllocator> allocator;ck(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");
    ComPtr<ID3D12GraphicsCommandList> cmd;ck(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)),"List");
    ComPtr<ID3D12GraphicsCommandListPreview> cmdPreview;ck(cmd.As(&cmdPreview),"ListPreview");
    ComPtr<ID3D12Fence> fence;ck(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");UINT64 signal=0;
    auto submit=[&](){ck(cmd->Close(),"Close");ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
        ck(queue->Signal(fence.Get(),++signal),"Signal");HANDLE ev=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ev)throw std::runtime_error("Event");
        ck(fence->SetEventOnCompletion(signal,ev),"Completion");auto wait=WaitForSingleObject(ev,30000);CloseHandle(ev);
        if(wait!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");ck(device->GetDeviceRemovedReason(),"DeviceState");};
    auto reset=[&](){ck(allocator->Reset(),"AllocatorReset");ck(cmd->Reset(allocator.Get(),pipeline.Get()),"ListReset");};
    cmd->CopyResource(input.Get(),uploadA.Get());cmd->CopyResource(weights.Get(),uploadW.Get());
    transition(cmd.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd.Get(),weights.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(optimal){
        for(UINT t=0;t<tileCount;++t){D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO ci{};ci.DestInfo=dest;
            ci.SrcInfo={sourceStride,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,tk*2};
            ci.DataDesc={converted->GetGPUVirtualAddress()+t*stride,weights->GetGPUVirtualAddress()+UINT64(t)*sourceStride};
            cmdPreview->ConvertLinearAlgebraMatrix(&ci,1);}
        transition(cmd.Get(),converted.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    submit();
    std::vector<double> samples;std::vector<char> previous;
    for(UINT run=0;run<14;++run){reset();cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootShaderResourceView(0,(optimal?converted.Get():weights.Get())->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(1,input->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
        UINT shape[]={m,n,k,UINT(stride)};cmd->SetComputeRoot32BitConstants(3,4,shape,0);
        cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);cmd->Dispatch((m+31)/32,n/tn,1);
        cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);cmd->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times.Get(),0);
        transition(cmd.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(readback.Get(),output.Get());transition(cmd.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit();D3D12_RANGE tr{0,16};ck(times->Map(0,&tr,&mapped),"TimeMap");auto stamps=static_cast<UINT64*>(mapped);
        double ms=double(stamps[1]-stamps[0])*1000/double(frequency);times->Unmap(0,&empty);if(run>=2)samples.push_back(ms);
        D3D12_RANGE rr{0,SIZE_T(outBytes)};ck(readback->Map(0,&rr,&mapped),"OutputMap");
        if(!previous.empty()&&memcmp(previous.data(),mapped,SIZE_T(outBytes)))throw std::runtime_error("Nondeterministic output");
        if(previous.empty())previous.assign(static_cast<char*>(mapped),static_cast<char*>(mapped)+outBytes);readback->Unmap(0,&empty);
    }
    UINT errors=0;for(UINT64 i=0;info&&i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);
        std::vector<char> storage(bytes);auto msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data());ck(info->GetMessage(i,msg,&bytes),"DebugMessage");
        if(msg->Severity==D3D12_MESSAGE_SEVERITY_ERROR||msg->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){fprintf(stderr,"D3D12_ERROR %s\n",msg->pDescription);++errors;}}
    if(errors)throw std::runtime_error("D3D12 debug errors");
    std::ofstream out(folder+"/output.bin",std::ios::binary);out.write(previous.data(),previous.size());if(!out)throw std::runtime_error("Output write");
    double mean=0;for(auto v:samples)mean+=v;mean/=samples.size();std::sort(samples.begin(),samples.end());
    std::ofstream report(folder+"/gpu.json");report<<"{\"gpu_mean_ms\":"<<mean<<",\"gpu_median_ms\":"<<(samples[5]+samples[6])/2
        <<",\"gpu_min_ms\":"<<samples.front()<<",\"gpu_max_ms\":"<<samples.back()<<",\"warm_runs\":12,\"repeat_exact\":true,\"debug_enabled\":"<<(debugEnabled?"true":"false")<<",\"debug_errors\":"<<(debugEnabled?"0":"null")<<",\"optimal\":"<<(optimal?"true":"false")<<",\"weight_tile_stride\":"<<stride<<"}";
    printf("MATRIX_PASS M%u N%u K%u GPU_MS %.6f OPTIMAL %u\n",m,n,k,mean,optimal);return 0;
}catch(const std::exception& e){fprintf(stderr,"MATRIX_FAILED %s\n",e.what());return 1;}}
