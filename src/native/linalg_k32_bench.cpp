// Isolated real-weight benchmark. No changes to the installed DLL or graph policy.
#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
class K32Bench : public BatchGraphProbe {
public:
    void record(UINT index){recordGemm(*jobs.at(index));}
    void specialize(){jobs.back()->native->pipeline=linAlgPso(device.Get(),jobs.back()->native->root.Get(),"matrix-k32-static.dxil");}
    void buildCache(UINT index,ID3D12QueryHeap* queries,ID3D12Resource* ticks,
                    ComPtr<ID3D12CommandAllocator>& cacheAllocator,ComPtr<ID3D12GraphicsCommandList>& cache){
        record(index);submit(); // Establish entry states before closing the reusable list.
        std::vector<NativeTrackedState> states;appendTrackedStates(states);
        std::vector<D3D12_RESOURCE_STATES> before;for(auto s:states)before.push_back(*s.state);
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&cacheAllocator)),"K32 allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,cacheAllocator.Get(),nullptr,IID_PPV_ARGS(&cache)),"K32 cache");
        {ExternalCommandRecording borrowed(context,cache.Get());
            cmd->EndQuery(queries,D3D12_QUERY_TYPE_TIMESTAMP,0);
            for(UINT r=0;r<8;++r)record(index);
            cmd->EndQuery(queries,D3D12_QUERY_TYPE_TIMESTAMP,1);
            cmd->ResolveQueryData(queries,D3D12_QUERY_TYPE_TIMESTAMP,0,2,ticks,0);
            for(size_t i=0;i<states.size();++i){auto s=states[i];if(*s.state!=before[i])transition(cmd.Get(),s.resource,*s.state,before[i]);*s.state=before[i];}
            check(cache->Close(),"K32 cache close");}
    }
};
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("Usage: k32_bench block-folder qkv|projection output-folder");
    std::string folder=argv[1],role=argv[2],outFolder=argv[3];
    if(role!="qkv"&&role!="projection")throw std::runtime_error("Invalid role");
    std::filesystem::create_directories(nativePath(outFolder));UINT shape[9]{};
    std::ifstream config(nativePath(folder+"/shape.txt"));for(auto& x:shape)if(!(config>>x))throw std::runtime_error("Invalid shape");
    if(shape[8]!=32)throw std::runtime_error("Only C32 windows");
    UINT m=shape[4]*shape[5],n=role=="qkv"?96:32;
    K32Bench probe;auto ctx=probe.sharedContext();ctx->tileBatch=1;ctx->linAlg=true;
    auto a=probe.make(UINT64(m)*32),b=probe.loadWeight(folder,role.c_str(),32*n);
    b->c32ProjectionRole=role=="qkv"?1:2;
    std::vector<float> input(size_t(m)*32);
    // Deterministic signed activation stress, not captured game activations.
    for(size_t i=0;i<input.size();++i)input[i]=float(int((i*17+i/32*13)%257)-128)/128;
    probe.uploadLanes(a,input);std::vector<Tensor*> outputs;
    for(UINT p=0;p<3;++p){auto out=probe.make(UINT64(m)*n);outputs.push_back(out);
        ctx->c32ProjectionPolicy=p?"both":"baseline";probe.addGemm(a,b,out,m,n,32);if(p==2)probe.specialize();}
    probe.submit();
    std::vector<std::vector<float>> firstOutputs;
    for(UINT p=0;p<3;++p){probe.record(p);probe.submit();auto data=probe.downloadLanes(outputs[p]);
        for(float x:data)if(!std::isfinite(x))throw std::runtime_error("Nonfinite output");
        firstOutputs.push_back(std::move(data));}
    if(memcmp(firstOutputs[1].data(),firstOutputs[2].data(),firstOutputs[1].size()*4))throw std::runtime_error("Specialized/generic output mismatch");
    double error=0,energy=0;for(size_t i=0;i<firstOutputs[0].size();++i){double d=double(firstOutputs[1][i])-firstOutputs[0][i];error+=d*d;energy+=double(firstOutputs[0][i])*firstOutputs[0][i];}
    printf("K32_OUTPUT specialized_generic_exact=1 dml_relative_rmse=%.9f\n",sqrt(error/std::max(energy,1e-30)));
    D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
    ComPtr<ID3D12QueryHeap> queries;check(ctx->device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)),"K32 query heap");
    auto ticks=buffer(ctx->device.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 frequency;check(ctx->queue->GetTimestampFrequency(&frequency),"K32 frequency");
    // Each visit builds a state-balanced cache. Only eight dispatches + normal barriers are timed.
    for(UINT cycle=0;cycle<3;++cycle)for(UINT p:{0u,1u,2u,2u,1u,0u}){
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> cache;
        probe.buildCache(p,queries.Get(),ticks.Get(),allocator,cache);
        for(UINT repeat=0;repeat<5;++repeat){probe.executeClosed(cache.Get());
            void* mapped;D3D12_RANGE range{0,16},empty{0,0};check(ticks->Map(0,&range,&mapped),"K32 ticks");
            auto t=static_cast<UINT64*>(mapped);double ms=double(t[1]-t[0])*1000/frequency/8;ticks->Unmap(0,&empty);
            if(repeat)printf("K32_TIMING cycle=%u policy=%u ms=%.9f\n",cycle,p,ms);}
    }
    for(UINT p=0;p<3;++p){auto data=probe.downloadLanes(outputs[p]);
        if(memcmp(firstOutputs[p].data(),data.data(),data.size()*4))throw std::runtime_error("Replay output changed");}
    printf("K32_BENCH_PASS M=%u N=%u replay_exact=1\n",m,n);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
