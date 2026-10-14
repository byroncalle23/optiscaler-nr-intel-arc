#include "nr_temporal.h"
#include "dml_frame.h"
#include "dml_graph_pipeline.h"
#include <fstream>

// Standalone FP32 reference graph. This validates the temporal model contract,
// not the installed cooperative-vector policy's speed or live game quality.
class TemporalModelHost : public NativeRuntime {
public:
    void copy(Tensor* source,Tensor* destination) {
        if(source->elements!=destination->elements)throw std::runtime_error("Temporal graph shape mismatch");
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);destination->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(destination->resource.Get(),0,source->resource.Get(),0,source->elements*4);
    }
    void checkDebug() {
        if(!diagnostics)throw std::runtime_error("Debug layer required");
        for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T n=0;check(diagnostics->GetMessage(i,nullptr,&n),"Debug size");
            std::vector<char> data(n);auto m=reinterpret_cast<D3D12_MESSAGE*>(data.data());check(diagnostics->GetMessage(i,m,&n),"Debug message");
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(m->pDescription);}
    }
};
static void save(const std::string& path,const std::vector<float>& values) {
    std::ofstream file(nativePath(path),std::ios::binary);file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*4));
    if(!file)throw std::runtime_error("Model output write failed");
}
int main(int argc,char** argv){try {
    if(argc!=3)throw std::runtime_error("Usage: nr_temporal_model_test.exe model-directory fixture-directory");
    SetEnvironmentVariableA("NR_D3D12_DEBUG","1");
    const std::string model=argv[1],folder=argv[2];constexpr UINT w=640,h=360,pw=640,ph=384;
    TemporalModelHost io;auto ctx=io.sharedContext();
    auto colour=io.load(folder,"current",UINT64(w)*h*3),motion=io.load(folder,"motion",UINT64(w)*h*2),confidence=io.load(folder,"confidence",UINT64(w)*h);
    FrameHarness input(model+"/frame_input",ctx,nullptr,nullptr,colour);input.constrainToGpuBudget();ctx->reuseScratch=true;
    GraphPipeline graph(model,ctx,input.resultTensor());
    auto noise=io.load(model+"/frame_input","noise",UINT64(pw)*ph*3);
    NativeTemporalPass temporal(w,h,pw,ph,ctx,colour,noise,graph.resultTensor(),motion,confidence);
    // Never read uninitialized history on the first frame, including padded rows.
    temporal.settings(false,true,.02f,true);io.submit();
    NativeGpuProfile profiler(ctx->device.Get(),16);
    for(int frame=0;frame<2;++frame) {
        profiler.reset();auto id=profiler.begin(ctx->cmd.Get(),"reference_full_graph_temporal");
        temporal.recordFeatures();io.copy(temporal.featureTensor(),input.resultTensor());graph.record();temporal.recordComposition();
        profiler.end(ctx->cmd.Get(),id);profiler.resolve(ctx->cmd.Get());io.submit();temporal.advanceSubmittedFrame();
        auto ms=profiler.readAfterFence(ctx->queue.Get()).at("reference_full_graph_temporal").ms;
        const auto prefix=folder+"/model-frame"+std::to_string(frame);
        save(prefix+"-head.bin",io.download(graph.resultTensor()));
        save(prefix+"-features.bin",io.download(temporal.featureTensor()));
        save(prefix+"-composed.bin",io.download(temporal.composedTensor()));
        printf("{\"frame\":%d,\"reference_gpu_ms\":%.6f}\n",frame,ms);
    }
    io.checkDebug();printf("FULL_MODEL_TEMPORAL_DEBUG_PASS\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
