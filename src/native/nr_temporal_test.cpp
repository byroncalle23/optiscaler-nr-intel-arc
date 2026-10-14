#include "nr_temporal.h"
#include <fstream>

class TemporalTestHost : public NativeRuntime {
public:
    void copy(Tensor* source,Tensor* destination) {
        source->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);destination->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(destination->resource.Get(),0,source->resource.Get(),0,source->elements*4);
    }
    void checkDebug() {
        if(!diagnostics)throw std::runtime_error("Temporal test requires debug diagnostics");
        for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i) {
            SIZE_T bytes=0;check(diagnostics->GetMessage(i,nullptr,&bytes),"Debug message size");std::vector<char> storage(bytes);
            auto message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(diagnostics->GetMessage(i,message,&bytes),"Debug message");
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(message->pDescription);
        }
    }
};
static void save(const std::string& path,const std::vector<float>& values) {
    std::ofstream file(nativePath(path),std::ios::binary);file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*4));
    if(!file)throw std::runtime_error("Temporal output write failed");
}
int main(int argc,char** argv) {try {
    if(argc!=2)throw std::runtime_error("Usage: nr_temporal_test.exe fixture-directory");
    SetEnvironmentVariableA("NR_D3D12_DEBUG","1");
    const std::string folder=argv[1];UINT w,h,pw,ph,valid,reset,clamp;float margin;
    std::ifstream shape(nativePath(folder+"/shape.txt"));
    if(!(shape>>w>>h>>pw>>ph>>valid>>reset>>margin>>clamp))throw std::runtime_error("Invalid temporal fixture");
    TemporalTestHost io;auto ctx=io.sharedContext();
    auto current=io.load(folder,"current",UINT64(w)*h*3),noise=io.load(folder,"noise",UINT64(pw)*ph*3);
    auto head=io.load(folder,"head",UINT64(pw)*ph*4),motion=io.load(folder,"motion",UINT64(w)*h*2);
    auto confidence=io.load(folder,"confidence",UINT64(w)*h),history=io.load(folder,"history",UINT64(w)*h*3);
    NativeTemporalPass pass(w,h,pw,ph,ctx,current,noise,head,motion,confidence);
    float strength=0;int frames=2;std::ifstream policy(nativePath(folder+"/stability.txt"));if(policy){if(!(policy>>strength>>frames)||frames<2||frames>16)throw std::runtime_error("Invalid stability fixture");unsigned bounded=0;policy>>bounded;policy.clear();pass.stability(strength,bounded!=0,0,bounded>=2,bounded==3);}
    io.copy(history,pass.testHistory());io.submit();
    pass.settings(valid!=0,reset!=0,margin,clamp!=0);
    NativeGpuProfile profile(ctx->device.Get(),16);
    for(int frame=0;frame<frames;++frame) {
        if(policy){
            for(auto field:{std::make_pair("current",current),std::make_pair("head",head),std::make_pair("motion",motion),std::make_pair("confidence",confidence)}){
                auto name="input"+std::to_string(frame)+"-"+field.first;
                if(std::filesystem::exists(nativePath(folder+"/"+name+".bin")))io.copy(io.load(folder,name.c_str(),field.second->elements),field.second);
            }
            io.submit();bool frameReset=frame==0||std::filesystem::exists(nativePath(folder+"/input"+std::to_string(frame)+"-reset.flag"));
            pass.settings(frame>0,frameReset,margin,clamp!=0);
        }
        profile.reset();auto range=profile.begin(ctx->cmd.Get(),"temporal");
        pass.recordFeatures();pass.recordComposition();profile.end(ctx->cmd.Get(),range);profile.resolve(ctx->cmd.Get());
        io.submit();pass.advanceSubmittedFrame();
        const auto ms=profile.readAfterFence(ctx->queue.Get()).at("temporal").ms;
        std::string prefix=folder+"/frame"+std::to_string(frame);
        save(prefix+"-features.bin",io.download(pass.featureTensor()));
        save(prefix+"-reprojected.bin",io.download(pass.reprojectedTensor()));
        save(prefix+"-composed.bin",io.download(pass.composedTensor()));
        printf("{\"frame\":%d,\"gpu_ms\":%.6f}\n",frame,ms);
    }
    io.checkDebug();printf("TEMPORAL_GPU_DEBUG_PASS\n");return 0;
} catch(const std::exception& e) {fprintf(stderr,"%s\n",e.what());return 1;} }

