#define NOMINMAX
#include <windows.h>
extern "C" {__declspec(dllexport) extern const UINT D3D12SDKVersion=721;__declspec(dllexport) extern const char* D3D12SDKPath=".\\D3D12\\";}
#define NR_EXPERIMENTAL_LINALG
#define NR_GRAPH_DIAGNOSTICS
#define main unusedBatchMain
#include "dml_batch_graph.cpp"
#undef main

struct Snapshot {std::string name;UINT64 elements;UINT lanes;ComPtr<ID3D12Resource> readback;};
int main(int argc,char** argv){try{
    if(argc!=6)throw std::runtime_error("Usage: diagnostic assets output B nativePolicy detailNode-or-none");
    UINT batch=std::stoul(argv[3]);if(batch!=1&&batch!=2)throw std::runtime_error("Diagnostic B1/2 only");
    std::string folder=argv[2],policy=argv[4],detail=argv[5];std::filesystem::create_directories(folder);
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=batch;ctx->reuseScratch=true;
    ctx->linAlg=policy!="dml";ctx->linAlgQkvOnly=policy=="qkv";ctx->linAlgFfnOnly=policy=="ffn";
    if(policy!="dml"&&policy!="all"&&policy!="qkv"&&policy!="ffn")throw std::runtime_error("Invalid policy");
    probe.constrainToGpuBudget();std::ifstream config(nativePath(std::string(argv[1])+"/frame_input/shape.txt"));
    UINT h,w,th,tw;if(!(config>>h>>w>>th>>tw))throw std::runtime_error("Missing shape");
    auto colour=probe.make(UINT64(h)*w*3);std::vector<float> input(size_t(colour->elements*batch));
    for(UINT lane=0;lane<batch;++lane)for(UINT64 i=0;i<colour->elements;++i)
        input[size_t(lane*colour->elements+i)]=float((i*17+(i/3/w)*31+lane*73)%257)/256.0f;
    probe.uploadLanes(colour,input);FrameHarness features(std::string(argv[1])+"/frame_input",ctx,nullptr,nullptr,colour);
    auto graph=std::make_unique<GraphPipeline>(argv[1],ctx,features.resultTensor());
    FrameHarness compose(std::string(argv[1])+"/frame_output",ctx,graph->resultTensor(),colour);
    // Complete setup before capture, preserving converted weights' lifetimes.
    probe.submit();std::vector<Snapshot> snapshots;
    auto capture=[&](const std::string& name,Tensor* tensor){
        UINT64 bytes=tensor->elements*tensor->lanes*4;auto rb=buffer(ctx->device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto before=tensor->state;tensor->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
        ctx->cmd->CopyBufferRegion(rb.Get(),0,tensor->resource.Get(),0,bytes);tensor->to(ctx->cmd.Get(),before);
        snapshots.push_back({name,tensor->elements,tensor->lanes,rb});
    };
    features.record();capture("features",features.resultTensor());
    for(auto& node:graph->publicNodes()){
        ctx->diagnosticNode=node->name;ctx->diagnosticJob=0;ctx->diagnosticShader=0;
        ctx->capture=node->name==detail?capture:std::function<void(const std::string&,Tensor*)>{};
        node->record();ctx->capture={};capture(node->name,node->output());
    }
    compose.record();capture("rgb",compose.resultTensor());probe.submit();
    std::ofstream manifest(folder+"/snapshots.tsv");
    for(auto& s:snapshots){UINT64 bytes=s.elements*s.lanes*4;void* p;D3D12_RANGE range{0,size_t(bytes)},empty{0,0};
        check(s.readback->Map(0,&range,&p),"Snapshot readback");std::ofstream file(folder+"/"+s.name+".bin",std::ios::binary);
        file.write(static_cast<const char*>(p),std::streamsize(bytes));s.readback->Unmap(0,&empty);if(!file)throw std::runtime_error("Snapshot write");
        manifest<<s.name<<'\t'<<s.elements<<'\t'<<s.lanes<<'\n';
    }
    if(!manifest)throw std::runtime_error("Manifest write");
    printf("DIAGNOSTIC_PASS B%u POLICY %s SNAPSHOTS %zu NATIVE_JOBS %llu\n",batch,policy.c_str(),snapshots.size(),ctx->linAlgJobs);return 0;
}catch(const std::exception& e){fprintf(stderr,"DIAGNOSTIC_FAILED %s\n",e.what());return 1;}}
