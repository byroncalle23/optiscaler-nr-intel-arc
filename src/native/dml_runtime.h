#pragma once
#if defined(NR_C32_PROJECTION) && !defined(NR_EXPERIMENTAL_LINALG)
#error C32 projections are standalone SDK721 only
#endif
#if defined(NR_SOFTMAX_WAVE) && !defined(NR_NORM_WAVE)
#error Softmax Wave requires the standalone normalization experiment
#endif
#if defined(NR_NORM_WAVE) && (!defined(NR_ATTENTION_PARTITION) || !defined(NR_GLOBAL_NORMALIZATION) || !defined(NR_EXPERIMENTAL_LINALG))
#error Wave normalization requires the standalone native WG256 experiment
#endif
#if defined(NR_DENSE_TILE) && !defined(NR_EXPERIMENTAL_LINALG)
#error Dense tile is restricted to the standalone SDK experiment
#endif
#if defined(NR_ATTENTION_PARTITION) && (!defined(NR_GLOBAL_NORMALIZATION) || !defined(NR_EXPERIMENTAL_LINALG))
#error Attention partition is restricted to the native normalization experiment
#endif
#if defined(NR_C32_COMPACT) && (!defined(NR_BRANCH_COMPACT) || !defined(NR_GLOBAL_NORMALIZATION) || !defined(NR_EXPERIMENTAL_LINALG))
#error Compact C32 requires the isolated native branch and normalization experiment
#endif
#if defined(NR_GLOBAL_INT8) && !defined(NR_EXPERIMENTAL_LINALG)
#error Global INT8 is restricted to the standalone experimental SDK build
#endif
#if defined(NR_BRANCH_COMPACT) && !defined(NR_EXPERIMENTAL_LINALG)
#error Compact branches are restricted to the standalone experimental SDK build
#endif
#include "dml_common.h"
#include "native_shaders.h"
#include "nr_gpu_profile.h"
#ifdef NR_GAME_FACTORY_PROBE
#include "../../integration/optiscaler/NativeNrRuntime.h"
#endif
#include <d3d12sdklayers.h>
#include <fstream>
#include <memory>
#include <map>
#ifdef NR_GRAPH_DIAGNOSTICS
#include <functional>
#endif
#ifdef NR_EXPERIMENTAL_LINALG
#include "nr_linalg_graph.h"
#endif

struct Tensor {
#ifdef NR_CLEAN_PROFILE
    bool denseHalfShadow=false;
#endif
#ifdef NR_C32_PROJECTION
    UINT c32ProjectionRole=0; // 1=window QKV,2=window context; no inferred generic roles.
#endif
#ifdef NR_GLOBAL_INT8
    std::string int8WeightKey;
#endif
    UINT lanes=1; // elements is the logical per-tile extent, not physical capacity.
    bool immutableWeight=false;
    ComPtr<ID3D12Resource> resource;
    UINT64 elements;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    void to(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_STATES next) {
        if (state != next) { transition(cmd, resource.Get(), state, next); state = next; }
    }
};
struct NativeTrackedState {ID3D12Resource* resource;D3D12_RESOURCE_STATES* state;};
#ifdef NR_CLEAN_PROFILE
struct DenseShadowAudit {ComPtr<ID3D12Resource> readback;UINT64 count;std::string name;};
inline thread_local std::vector<DenseShadowAudit> denseShadowAudits;
inline thread_local UINT denseShadowAuditRecorded=0;
struct DenseRealCapture {ComPtr<ID3D12Resource> readback;UINT64 bytes;std::string name;};
inline thread_local std::vector<DenseRealCapture> denseRealCaptures;
inline thread_local bool denseRealCaptured=false;
inline thread_local UINT denseRealM=0,denseRealN=0,denseRealK=0;
struct DenseInlinePair {ComPtr<ID3D12Resource> candidate,reference,output;UINT count;std::string name;};
inline thread_local std::vector<DenseInlinePair> denseInlinePairs;
inline thread_local UINT denseInlineCount=0;
inline void checkDenseInlinePairs(){
    UINT mismatches=0;for(auto& p:denseInlinePairs){void *a=nullptr,*b=nullptr;D3D12_RANGE range{0,SIZE_T(p.count)*4};check(p.candidate->Map(0,&range,&a),"Inline candidate map");check(p.reference->Map(0,&range,&b),"Inline reference map");
        double e=0,d=0;UINT changed=0;for(UINT i=0;i<p.count;++i){double x=static_cast<float*>(a)[i],y=static_cast<float*>(b)[i];e+=(x-y)*(x-y);d+=y*y;changed+=x!=y;}
        if(changed){++mismatches;printf("DENSE_INLINE_DIFF ordinal=%u job=%s changed=%u rel_rmse=%.12g\n",UINT(&p-denseInlinePairs.data()),p.name.c_str(),changed,sqrt(e/std::max(d,1e-30)));}
        p.candidate->Unmap(0,nullptr);p.reference->Unmap(0,nullptr);
    }
    if(!denseInlinePairs.empty())printf("DENSE_INLINE_COMPLETE jobs=%zu mismatches=%u\n",denseInlinePairs.size(),mismatches);denseInlinePairs.clear();
}
inline void saveDenseRealCaptures(){
    wchar_t directory[32768]{};
    if(denseRealCaptures.empty()||!GetEnvironmentVariableW(L"NR_DENSE_REAL_CAPTURE_DIR",directory,32768))return;
    std::filesystem::create_directories(directory);
    for(auto& c:denseRealCaptures){void* data=nullptr;D3D12_RANGE range{0,SIZE_T(c.bytes)};check(c.readback->Map(0,&range,&data),"Real GEMM capture map");
        std::ofstream out(std::filesystem::path(directory)/c.name,std::ios::binary);out.write(static_cast<char*>(data),c.bytes);c.readback->Unmap(0,nullptr);if(!out)throw std::runtime_error("Real GEMM capture save");}
    std::ofstream shape(std::filesystem::path(directory)/"shape.txt");shape<<denseRealM<<" "<<denseRealN<<" "<<denseRealK<<"\n";
    printf("REAL_DENSE_CAPTURE_SAVED m=%u n=%u k=%u\n",denseRealM,denseRealN,denseRealK);denseRealCaptures.clear();
}
inline void validateDenseShadowAudits(){
    for(auto& a:denseShadowAudits){void* data=nullptr;D3D12_RANGE range{0,SIZE_T(a.count*6)};
        check(a.readback->Map(0,&range,&data),"Dense shadow audit map");
        auto f=static_cast<float*>(data);auto h=reinterpret_cast<uint16_t*>(static_cast<char*>(data)+a.count*4);
        for(UINT64 i=0;i<a.count;++i){uint32_t bits;memcpy(&bits,&f[i],4);uint32_t mag=bits&0x7fffffff;
            uint16_t expected=uint16_t((bits>>16)&32768);
            if(mag)expected|=uint16_t((int((bits>>23)&255)-112)<<10)|uint16_t((bits&0x7fffff)>>13);
            if(!std::isfinite(f[i])||h[i]!=expected){fprintf(stderr,"DENSE_SHADOW_MISMATCH job=%s index=%llu value=%g expected=%u actual=%u\n",a.name.c_str(),i,f[i],expected,h[i]);a.readback->Unmap(0,nullptr);throw std::runtime_error("Producer FP16 shadow differs from FP32 prefix");}
        }
        a.readback->Unmap(0,nullptr);
    }
    if(!denseShadowAudits.empty())printf("DENSE_SHADOW_AUDIT_PASS jobs=%zu\n",denseShadowAudits.size());denseShadowAudits.clear();
}
#endif
struct GemmJob {
#ifdef NR_GLOBAL_INT8
    struct Int8Projection{Tensor *packedA,*packedB,*scratch;UINT m,n,k;float scale;bool compactOutput=false,preparedInput=false;};
    std::unique_ptr<Int8Projection> int8;
#endif
#ifdef NR_EXPERIMENTAL_LINALG
    std::unique_ptr<LinAlgProjection> native;
    std::unique_ptr<LinAlgWaveGemm> wave;
    bool fusedPlaceholder=false;
#endif
    std::string profileName;
    ComPtr<IDMLCompiledOperator> op;
    ComPtr<IDMLOperatorInitializer> initializer;
    ComPtr<IDMLBindingTable> initTable, table;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> scratch, persistent;
    Tensor *a, *b, *out;
    std::vector<std::unique_ptr<GemmJob>> before,after;
    // Keep initialization-only casts and their descriptors alive through submission.
    std::vector<std::unique_ptr<GemmJob>> setup;
};
struct NativeContext {
#ifdef NR_ATTENTION_PARTITION
    std::string attentionPartitionPolicy="baseline";
#endif
#ifdef NR_C32_COMPACT
    std::string c32CompactPolicy="baseline";
    UINT c32CompactWindows=0;
    ComPtr<ID3D12PipelineState> c32WeightPack,c32ExpandGate,c32Project;
#endif
#ifdef NR_GLOBAL_NORMALIZATION
    std::string globalNormalizationPolicy="baseline";
#endif
#ifdef NR_BRANCH_COMPACT
    bool branchCompact=false;
    UINT branchCompactWindows=0;
    ComPtr<ID3D12RootSignature> branchCompactRoot;
    ComPtr<ID3D12PipelineState> branchWeightPack,branchPublishPack,branchExpandGate,branchProjectSum;
#endif
#ifdef NR_GLOBAL_INT8
    std::string globalInt8Policy="none",globalInt8WeightRoot;
    std::map<std::string,float> globalInt8Scales;
    UINT globalInt8Jobs=0;
    bool globalInt8Segment=false;
    UINT globalInt8Segments=0;
    ComPtr<ID3D12PipelineState> globalInt8GatePack;
    ComPtr<ID3D12RootSignature> globalInt8Root;
    ComPtr<ID3D12PipelineState> globalInt8Quantize,globalInt8Product,globalInt8Publish;
#endif
#ifdef NR_GRAPH_DIAGNOSTICS
    std::function<void(const std::string&,Tensor*)> capture;
    std::string diagnosticNode;
    UINT diagnosticJob=0;
    UINT diagnosticShader=0;
#endif
#ifdef NR_EXPERIMENTAL_LINALG
#ifdef NR_DENSE_WAVE
    std::string denseWavePolicy="baseline";
    UINT denseWaveJobs=0;
    ComPtr<ID3D12RootSignature> denseWaveRoot,denseWavePackRoot;
    ComPtr<ID3D12PipelineState> denseWavePipeline,denseWavePack;
#endif
#ifdef NR_DENSE_TILE
    UINT denseTileN=64;
#endif
#ifdef NR_NORM_WAVE
    std::string normWavePolicy="baseline";
#endif
#ifdef NR_SOFTMAX_WAVE
    std::string softmaxWavePolicy="baseline";
#endif
#ifdef NR_ATTENTION_REGISTERS
    std::string attentionRegistersPolicy="baseline";
#endif
#ifdef NR_C32_FULL_FFN
    std::string c32FullFfnPolicy="baseline";
    UINT c32FullFfnJobs=0;
#endif
#ifdef NR_WINDOW_SEGMENT
    std::string windowSegmentPolicy="baseline";
    UINT windowFeedReused=0,windowC32Fused=0;
    ComPtr<ID3D12RootSignature> windowC32Root;
    ComPtr<ID3D12PipelineState> windowC32Project;
#endif
#ifdef NR_C32_PROJECTION
    std::string c32ProjectionPolicy="baseline";
    UINT c32QkvJobs=0,c32ContextJobs=0;
    ComPtr<ID3D12RootSignature> c32ProjectionRoot,c32ProjectionPackRoot;
    ComPtr<ID3D12PipelineState> c32ProjectionPipeline,c32ProjectionPack;
#endif
    bool linAlg=false;
    bool linAlgQkvOnly=false;
    bool linAlgFfnOnly=false;
    bool linAlgGrouped=false;
    bool linAlgAttention=false;
    bool linAlgAttentionInline=false;
    bool linAlgWindowFused=false;
    bool linAlgWindowNormalizeFused=false;
    UINT64 linAlgFusedWindows=0;
    ComPtr<ID3D12RootSignature> linAlgFusedWindowRoot;
    ComPtr<ID3D12PipelineState> linAlgFusedWindowPipeline;
    UINT64 linAlgAttentionJobs=0;
    ComPtr<ID3D12RootSignature> linAlgWaveRoot,linAlgDynamicPackRoot;
    ComPtr<ID3D12PipelineState> linAlgWavePipeline,linAlgDynamicPackPipeline;
    ComPtr<ID3D12PipelineState> linAlgInlinePipeline;
    UINT64 linAlgGroupedJobs=0;
    UINT64 linAlgJobs=0,linAlgWeightBytes=0;
    ComPtr<ID3D12RootSignature> linAlgRootSignature,linAlgPackRoot;
    ComPtr<ID3D12PipelineState> linAlgPipeline,linAlgPackPipeline;
    ComPtr<ID3D12RootSignature> linAlgGroupedRoot,linAlgGroupedPackRoot;
    ComPtr<ID3D12PipelineState> linAlgGrouped32,linAlgGrouped64,linAlgGroupedPack;
#endif
    std::shared_ptr<NativeGpuProfile> profiler;
    UINT tileBatch=1; // Opt-in standalone graph experiment; production stays B=1.
    bool allowHalfPrecision=false;
    bool tensorHalfPrecision=false;
    bool prepackHalfWeights=false;
    bool directComputeGemm=false;
    bool convHalf=false;
    UINT64 prepackedHalfWeights=0;
    UINT64 tensorHalfGemmJobs=0;
    NativeContext(){char flag[4]{};allowHalfPrecision=GetEnvironmentVariableA("NR_DML_ALLOW_HALF",flag,sizeof(flag))==1&&flag[0]=='1';tensorHalfPrecision=GetEnvironmentVariableA("NR_DML_TENSOR_HALF",flag,sizeof(flag))==1&&flag[0]=='1';prepackHalfWeights=GetEnvironmentVariableA("NR_DML_PREPACK_HALF",flag,sizeof(flag))==1&&flag[0]=='1';directComputeGemm=GetEnvironmentVariableA("NR_DML_DIRECT_COMPUTE",flag,sizeof(flag))==1&&flag[0]=='1';convHalf=GetEnvironmentVariableA("NR_DML_CONV_HALF",flag,sizeof(flag))==1&&flag[0]=='1';if(convHalf){tensorHalfPrecision=true;prepackHalfWeights=true;}}
    NativeContext(const NativeContext&)=delete;
    NativeContext& operator=(const NativeContext&)=delete;
    ComPtr<ID3D12Device> device;
    ComPtr<IDMLDevice> dml;
    ComPtr<IDMLCommandRecorder> recorder;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> diagnostics;
    HANDLE event=nullptr;
    UINT64 serial=0;
    UINT64 defaultBufferBytes=0,defaultBufferLimit=~UINT64(0);
    bool reuseScratch=false;
    bool acceptedGameGlobalPolicy=false;
    bool externalRecording=false;
    UINT64 tensorScratchReuses=0,dmlScratchReuses=0;
    std::map<std::string,std::unique_ptr<Tensor>> tensorScratch;
    std::map<UINT64,ComPtr<ID3D12Resource>> dmlScratch;
    ~NativeContext(){if(event)CloseHandle(event);}
    static std::shared_ptr<NativeContext> onDevice(ID3D12Device* device){
        if(!device)throw std::runtime_error("External device missing");
        auto result=std::make_shared<NativeContext>();result->device=device;
        check(DMLCreateDevice(device,DML_CREATE_DEVICE_FLAG_NONE,IID_PPV_ARGS(&result->dml)),"External DirectML device");
        check(result->dml->CreateCommandRecorder(IID_PPV_ARGS(&result->recorder)),"External DirectML recorder");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&result->queue)),"Initialization queue");
        check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&result->allocator)),"Initialization allocator");
        check(device->CreateCommandList(0,q.Type,result->allocator.Get(),nullptr,IID_PPV_ARGS(&result->cmd)),"Initialization list");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&result->fence)),"Initialization fence");
        result->event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!result->event)throw std::runtime_error("Initialization event");
        device->QueryInterface(IID_PPV_ARGS(&result->diagnostics));return result;
    }
};
// Only record() is permitted while borrowed. The caller owns submission/fences.
class ExternalCommandRecording {
    std::shared_ptr<NativeContext> context;
    ComPtr<ID3D12GraphicsCommandList> saved;
public:
    ExternalCommandRecording(std::shared_ptr<NativeContext> existing,ID3D12GraphicsCommandList* list):context(existing){
        if(!context||!list||context->externalRecording)throw std::runtime_error("Invalid or nested external recording");
        if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE)throw std::runtime_error("External list must support compute");
        ComPtr<ID3D12Device> owner;check(list->GetDevice(IID_PPV_ARGS(&owner)),"External list device");
        if(!sameNativeDevice(owner.Get(),context->device.Get()))throw std::runtime_error("External list device mismatch");
        ComPtr<ID3D12GraphicsCommandList> nativeList;auto identity=nativeObjectIdentity(list);check(identity.As(&nativeList),"Native external list");
        saved=context->cmd;context->cmd=nativeList;context->externalRecording=true;
    }
    ExternalCommandRecording(const ExternalCommandRecording&)=delete;
    ExternalCommandRecording& operator=(const ExternalCommandRecording&)=delete;
    ~ExternalCommandRecording(){context->cmd=saved;context->externalRecording=false;}
};
class NativeRuntime {
protected:
    std::shared_ptr<NativeContext> context;
    ComPtr<ID3D12Device>& device=context->device;
    ComPtr<IDMLDevice>& dml=context->dml;
    ComPtr<IDMLCommandRecorder>& recorder=context->recorder;
    ComPtr<ID3D12CommandQueue>& queue=context->queue;
    ComPtr<ID3D12CommandAllocator>& allocator=context->allocator;
    ComPtr<ID3D12GraphicsCommandList>& cmd=context->cmd;
    ComPtr<ID3D12Fence>& fence=context->fence;
    ComPtr<ID3D12InfoQueue>& diagnostics=context->diagnostics;
    HANDLE& event=context->event;
    UINT64& serial=context->serial;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    std::vector<std::unique_ptr<Tensor>> tensors;
    std::vector<std::unique_ptr<GemmJob>> jobs;
public:
    void appendTrackedStates(std::vector<NativeTrackedState>& states){for(auto& tensor:tensors)states.push_back({tensor->resource.Get(),&tensor->state});}
    explicit NativeRuntime(std::shared_ptr<NativeContext> existing=nullptr) : context(existing ? existing : std::make_shared<NativeContext>()){
        if(existing){if(!device || !cmd)throw std::runtime_error("Uninitialized shared context");if(context->externalRecording)throw std::runtime_error("Initialization is forbidden during external recording");return;}
        char debugSetting[4]{};
        const bool debugEnabled=GetEnvironmentVariableA("NR_D3D12_DEBUG",debugSetting,sizeof(debugSetting))==1 && debugSetting[0]=='1';
        if(debugEnabled){
#ifndef NR_GAME_FACTORY_PROBE
            ComPtr<ID3D12Debug> debug;check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer");debug->EnableDebugLayer();
#endif
        }
        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index=0;;++index) {
            ComPtr<IDXGIAdapter1> candidate;
            auto hr=factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if(hr==DXGI_ERROR_NOT_FOUND) break;
            check(hr,"Enumerate adapter"); DXGI_ADAPTER_DESC1 desc{}; check(candidate->GetDesc1(&desc),"Adapter description");
            if(desc.VendorId==0x8086 && desc.DeviceId==0xe20b) {adapter=candidate;break;}
        }
        if(!adapter) throw std::runtime_error("B580 not found");
#if defined(NR_EXPERIMENTAL_LINALG) && !defined(NR_GAME_FACTORY_PROBE)
        // The feature must be enabled before creating the isolated device.
        UUID features[]={D3D12ExperimentalShaderModels};
        check(D3D12EnableExperimentalFeatures(1,features,nullptr,nullptr),"LinAlg experimental features");
#endif
#ifdef NR_GAME_FACTORY_PROBE
        HRESULT factoryResult;
        if(!NativeNrCreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),reinterpret_cast<void**>(device.GetAddressOf()),&factoryResult))throw std::runtime_error("Factory did not handle B580");
        check(factoryResult,"Private SDK721 device");
#else
        check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"D3D12 device");
#endif
        if(debugEnabled)check(device.As(&diagnostics),"D3D12 diagnostics");
        check(DMLCreateDevice(device.Get(),DML_CREATE_DEVICE_FLAG_NONE,IID_PPV_ARGS(&dml)),"DirectML device");
        check(dml->CreateCommandRecorder(IID_PPV_ARGS(&recorder)),"DirectML recorder");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
        check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");
        check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)),"Command list");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event) throw std::runtime_error("Completion event");
    }
    std::shared_ptr<NativeContext> sharedContext() const{return context;}
    ComPtr<ID3D12Resource> defaultBuffer(UINT64 bytes){
        UINT64 allocated=(bytes+65535)/65536*65536;
        if(allocated>context->defaultBufferLimit || context->defaultBufferBytes>context->defaultBufferLimit-allocated)
            throw std::runtime_error("Graph buffers exceed available GPU budget: requested="+std::to_string(allocated)+", allocated="+std::to_string(context->defaultBufferBytes)+", limit="+std::to_string(context->defaultBufferLimit));
        auto resource=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        context->defaultBufferBytes+=allocated;return resource;
    }
    void constrainToGpuBudget(){
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Budget factory");
        ComPtr<IDXGIAdapter3> adapter;check(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"Budget adapter");
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};check(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info),"GPU budget");
        UINT64 available=info.Budget>info.CurrentUsage ? info.Budget-info.CurrentUsage : 0;
        context->defaultBufferLimit=context->defaultBufferBytes+available*4/5;
    }
    ComPtr<ID3DBlob> compileBatchedShader(const char* file,const char* entry,const D3D_SHADER_MACRO* options=nullptr){
        std::vector<D3D_SHADER_MACRO> macros;if(options)for(auto p=options;p->Name;++p)macros.push_back(*p);
        macros.push_back({"NR_TILE_BATCH",context->tileBatch>1?"1":"0"});macros.push_back({nullptr,nullptr});
        return compileNativeShader(file,entry,macros.data());
    }
    void batchRoot(D3D12_ROOT_PARAMETER& p){p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants.ShaderRegister=15;p.Constants.Num32BitValues=6;}
    void bindBatch(UINT slot,std::initializer_list<Tensor*> buffers){UINT stride[6]{};UINT i=0;
        for(auto t:buffers){if(i>=6)throw std::runtime_error("Too many batch bindings");
            if(t&&t->lanes>1){if(t->elements>~UINT(0))throw std::runtime_error("Batch stride exceeds UINT");stride[i]=UINT(t->elements);}++i;}
        cmd->SetComputeRoot32BitConstants(slot,6,stride,0);
    }
    Tensor* make(UINT64 count,bool batched=true) {
        auto t=std::make_unique<Tensor>(); t->elements=count;t->lanes=batched?context->tileBatch:1;
        t->resource=defaultBuffer(count*4*t->lanes);
        auto result=t.get(); tensors.push_back(std::move(t)); return result;
    }
    Tensor* makeScratch(const char* role,UINT64 count){
        if(!context->reuseScratch)return make(count);
        std::string key=std::string(role)+":"+std::to_string(count)+":B"+std::to_string(context->tileBatch);
        auto hit=context->tensorScratch.find(key);
        if(hit!=context->tensorScratch.end()){++context->tensorScratchReuses;return hit->second.get();}
        auto tensor=std::make_unique<Tensor>();tensor->elements=count;tensor->lanes=context->tileBatch;tensor->resource=defaultBuffer(count*4*tensor->lanes);
        auto result=tensor.get();context->tensorScratch.emplace(key,std::move(tensor));return result;
    }
    ComPtr<ID3D12Resource> makeDmlScratch(UINT64 bytes){
        if(!context->reuseScratch)return defaultBuffer(bytes);
        auto hit=context->dmlScratch.find(bytes);
        if(hit!=context->dmlScratch.end()){++context->dmlScratchReuses;return hit->second;}
        auto resource=defaultBuffer(bytes);context->dmlScratch.emplace(bytes,resource);return resource;
    }
    Tensor* load(const std::string& folder,const char* name,UINT64 count) {
        if(context->externalRecording)throw std::runtime_error("Fixture upload is forbidden during external recording");
        std::ifstream file(nativePath(folder+"/"+name+".bin"),std::ios::binary|std::ios::ate);
        if(!file || file.tellg()!=std::streamoff(count*4)) throw std::runtime_error(std::string("Invalid fixture tensor: ")+name);
        file.seekg(0); std::vector<float> values(static_cast<size_t>(count));
        if(!file.read(reinterpret_cast<char*>(values.data()),std::streamsize(count*4))) throw std::runtime_error("Fixture read failed");
        auto t=make(count,false); auto upload=buffer(device.Get(),count*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr; D3D12_RANGE empty{0,0}; check(upload->Map(0,&empty,&mapped),"Upload map");
        memcpy(mapped,values.data(),static_cast<size_t>(count*4)); upload->Unmap(0,nullptr);
        t->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST); cmd->CopyBufferRegion(t->resource.Get(),0,upload.Get(),0,count*4);
        t->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS); uploads.push_back(upload); return t;
    }
    Tensor* loadWeight(const std::string& folder,const char* name,UINT64 count){
        auto weight=load(folder,name,count);weight->immutableWeight=true;
#ifdef NR_GLOBAL_INT8
        auto path=nativePath(folder);
        if(path.parent_path().filename()=="bottleneck"&&(std::string(name)=="expand"||std::string(name)=="ffn_projection"))
            weight->int8WeightKey=path.filename().string()+"-"+name;
#endif
        return weight;
    }
#ifdef NR_GLOBAL_INT8
    bool addGlobalInt8(GemmJob& job,UINT m,UINT n,UINT k){
        if(context->globalInt8Policy=="none"||job.b->int8WeightKey.empty())return false;
        bool expand=(n==4096&&k==1024),contract=(n==1024&&k==4096);
        bool selected=context->globalInt8Policy=="both"||(expand&&context->globalInt8Policy=="expand")||(contract&&context->globalInt8Policy=="contract");
        if(!selected)return false;
        if(!(expand||contract)||context->tileBatch!=1||m%8||n%16||k%32||(m!=96&&m!=192)||job.a->elements!=UINT64(m)*k||job.b->elements!=UINT64(k)*n||job.out->elements!=UINT64(m)*n)throw std::runtime_error("Global INT8 FFN geometry mismatch");
        auto scaleIt=context->globalInt8Scales.find(job.b->int8WeightKey);
        if(scaleIt==context->globalInt8Scales.end()||!std::isfinite(scaleIt->second)||scaleIt->second<=0)throw std::runtime_error("Missing FFN activation calibration");
        if(!context->globalInt8Root){
            D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT caps{};
            caps.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
            caps.WaveMatrixMultiply.Inputs={16,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32};
            check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&caps,sizeof(caps)),"Global INT8 capability");
            if(!(UINT(caps.WaveMatrixMultiply.SupportFlags)&1)||(UINT(caps.WaveMatrixMultiply.SupportFlags)&6)||!caps.WaveMatrixMultiply.NumShapes||caps.WaveMatrixMultiply.NumShapes>256)throw std::runtime_error("Global native INT8 unavailable");
            std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(caps.WaveMatrixMultiply.NumShapes);caps.WaveMatrixMultiply.Shapes=shapes.data();
            check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&caps,sizeof(caps)),"Global INT8 shapes");
            if(!std::any_of(shapes.begin(),shapes.end(),[](auto s){return s.M==8&&s.K==32&&s.N==16;}))throw std::runtime_error("Global INT8 shape unavailable");
            D3D12_ROOT_PARAMETER p[5]{};
            for(UINT i=0;i<4;++i){p[i].ParameterType=i<2?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV;p[i].Descriptor.ShaderRegister=i%2;}
            p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[4].Constants.Num32BitValues=6;
            D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=5;desc.pParameters=p;ComPtr<ID3DBlob> blob,error;
            check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Global INT8 root serialize");
            check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&context->globalInt8Root)),"Global INT8 root");
            context->globalInt8Quantize=linAlgPso(device.Get(),context->globalInt8Root.Get(),"shape-Quantize.dxil");
            context->globalInt8Product=linAlgPso(device.Get(),context->globalInt8Root.Get(),"shape-Product.dxil");
            context->globalInt8Publish=linAlgPso(device.Get(),context->globalInt8Root.Get(),"shape-Publish.dxil");
        }
        auto p=std::make_unique<GemmJob::Int8Projection>();p->m=m;p->n=n;p->k=k;p->scale=scaleIt->second;
        // Owned by this runtime, never alias graph scratch; tracked for closed-list replay.
        p->packedA=make(UINT64(m)*k/4,false);p->packedB=make(UINT64(k)*n/4+n,false);p->scratch=make(UINT64(m)*n,false);
        UINT64 bytes=p->packedB->elements*4;
        std::ifstream f(nativePath(context->globalInt8WeightRoot+"/"+job.b->int8WeightKey+"/packed_b.bin"),std::ios::binary|std::ios::ate);
        if(!f||f.tellg()!=std::streamoff(bytes))throw std::runtime_error("Global INT8 packed weight size");
        auto upload=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* data;D3D12_RANGE empty{0,0};
        check(upload->Map(0,&empty,&data),"Global INT8 weight upload");f.seekg(0);f.read(static_cast<char*>(data),std::streamsize(bytes));
        if(!f){upload->Unmap(0,nullptr);throw std::runtime_error("Global INT8 weight read");}
        const float* scales=reinterpret_cast<const float*>(static_cast<const char*>(data)+UINT64(k)*n);
        for(UINT column=0;column<n;++column)if(!std::isfinite(scales[column])||scales[column]<=0){upload->Unmap(0,nullptr);throw std::runtime_error("Global INT8 weight scale");}
        upload->Unmap(0,nullptr);p->packedB->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(p->packedB->resource.Get(),0,upload.Get(),0,bytes);p->packedB->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uploads.push_back(upload);job.int8=std::move(p);job.profileName+="_int8_staged";++context->globalInt8Jobs;return true;
    }
    void recordGlobalInt8(GemmJob& job){
        auto& p=*job.int8;
        auto dispatch=[&](ID3D12PipelineState* pipeline,Tensor* input,Tensor* scratch,UINT x,UINT y){
            input->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);p.packedB->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            scratch->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);job.out->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(context->globalInt8Root.Get());cmd->SetPipelineState(pipeline);
            cmd->SetComputeRootShaderResourceView(0,input->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,p.packedB->resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(2,scratch->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(3,job.out->resource->GetGPUVirtualAddress());
            UINT shape[]={p.m,p.n,p.k,0,0,0};float inv=1.f/p.scale;memcpy(shape+4,&inv,4);memcpy(shape+5,&p.scale,4);
            cmd->SetComputeRoot32BitConstants(4,6,shape,0);cmd->Dispatch(x,y,1);uavBarrier(cmd.Get());
        };
        if(!p.preparedInput){NativeProfileRange r(context->profiler.get(),cmd.Get(),"int8_quantize");UINT groups=(p.m*p.k/4+63)/64;dispatch(context->globalInt8Quantize.Get(),job.a,p.packedA,std::min(groups,4096u),(groups+4095)/4096);}
        {NativeProfileRange r(context->profiler.get(),cmd.Get(),"int8_product");dispatch(context->globalInt8Product.Get(),p.packedA,p.scratch,p.m/8,p.n/16);}
        if(!p.compactOutput){NativeProfileRange r(context->profiler.get(),cmd.Get(),"int8_publish");dispatch(context->globalInt8Publish.Get(),p.packedA,p.scratch,p.m/8,p.n/16);}
    }
    void recordInt8GatePack(GemmJob& expansion,GemmJob& contraction){
        auto& a=*expansion.int8;auto& b=*contraction.int8;
        if(!a.compactOutput||!b.preparedInput||a.m!=b.m||a.n!=b.k)throw std::runtime_error("Invalid compact FFN segment");
        NativeProfileRange range(context->profiler.get(),cmd.Get(),"Int8GatePack");
        a.scratch->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);a.packedB->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        b.packedA->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);contraction.out->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(context->globalInt8Root.Get());cmd->SetPipelineState(context->globalInt8GatePack.Get());
        cmd->SetComputeRootShaderResourceView(0,a.scratch->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,a.packedB->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2,b.packedA->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(3,contraction.out->resource->GetGPUVirtualAddress());
        UINT shape[]={b.m,b.n,b.k,a.k,0,0};float inverse=1.f/b.scale;memcpy(shape+4,&inverse,4);memcpy(shape+5,&a.scale,4);
        cmd->SetComputeRoot32BitConstants(4,6,shape,0);UINT groups=(b.m*b.k/4+63)/64;
        cmd->Dispatch(std::min(groups,4096u),(groups+4095)/4096,1);uavBarrier(cmd.Get());
    }
#endif
    // FLOAT16 views are confined to the GEMM experiment. Existing shaders still use FLOAT32.
    // Conversions remain on the caller's GPU command list and are included in profiling.
    void initializeJob(GemmJob* job,std::vector<DML_BUFFER_BINDING> bindings,DML_BUFFER_BINDING outputBindingBuffer,bool ownedB=false){
        IDMLCompiledOperator* operators[]={job->op.Get()};
        check(dml->CreateOperatorInitializer(1,operators,IID_PPV_ARGS(&job->initializer)),"GEMM initializer");
        auto ip=job->initializer->GetBindingProperties(),ep=job->op->GetBindingProperties();
        UINT initCount=std::max(1u,ip.RequiredDescriptorCount),execCount=std::max(1u,ep.RequiredDescriptorCount);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hd.NumDescriptors=initCount+execCount;
        check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&job->heap)),"GEMM descriptors");
        DML_BINDING_TABLE_DESC td{job->initializer.Get(),job->heap->GetCPUDescriptorHandleForHeapStart(),job->heap->GetGPUDescriptorHandleForHeapStart(),initCount};
        check(dml->CreateBindingTable(&td,IID_PPV_ARGS(&job->initTable)),"Initializer table");
        UINT increment=device->GetDescriptorHandleIncrementSize(hd.Type);td.Dispatchable=job->op.Get();td.SizeInDescriptors=execCount;
        td.CPUDescriptorHandle.ptr+=SIZE_T(initCount)*increment;td.GPUDescriptorHandle.ptr+=UINT64(initCount)*increment;
        check(dml->CreateBindingTable(&td,IID_PPV_ARGS(&job->table)),"GEMM table");
        UINT64 scratchSize=std::max(ip.TemporaryResourceSize,ep.TemporaryResourceSize);
        if(scratchSize) job->scratch=makeDmlScratch(scratchSize);
        if(ep.PersistentResourceSize) job->persistent=defaultBuffer(ep.PersistentResourceSize);
        DML_BUFFER_BINDING scratch{job->scratch.Get(),0,scratchSize},persistent{job->persistent.Get(),0,ep.PersistentResourceSize};
        DML_BINDING_DESC sd{DML_BINDING_TYPE_BUFFER,&scratch},pd{job->persistent ? DML_BINDING_TYPE_BUFFER : DML_BINDING_TYPE_NONE,job->persistent ? &persistent : nullptr};
        if(ip.TemporaryResourceSize)job->initTable->BindTemporaryResource(&sd);
        if(ep.TemporaryResourceSize)job->table->BindTemporaryResource(&sd);
        job->initTable->BindOutputs(1,&pd);if(job->persistent)job->table->BindPersistentResource(&pd);
        if(ownedB){
            DML_BUFFER_BINDING ownedInputs[3]{};ownedInputs[1]=bindings.at(1);
            DML_BUFFER_ARRAY_BINDING array{3,ownedInputs};DML_BINDING_DESC input{DML_BINDING_TYPE_BUFFER_ARRAY,&array};
            job->initTable->BindInputs(1,&input);bindings[1]={nullptr,0,0};
        }
        std::vector<DML_BINDING_DESC> inputs;
        for(auto& binding:bindings)inputs.push_back({binding.Buffer?DML_BINDING_TYPE_BUFFER:DML_BINDING_TYPE_NONE,binding.Buffer?&binding:nullptr});
        DML_BINDING_DESC outputBinding{DML_BINDING_TYPE_BUFFER,&outputBindingBuffer};
        job->table->BindInputs(static_cast<UINT>(inputs.size()),inputs.data());job->table->BindOutputs(1,&outputBinding);
        ID3D12DescriptorHeap* heaps[]={job->heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
        recorder->RecordDispatch(cmd.Get(),job->initializer.Get(),job->initTable.Get());uavBarrier(cmd.Get());
    }
    std::unique_ptr<GemmJob> castJob(Tensor* input,Tensor* output,UINT64 count,bool toHalf,UINT64 inputOffset=0){
        if(count>~UINT(0))throw std::runtime_error("Cast tensor is too large");
        auto job=std::make_unique<GemmJob>();job->a=job->b=input;job->out=output;
        job->profileName=toHalf?"pack_fp16":"unpack_fp16";
        UINT sizes[]={1,1,1,static_cast<UINT>(count)};
        const UINT64 halfBytes=(count*2+3)&~UINT64(3),floatBytes=count*4;
        if(inputOffset+(toHalf?floatBytes:halfBytes)>input->elements*4 || (toHalf?halfBytes:floatBytes)>output->elements*4)
            throw std::runtime_error("FP16 cast tensor view exceeds its buffer");
        DML_BUFFER_TENSOR_DESC ib{toHalf?DML_TENSOR_DATA_TYPE_FLOAT32:DML_TENSOR_DATA_TYPE_FLOAT16,DML_TENSOR_FLAG_NONE,4,sizes,nullptr,toHalf?floatBytes:halfBytes,0};
        auto ob=ib;ob.DataType=toHalf?DML_TENSOR_DATA_TYPE_FLOAT16:DML_TENSOR_DATA_TYPE_FLOAT32;ob.TotalTensorSizeInBytes=toHalf?halfBytes:floatBytes;
        DML_TENSOR_DESC it{DML_TENSOR_TYPE_BUFFER,&ib},ot{DML_TENSOR_TYPE_BUFFER,&ob};
        DML_CAST_OPERATOR_DESC cast{&it,&ot};DML_OPERATOR_DESC desc{DML_OPERATOR_CAST,&cast};ComPtr<IDMLOperator> op;
        check(dml->CreateOperator(&desc,IID_PPV_ARGS(&op)),"FP16 cast creation");
        check(dml->CompileOperator(op.Get(),DML_EXECUTION_FLAG_NONE,IID_PPV_ARGS(&job->op)),"FP16 cast compilation");
        initializeJob(job.get(),{{input->resource.Get(),inputOffset,ib.TotalTensorSizeInBytes}},{output->resource.Get(),0,ob.TotalTensorSizeInBytes});
        return job;
    }
    void recordGemm(GemmJob& job){
#ifdef NR_EXPERIMENTAL_LINALG
        if(job.fusedPlaceholder)throw std::runtime_error("Fused window GEMM placeholder must not dispatch");
#endif
#ifdef NR_GRAPH_DIAGNOSTICS
        std::string label=context->diagnosticNode+"_gemm"+std::to_string(context->diagnosticJob++);
        if(context->capture){printf("DIAG_JOB %s %s\n",label.c_str(),job.profileName.c_str());context->capture(label+"_a",job.a);context->capture(label+"_w",job.b);}
#endif
        NativeProfileRange profile(context->profiler.get(),cmd.Get(),job.profileName.c_str());
#ifdef NR_GLOBAL_INT8
        if(job.int8){recordGlobalInt8(job);
#ifdef NR_GRAPH_DIAGNOSTICS
            if(context->capture)context->capture(label+"_out",job.out);
#endif
            return;
        }
#endif
#ifdef NR_EXPERIMENTAL_LINALG
        if(job.wave){
            auto& p=*job.wave;
            if(p.inlinePreparation){
                NativeProfileRange fused(context->profiler.get(),cmd.Get(),"wave_inline_fp16");
                job.a->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);job.b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                job.out->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmd->SetComputeRootSignature(p.root.Get());cmd->SetPipelineState(p.pipeline.Get());
                cmd->SetComputeRootShaderResourceView(0,job.a->resource->GetGPUVirtualAddress()+p.aOffset);
                cmd->SetComputeRootShaderResourceView(1,job.b->resource->GetGPUVirtualAddress()+p.bOffset);
                cmd->SetComputeRootUnorderedAccessView(2,job.out->resource->GetGPUVirtualAddress());
                UINT shape[]={p.m,p.n,p.k,p.groups,job.a->lanes==1?0u:UINT(job.a->elements),job.b->lanes==1?0u:UINT(job.b->elements),UINT(p.transposeB),p.lanes};
                cmd->SetComputeRoot32BitConstants(3,8,shape,0);cmd->Dispatch((p.m+7)/8,(p.n+15)/16,p.groups*p.lanes);uavBarrier(cmd.Get());
#ifdef NR_GRAPH_DIAGNOSTICS
                if(context->capture)context->capture(label+"_out",job.out);
#endif
                return;
            }
            {
                NativeProfileRange prepare(context->profiler.get(),cmd.Get(),"wave_prepare_fp16");
                auto pack=[&](Tensor* source,Tensor* target,UINT sourceRows,UINT sourceColumns,UINT rows,UINT columns,bool transpose,UINT64 offset){
                    source->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);target->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    cmd->SetComputeRootSignature(p.packRoot.Get());cmd->SetPipelineState(p.packPipeline.Get());
                    cmd->SetComputeRootShaderResourceView(0,source->resource->GetGPUVirtualAddress()+offset);
                    cmd->SetComputeRootUnorderedAccessView(1,target->resource->GetGPUVirtualAddress());
                    UINT shape[]={sourceRows,sourceColumns,rows,columns,p.groups,source->lanes==1?0u:UINT(source->elements),UINT(transpose),p.groups*rows*columns/2};
                    cmd->SetComputeRoot32BitConstants(2,8,shape,0);UINT groups=(shape[7]+127)/128;
                    cmd->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,p.lanes);
                    target->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                };
                pack(job.a,p.packedA,p.m,p.k,p.mp,p.kp,false,p.aOffset);
                pack(job.b,p.packedB,p.transposeB?p.n:p.k,p.transposeB?p.k:p.n,p.kp,p.np,p.transposeB,p.bOffset);
            }
            {
                NativeProfileRange multiply(context->profiler.get(),cmd.Get(),"wave_multiply_fp16");
                job.out->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmd->SetComputeRootSignature(p.root.Get());cmd->SetPipelineState(p.pipeline.Get());
                cmd->SetComputeRootShaderResourceView(0,p.packedA->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,p.packedB->resource->GetGPUVirtualAddress());
                cmd->SetComputeRootUnorderedAccessView(2,job.out->resource->GetGPUVirtualAddress());
                UINT shape[]={p.m,p.n,p.k,p.mp,p.np,p.kp,p.groups,p.lanes};cmd->SetComputeRoot32BitConstants(3,8,shape,0);
                cmd->Dispatch((p.m+7)/8,(p.n+15)/16,p.groups*p.lanes);uavBarrier(cmd.Get());
            }
#ifdef NR_GRAPH_DIAGNOSTICS
            if(context->capture)context->capture(label+"_out",job.out);
#endif
            return;
        }
        if(job.native){
            auto& p=*job.native;
#ifdef NR_CLEAN_PROFILE
            bool captureReal=!denseRealCaptured&&GetEnvironmentVariableA("NR_DENSE_REAL_CAPTURE_DIR",nullptr,0)&&p.groups==1&&p.rows>=128&&p.inner>=128&&!p.inputShadow;
            auto captureTensor=[&](Tensor* t,UINT64 bytes,const char* name){auto rb=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);t->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(rb.Get(),0,t->resource.Get(),0,bytes);denseRealCaptures.push_back({rb,bytes,name});};
            if(captureReal){denseRealCaptured=true;denseRealM=p.rows;denseRealN=p.columns;denseRealK=p.inner;captureTensor(job.a,UINT64(p.rows)*p.inner*4,"input.bin");captureTensor(job.b,UINT64(p.inner)*p.columns*4,"weights.bin");}
            if(p.inputShadow&&GetEnvironmentVariableA("NR_STUDY_FP16_DENSE_AUDIT",nullptr,0)&&denseShadowAuditRecorded<148){
                auto rb=buffer(device.Get(),job.a->elements*6,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
                job.a->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(rb.Get(),0,job.a->resource.Get(),0,job.a->elements*6);
                denseShadowAudits.push_back({rb,job.a->elements,job.profileName});++denseShadowAuditRecorded;
            }
#endif
#ifdef NR_GRAPH_DIAGNOSTICS
            if(context->capture){Tensor packed;packed.resource=p.packed;packed.elements=UINT64(p.groups)*p.columns*p.inner/2;packed.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;context->capture(label+"_packed",&packed);}
#endif
            job.a->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            job.out->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(p.root.Get());cmd->SetPipelineState(p.pipeline.Get());
            cmd->SetComputeRootShaderResourceView(0,p.optimal->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(1,job.a->resource->GetGPUVirtualAddress());
#ifdef NR_CLEAN_PROFILE
            if(p.inputShadow){
                if(!job.a->denseHalfShadow||job.a->lanes!=1)throw std::runtime_error("Dense half producer missing");
                cmd->SetComputeRootShaderResourceView(1,job.a->resource->GetGPUVirtualAddress()+job.a->elements*4);
            }
#endif
            cmd->SetComputeRootUnorderedAccessView(2,job.out->resource->GetGPUVirtualAddress());
            UINT shape[]={p.rows,p.columns,p.inner,p.stride,p.groups,p.lanes};
            cmd->SetComputeRoot32BitConstants(3,p.groups>1?6:4,shape,0);
#ifdef NR_DENSE_WAVE
            if(p.waveDense){
#ifdef NR_CLEAN_PROFILE
                if(p.reuseRows16)cmd->Dispatch((p.rows+15)/16,p.columns/16,1);else
                if(context->denseWavePolicy=="register32")cmd->Dispatch((p.rows+15)/16,p.columns/32,1);else
#endif
                cmd->Dispatch((p.rows+7)/8,p.columns/64,1);
            }else
#endif
            cmd->Dispatch((p.rows+31)/32,p.columns/p.tile,p.groups>1?p.groups*p.lanes:1);uavBarrier(cmd.Get());
#ifdef NR_CLEAN_PROFILE
            if(captureReal)captureTensor(job.out,UINT64(p.rows)*p.columns*4,"original.bin");
            if(p.inputShadow&&!p.reuseRows16&&GetEnvironmentVariableA("NR_DENSE_INLINE_COMPARE",nullptr,0)&&denseInlineCount<148){
                UINT count=p.rows*p.columns;UINT64 bytes=UINT64(count)*4;
                auto ca=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST),re=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
                auto output=defaultBuffer(bytes);job.out->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(ca.Get(),0,job.out->resource.Get(),0,bytes);
                cmd->SetComputeRootSignature(context->linAlgRootSignature.Get());cmd->SetPipelineState(context->linAlgPipeline.Get());
                cmd->SetComputeRootShaderResourceView(0,p.optimal->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,job.a->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
                UINT constants[]={p.rows,p.columns,p.inner,p.stride};cmd->SetComputeRoot32BitConstants(3,4,constants,0);cmd->Dispatch((p.rows+31)/32,p.columns/32,1);uavBarrier(cmd.Get());
                transition(cmd.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(re.Get(),0,output.Get(),0,bytes);
                denseInlinePairs.push_back({ca,re,output,count,job.profileName});++denseInlineCount;
            }
#endif
#ifdef NR_GRAPH_DIAGNOSTICS
            if(context->capture)context->capture(label+"_out",job.out);
#endif
            return;
        }
#endif
        for(auto& conversion:job.before)recordGemm(*conversion);
        job.a->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);job.b->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);job.out->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]={job.heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
        recorder->RecordDispatch(cmd.Get(),job.op.Get(),job.table.Get());uavBarrier(cmd.Get());
        for(auto& conversion:job.after)recordGemm(*conversion);
#ifdef NR_GRAPH_DIAGNOSTICS
        if(context->capture)context->capture(label+"_out",job.out);
#endif
    }
    void addGemm(Tensor* a,Tensor* b,Tensor* out,UINT m,UINT n,UINT k,UINT batch=1,bool transposeB=false,UINT64 aOffset=0,UINT64 bOffset=0) {
        if(context->externalRecording)throw std::runtime_error("GEMM initialization is forbidden during external recording");
        auto job=std::make_unique<GemmJob>(); job->a=a;job->b=b;job->out=out;
        job->profileName="gemm_"+std::to_string(m)+"x"+std::to_string(n)+"x"+std::to_string(k)+"_batch"+std::to_string(batch);
        UINT lanes=context->tileBatch;
#ifdef NR_GLOBAL_INT8
        if(batch==1&&!transposeB&&!aOffset&&!bOffset&&b->immutableWeight&&addGlobalInt8(*job,m,n,k)){jobs.push_back(std::move(job));return;}
#endif
        if(lanes>1&&(context->tensorHalfPrecision||context->convHalf))throw std::runtime_error("Tile batch precision experiment must remain FP32");
        if(a->lanes!=lanes||out->lanes!=lanes||(b->lanes!=1&&b->lanes!=lanes))throw std::runtime_error("GEMM tile lane mismatch");
        UINT as[]={lanes,batch,m,k},bs[]={lanes,batch,transposeB ? n : k,transposeB ? k : n},os[]={lanes,batch,m,n};
        UINT astride[]={UINT(a->elements),m*k,k,1},bstride[]={b->lanes==1?0u:UINT(b->elements),k*n,bs[3],1},ostride[]={UINT(out->elements),m*n,n,1};
        UINT64 aBytes=UINT64(batch)*m*k*4,bBytes=UINT64(batch)*k*n*4,outBytes=UINT64(batch)*m*n*4;
        if(aOffset+aBytes>a->elements*4 || bOffset+bBytes>b->elements*4 || outBytes>out->elements*4)
            throw std::runtime_error("GEMM tensor view exceeds its buffer");
#ifdef NR_EXPERIMENTAL_LINALG
        const bool dynamicAttention=!b->immutableWeight&&((a==b&&transposeB&&k==32)||(!transposeB&&n==32&&bOffset>0));
        if(context->linAlgAttention&&dynamicAttention&&b->lanes==lanes&&n%16==0&&k%16==0){
            if(!context->linAlgWavePipeline){
                D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT support{};
                support.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
                support.WaveMatrixMultiply.Inputs={16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16};
                check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&support,sizeof(support)),"Dynamic Wave support");
                if(!(support.WaveMatrixMultiply.SupportFlags&D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_SUPPORTED)||!support.WaveMatrixMultiply.NumShapes||support.WaveMatrixMultiply.NumShapes>256)throw std::runtime_error("Dynamic Wave FP16 unsupported");
                std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(support.WaveMatrixMultiply.NumShapes);support.WaveMatrixMultiply.Shapes=shapes.data();
                check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&support,sizeof(support)),"Dynamic Wave shapes");
                if(!std::any_of(shapes.begin(),shapes.end(),[](auto s){return s.M==8&&s.K==16&&s.N==16;}))throw std::runtime_error("Dynamic Wave shape missing");
                context->linAlgWaveRoot=linAlgRoot(device.Get(),false,false,8);context->linAlgDynamicPackRoot=linAlgRoot(device.Get(),true,false,8);
                context->linAlgWavePipeline=linAlgPso(device.Get(),context->linAlgWaveRoot.Get(),"dynamic-wave.dxil");
                context->linAlgDynamicPackPipeline=linAlgPso(device.Get(),context->linAlgDynamicPackRoot.Get(),"dynamic-pack.dxil");
            }
            auto p=std::make_unique<LinAlgWaveGemm>();p->m=m;p->n=n;p->k=k;p->mp=(m+7)/8*8;p->np=(n+15)/16*16;p->kp=(k+15)/16*16;p->groups=batch;p->lanes=lanes;
            p->aOffset=aOffset;p->bOffset=bOffset;p->transposeB=transposeB;
            if((m+7)/8>65535||(n+15)/16>65535||UINT64(batch)*lanes>65535||UINT64(batch)*lanes*p->mp*p->kp>~UINT(0)/2||UINT64(batch)*lanes*p->kp*p->np>~UINT(0)/2)throw std::runtime_error("Dynamic Wave address/dispatch limit");
            p->root=context->linAlgWaveRoot;p->packRoot=context->linAlgDynamicPackRoot;p->pipeline=context->linAlgWavePipeline;p->packPipeline=context->linAlgDynamicPackPipeline;
            p->inlinePreparation=context->linAlgAttentionInline;
            if(p->inlinePreparation){
                if(!context->linAlgInlinePipeline)context->linAlgInlinePipeline=linAlgPso(device.Get(),p->root.Get(),"dynamic-inline.dxil");
                p->pipeline=context->linAlgInlinePipeline;
            }else{
                p->packedA=makeScratch("wave.dynamic.a",UINT64(batch)*p->mp*p->kp/2);p->packedB=makeScratch("wave.dynamic.b",UINT64(batch)*p->kp*p->np/2);
            }
            job->wave=std::move(p);job->profileName+="_wave_dynamic";++context->linAlgAttentionJobs;jobs.push_back(std::move(job));return;
        }
        // Explicit group/lane addressing; never flatten groups with different weights.
        if(context->linAlg&&context->linAlgGrouped&&batch>1&&b->immutableWeight&&b->lanes==1&&!transposeB&&!aOffset&&!bOffset&&
           n%32==0&&k%32==0&&a->elements==UINT64(batch)*m*k&&out->elements==UINT64(batch)*m*n&&b->elements==UINT64(batch)*k*n){
            UINT tile=n%64==0&&k%64==0?64:32;
            if(m>65535u*32||n/tile>65535||UINT64(batch)*lanes>65535)throw std::runtime_error("Grouped LinAlg dispatch limit");
            D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT operation{};
            operation.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY;
            operation.ThreadVectorMatrixMultiply.VectorInputType=operation.ThreadVectorMatrixMultiply.MatrixInputType=
                operation.ThreadVectorMatrixMultiply.BiasInputType=operation.ThreadVectorMatrixMultiply.VectorResultType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
            check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&operation,sizeof(operation)),"Grouped LinAlg capability");
            if(!(operation.ThreadVectorMatrixMultiply.SupportFlags&D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_SUPPORTED))throw std::runtime_error("Grouped LinAlg FP16 unsupported");
            if(!context->linAlgGroupedPack){
                context->linAlgGroupedRoot=linAlgRoot(device.Get(),false,true);context->linAlgGroupedPackRoot=linAlgRoot(device.Get(),true,true);
                context->linAlgGroupedPack=linAlgPso(device.Get(),context->linAlgGroupedPackRoot.Get(),"group-pack.dxil");
            }
            auto& pipeline=tile==32?context->linAlgGrouped32:context->linAlgGrouped64;
            if(!pipeline)pipeline=linAlgPso(device.Get(),context->linAlgGroupedRoot.Get(),tile==32?"group-matrix32.dxil":"group-matrix64.dxil");
            auto p=std::make_unique<LinAlgProjection>();p->rows=m;p->columns=n;p->inner=k;p->groups=batch;p->lanes=lanes;p->tile=tile;
            p->root=context->linAlgGroupedRoot;p->pipeline=pipeline;
            ComPtr<ID3D12DevicePreview> preview;check(device.As(&preview),"Grouped LinAlg device preview");
            D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO dest{};
            dest.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL;dest.NumRows=dest.NumColumns=tile;
            dest.DestDataType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;preview->GetLinearAlgebraMatrixConversionDestinationInfo(&dest);
            if(!dest.DestSize)throw std::runtime_error("Grouped LinAlg optimal size missing");p->stride=(dest.DestSize+255u)&~255u;
            UINT tiles=batch*(n/tile)*(k/tile),packedTileBytes=tile*tile*2;
            p->packed=defaultBuffer(UINT64(batch)*n*k*2);p->optimal=defaultBuffer(UINT64(tiles)*p->stride);
            b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmd->SetComputeRootSignature(context->linAlgGroupedPackRoot.Get());cmd->SetPipelineState(context->linAlgGroupedPack.Get());
            cmd->SetComputeRootShaderResourceView(0,b->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(1,p->packed->GetGPUVirtualAddress());
            UINT shape[]={n,k,batch,tile};cmd->SetComputeRoot32BitConstants(2,4,shape,0);cmd->Dispatch((UINT64(batch)*n*k/2+255)/256,1,1);
            transition(cmd.Get(),p->packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12GraphicsCommandListPreview> list;check(cmd.As(&list),"Grouped LinAlg list preview");
            for(UINT t=0;t<tiles;++t){D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO ci{};ci.DestInfo=dest;
                ci.SrcInfo={packedTileBytes,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,tile*2};
                ci.DataDesc={p->optimal->GetGPUVirtualAddress()+UINT64(t)*p->stride,p->packed->GetGPUVirtualAddress()+UINT64(t)*packedTileBytes};list->ConvertLinearAlgebraMatrix(&ci,1);
            }
            transition(cmd.Get(),p->optimal.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context->linAlgWeightBytes+=UINT64(tiles)*p->stride;++context->linAlgJobs;++context->linAlgGroupedJobs;
            job->native=std::move(p);job->profileName+="_linalg_grouped";jobs.push_back(std::move(job));return;
        }
#ifdef NR_C32_PROJECTION
        const bool c32Selected=b->c32ProjectionRole==1?(context->c32ProjectionPolicy=="qkv"||context->c32ProjectionPolicy=="both"):
            b->c32ProjectionRole==2&&(context->c32ProjectionPolicy=="context"||context->c32ProjectionPolicy=="both");
        if(context->linAlg&&c32Selected){
            if(batch!=1||!b->immutableWeight||b->lanes!=1||transposeB||aOffset||bOffset||k!=32||
                n!=(b->c32ProjectionRole==1?96u:32u)||a->elements!=UINT64(m)*k||out->elements!=UINT64(m)*n||b->elements!=UINT64(k)*n)
                throw std::runtime_error("C32 projection role/shape mismatch");
            if(UINT64(m)*lanes>UINT64(65535)*32)throw std::runtime_error("C32 projection dispatch limit");
            if(!context->c32ProjectionPipeline){
                context->c32ProjectionRoot=linAlgRoot(device.Get(),false);context->c32ProjectionPackRoot=linAlgRoot(device.Get(),true);
                context->c32ProjectionPipeline=linAlgPso(device.Get(),context->c32ProjectionRoot.Get(),"matrix-c32.dxil");
                context->c32ProjectionPack=linAlgPso(device.Get(),context->c32ProjectionPackRoot.Get(),"pack-c32.dxil");
            }
            ComPtr<ID3D12DevicePreview> preview;check(device.As(&preview),"C32 device preview");
            auto p=std::make_unique<LinAlgProjection>();p->rows=m*lanes;p->columns=n;p->inner=k;p->tile=32;
            p->root=context->c32ProjectionRoot;p->pipeline=context->c32ProjectionPipeline;
            D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO dest{};
            dest.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL;dest.NumRows=dest.NumColumns=32;
            dest.DestDataType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;preview->GetLinearAlgebraMatrixConversionDestinationInfo(&dest);
            if(!dest.DestSize)throw std::runtime_error("C32 optimal layout missing");p->stride=(dest.DestSize+255u)&~255u;
            UINT tiles=n/32;p->packed=defaultBuffer(UINT64(n)*k*2);p->optimal=defaultBuffer(UINT64(tiles)*p->stride);
            b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmd->SetComputeRootSignature(context->c32ProjectionPackRoot.Get());cmd->SetPipelineState(context->c32ProjectionPack.Get());
            cmd->SetComputeRootShaderResourceView(0,b->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(1,p->packed->GetGPUVirtualAddress());
            UINT shape[]={n,k};cmd->SetComputeRoot32BitConstants(2,2,shape,0);cmd->Dispatch((n*k/2+255)/256,1,1);
            transition(cmd.Get(),p->packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12GraphicsCommandListPreview> list;check(cmd.As(&list),"C32 list preview");
            for(UINT t=0;t<tiles;++t){D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO ci{};ci.DestInfo=dest;
                ci.SrcInfo={2048,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,64};
                ci.DataDesc={p->optimal->GetGPUVirtualAddress()+UINT64(t)*p->stride,p->packed->GetGPUVirtualAddress()+UINT64(t)*2048};list->ConvertLinearAlgebraMatrix(&ci,1);}
            transition(cmd.Get(),p->optimal.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context->linAlgWeightBytes+=UINT64(tiles)*p->stride;++context->linAlgJobs;
            if(b->c32ProjectionRole==1)++context->c32QkvJobs;else ++context->c32ContextJobs;
            job->native=std::move(p);job->profileName+="_linalg_c32";jobs.push_back(std::move(job));return;
        }
#endif
        // Flatten tile rows only when both activations are dense. Never flatten
        // grouped/head attention, mutable operands, offsets, or transposed views.
        if(context->linAlg&&batch==1&&b->immutableWeight&&b->lanes==1&&!transposeB&&!aOffset&&!bOffset&&
           (!context->linAlgQkvOnly||((k==256||k==1024)&&n==3*k))&&
           (!context->linAlgFfnOnly||((k==1024&&n==4096)||(k==4096&&n==1024)))&&
           n%64==0&&k%64==0&&a->elements==UINT64(m)*k&&out->elements==UINT64(m)*n&&b->elements==UINT64(k)*n){
#ifdef NR_CLEAN_PROFILE
            if(a->denseHalfShadow&&GetEnvironmentVariableA("NR_STUDY_FP16_DENSE_WAVE",nullptr,0)){
                linAlgCheckWaveFP16(device.Get());
                auto p=std::make_unique<LinAlgProjection>();p->rows=m;p->columns=n;p->inner=k;p->tile=16;p->inputShadow=true;p->reuseRows16=p->waveDense=true;
                p->root=linAlgRoot(device.Get(),false);p->pipeline=linAlgPso(device.Get(),p->root.Get(),"fp16dense-wave16.dxil");
                auto packRoot=linAlgRoot(device.Get(),true);auto packPso=linAlgPso(device.Get(),packRoot.Get(),"fp16dense-pack.dxil");
                p->studyPackRoot=packRoot;p->studyPackPipeline=packPso;
                p->packed=defaultBuffer(UINT64(n)*k*2);p->optimal=p->packed;
                b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                cmd->SetComputeRootSignature(packRoot.Get());cmd->SetPipelineState(packPso.Get());
                cmd->SetComputeRootShaderResourceView(0,b->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(1,p->packed->GetGPUVirtualAddress());
                UINT packShape[]={n,k};cmd->SetComputeRoot32BitConstants(2,2,packShape,0);cmd->Dispatch((n*k/2+255)/256,1,1);
                transition(cmd.Get(),p->packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ++context->linAlgJobs;context->linAlgWeightBytes+=UINT64(n)*k*2;
                printf("NR_FP16_DENSE_JOB mode=wave m=%u n=%u k=%u\n",m,n,k);
                job->native=std::move(p);job->profileName+="_fp16_row16";jobs.push_back(std::move(job));return;
            }
#endif
#ifdef NR_DENSE_WAVE
            bool studyRegister=false;
#ifdef NR_CLEAN_PROFILE
            studyRegister=context->denseWavePolicy=="register32"&&m>=128;
#endif
            if(context->denseWavePolicy=="wave64"||studyRegister){
                if(UINT64(m)*lanes>UINT64(65535)*8||n/64>65535)throw std::runtime_error("Dense Wave dispatch limit");
                if(!context->denseWavePipeline){
                    linAlgCheckWaveFP16(device.Get());context->denseWaveRoot=linAlgRoot(device.Get(),false);context->denseWavePackRoot=linAlgRoot(device.Get(),true);
                    context->denseWavePipeline=linAlgPso(device.Get(),context->denseWaveRoot.Get(),studyRegister?"dense-register32.dxil":"dense-wave64.dxil");context->denseWavePack=linAlgPso(device.Get(),context->denseWavePackRoot.Get(),"dense-wave-pack.dxil");
                }
                auto p=std::make_unique<LinAlgProjection>();p->waveDense=true;p->rows=m*lanes;p->columns=n;p->inner=k;p->tile=64;
                p->root=context->denseWaveRoot;p->pipeline=context->denseWavePipeline;
                p->packed=defaultBuffer(UINT64(n)*k*2);p->optimal=p->packed;
                b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                cmd->SetComputeRootSignature(context->denseWavePackRoot.Get());cmd->SetPipelineState(context->denseWavePack.Get());
                cmd->SetComputeRootShaderResourceView(0,b->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(1,p->packed->GetGPUVirtualAddress());
                UINT shape[]={n,k};cmd->SetComputeRoot32BitConstants(2,2,shape,0);cmd->Dispatch((n*k/2+255)/256,1,1);
                transition(cmd.Get(),p->packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                context->linAlgWeightBytes+=UINT64(n)*k*2;++context->linAlgJobs;++context->denseWaveJobs;
                job->native=std::move(p);job->profileName+=studyRegister?"_wave_register32":"_wave_dense64";jobs.push_back(std::move(job));return;
            }
#endif
            UINT tileN=64;
#ifdef NR_DENSE_TILE
            tileN=context->denseTileN;
            if(tileN!=32&&tileN!=64)throw std::runtime_error("Invalid dense tile N");
#endif
            if(UINT64(m)*lanes>UINT64(65535)*32||n/tileN>65535)throw std::runtime_error("LinAlg dispatch limit");
            ComPtr<ID3D12DevicePreview> preview;check(device.As(&preview),"LinAlg graph device preview");
            D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT support{};
            check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT,&support,sizeof(support)),"LinAlg graph support");
            if(support.LinearAlgebraTier<D3D12_LINEAR_ALGEBRA_TIER_1_0)throw std::runtime_error("LinAlg graph unsupported");
            if(!context->linAlgPipeline){
                context->linAlgRootSignature=linAlgRoot(device.Get(),false);context->linAlgPackRoot=linAlgRoot(device.Get(),true);
                context->linAlgPipeline=linAlgPso(device.Get(),context->linAlgRootSignature.Get(),tileN==32?"matrix-n32.dxil":"matrix-optimal.dxil");
                context->linAlgPackPipeline=linAlgPso(device.Get(),context->linAlgPackRoot.Get(),tileN==32?"pack-n32.dxil":"pack-weights.dxil");
            }
            auto p=std::make_unique<LinAlgProjection>();p->rows=m*lanes;p->columns=n;p->inner=k;p->tile=tileN;
            p->root=context->linAlgRootSignature;p->pipeline=context->linAlgPipeline;
            D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO dest{};
            dest.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_MUL_OPTIMAL;dest.NumRows=tileN;dest.NumColumns=64;
            dest.DestDataType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;preview->GetLinearAlgebraMatrixConversionDestinationInfo(&dest);
            if(!dest.DestSize)throw std::runtime_error("LinAlg graph optimal size missing");
            p->stride=(dest.DestSize+255u)&~255u;
            UINT packedTileBytes=tileN*64*2;UINT tiles=(n/tileN)*(k/64);p->packed=defaultBuffer(UINT64(n)*k*2);p->optimal=defaultBuffer(UINT64(tiles)*p->stride);
            b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmd->SetComputeRootSignature(context->linAlgPackRoot.Get());cmd->SetPipelineState(context->linAlgPackPipeline.Get());
            cmd->SetComputeRootShaderResourceView(0,b->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(1,p->packed->GetGPUVirtualAddress());
            UINT shape[]={n,k};cmd->SetComputeRoot32BitConstants(2,2,shape,0);cmd->Dispatch((n*k/2+255)/256,1,1);
            transition(cmd.Get(),p->packed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12GraphicsCommandListPreview> list;check(cmd.As(&list),"LinAlg graph list preview");
            for(UINT tile=0;tile<tiles;++tile){D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO ci{};ci.DestInfo=dest;
                ci.SrcInfo={packedTileBytes,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT_ROW_MAJOR,128};
                ci.DataDesc={p->optimal->GetGPUVirtualAddress()+UINT64(tile)*p->stride,p->packed->GetGPUVirtualAddress()+UINT64(tile)*packedTileBytes};
                list->ConvertLinearAlgebraMatrix(&ci,1);
            }
            transition(cmd.Get(),p->optimal.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
#ifdef NR_CLEAN_PROFILE
            if(a->denseHalfShadow){p->inputShadow=true;p->pipeline=linAlgPso(device.Get(),p->root.Get(),GetEnvironmentVariableA("NR_STUDY_FP16_DENSE_NATIVE_LOAD",nullptr,0)?"fp16dense-thread-native.dxil":"fp16dense-thread.dxil");printf("NR_FP16_DENSE_JOB mode=thread m=%u n=%u k=%u\n",m,n,k);}
#endif
            context->linAlgWeightBytes+=UINT64(tiles)*p->stride;++context->linAlgJobs;
            job->native=std::move(p);job->profileName+="_linalg_optimal";jobs.push_back(std::move(job));return;
        }
#endif
        if(lanes>1){aBytes+=(lanes-1)*a->elements*4;bBytes+=(b->lanes-1)*b->elements*4;outBytes+=(lanes-1)*out->elements*4;}
        Tensor *ga=a,*gb=b,*go=out;UINT64 gaOffset=aOffset,gbOffset=bOffset;
        // Start with wide, non-batched projections; tiny attention GEMMs stay FP32.
        const bool half=context->tensorHalfPrecision&&batch==1&&n>=128&&k>=128;
        if(half){
            ++context->tensorHalfGemmJobs;
            ga=makeScratch("gemm-half-a",(UINT64(batch)*m*k+1)/2);
            const bool prepack=context->prepackHalfWeights&&b->immutableWeight;
            // Constant weights cannot share scratch: every matrix must survive all tiles/frames.
            gb=prepack?make((UINT64(batch)*k*n+1)/2):makeScratch("gemm-half-b",(UINT64(batch)*k*n+1)/2);
            go=makeScratch("gemm-half-out",(UINT64(batch)*m*n+1)/2);
            job->before.push_back(castJob(a,ga,UINT64(batch)*m*k,true,aOffset));
            if(prepack){
                auto conversion=castJob(b,gb,UINT64(batch)*k*n,true,bOffset);
                recordGemm(*conversion);job->setup.push_back(std::move(conversion));
                ++context->prepackedHalfWeights;
            }else job->before.push_back(castJob(b,gb,UINT64(batch)*k*n,true,bOffset));
            job->after.push_back(castJob(go,out,UINT64(batch)*m*n,false));
            job->a=ga;job->b=gb;job->out=go;gaOffset=gbOffset=0;
            aBytes=(aBytes/2+3)&~UINT64(3);bBytes=(bBytes/2+3)&~UINT64(3);outBytes=(outBytes/2+3)&~UINT64(3);
            job->profileName+="_fp16";
        }
        DML_BUFFER_TENSOR_DESC ab{half?DML_TENSOR_DATA_TYPE_FLOAT16:DML_TENSOR_DATA_TYPE_FLOAT32,DML_TENSOR_FLAG_NONE,4,as,nullptr,aBytes,0};
        auto bb=ab;bb.Sizes=bs;bb.TotalTensorSizeInBytes=bBytes;
        auto ob=ab;ob.Sizes=os;ob.TotalTensorSizeInBytes=outBytes;
        if(lanes>1){ab.Strides=astride;bb.Strides=bstride;ob.Strides=ostride;}
        DML_TENSOR_DESC at{DML_TENSOR_TYPE_BUFFER,&ab},bt{DML_TENSOR_TYPE_BUFFER,&bb},ot{DML_TENSOR_TYPE_BUFFER,&ob};
        DML_GEMM_OPERATOR_DESC gemm{};gemm.ATensor=&at;gemm.BTensor=&bt;gemm.OutputTensor=&ot;gemm.Alpha=1;
        gemm.TransB=transposeB ? DML_MATRIX_TRANSFORM_TRANSPOSE : DML_MATRIX_TRANSFORM_NONE;
        DML_OPERATOR_DESC od{DML_OPERATOR_GEMM,&gemm}; ComPtr<IDMLOperator> op;
        const bool conv=half&&context->convHalf&&b->immutableWeight&&!transposeB;
        UINT h=1,w=m;UINT ca[4]{},cb[4]{},co[4]{},sa[4]{},sb[4]{},so[4]{};
        UINT stride[]={1,1},dilation[]={1,1},padding[]={0,0};DML_CONVOLUTION_OPERATOR_DESC convolution{};
        if(conv){
            h=UINT(std::sqrt(double(m)));while(m%h)--h;w=m/h;
            ca[0]=1;ca[1]=k;ca[2]=h;ca[3]=w;cb[0]=n;cb[1]=k;cb[2]=cb[3]=1;co[0]=1;co[1]=n;co[2]=h;co[3]=w;
            sa[0]=m*k;sa[1]=1;sa[2]=w*k;sa[3]=k;sb[0]=1;sb[1]=sb[2]=sb[3]=n;so[0]=m*n;so[1]=1;so[2]=w*n;so[3]=n;
            ab.Sizes=ca;ab.Strides=sa;bb.Sizes=cb;bb.Strides=sb;bb.Flags=DML_TENSOR_FLAG_OWNED_BY_DML;ob.Sizes=co;ob.Strides=so;
            convolution.InputTensor=&at;convolution.FilterTensor=&bt;convolution.OutputTensor=&ot;
            convolution.Mode=DML_CONVOLUTION_MODE_CROSS_CORRELATION;convolution.Direction=DML_CONVOLUTION_DIRECTION_FORWARD;
            convolution.DimensionCount=2;convolution.Strides=stride;convolution.Dilations=dilation;
            convolution.StartPadding=padding;convolution.EndPadding=padding;convolution.OutputPadding=padding;convolution.GroupCount=1;
            od={DML_OPERATOR_CONVOLUTION,&convolution};job->profileName+="_conv1x1";
        }
        check(dml->CreateOperator(&od,IID_PPV_ARGS(&op)),"GEMM creation");
        UINT flags=context->allowHalfPrecision?DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION:0;
        if(context->directComputeGemm)flags|=DML_EXECUTION_FLAG_DISABLE_META_COMMANDS;
        check(dml->CompileOperator(op.Get(),static_cast<DML_EXECUTION_FLAGS>(flags),IID_PPV_ARGS(&job->op)),"GEMM compilation");
        initializeJob(job.get(),{{ga->resource.Get(),gaOffset,aBytes},{gb->resource.Get(),gbOffset,bBytes},{nullptr,0,0}}, {go->resource.Get(),0,outBytes},conv);
        jobs.push_back(std::move(job));
    }
    void submit(){
        if(context->externalRecording)throw std::runtime_error("Submission is forbidden while recording on caller list");
        check(cmd->Close(),"Close block commands");ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),++serial),"Block completion signal");check(fence->SetEventOnCompletion(serial,event),"Block completion event");
        if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0){fprintf(stderr,"GPU completion timed out\n");ExitProcess(2);}
        check(device->GetDeviceRemovedReason(),"Device state");
        if(diagnostics){
            UINT64 messages=diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
            for(UINT64 i=0;i<messages;++i){SIZE_T bytes=0;check(diagnostics->GetMessage(i,nullptr,&bytes),"Diagnostic size");
                std::vector<unsigned char> storage(bytes);auto message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                check(diagnostics->GetMessage(i,message,&bytes),"Diagnostic message");
                if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)
                    throw std::runtime_error(std::string("D3D12 validation: ")+message->pDescription);
            }
            diagnostics->ClearStoredMessages();
        }
        check(allocator->Reset(),"Reset allocator");check(cmd->Reset(allocator.Get(),nullptr),"Reset list");
    }
    std::vector<float> download(Tensor* t){if(context->externalRecording)throw std::runtime_error("Readback is forbidden during external recording");auto rb=buffer(device.Get(),t->elements*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        t->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(rb.Get(),0,t->resource.Get(),0,t->elements*4);submit();
        void* p;D3D12_RANGE range{0,static_cast<SIZE_T>(t->elements*4)};check(rb->Map(0,&range,&p),"Read block tensor");
        std::vector<float> result(static_cast<size_t>(t->elements));memcpy(result.data(),p,static_cast<size_t>(t->elements*4));D3D12_RANGE empty{0,0};rb->Unmap(0,&empty);return result;}
};
