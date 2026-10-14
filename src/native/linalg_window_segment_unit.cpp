// Whole-window segment validation including FFN, padding, attention and retained skip.
#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--cpu-check"){
        for(const char* reuse:{"0","1"}){
            D3D_SHADER_MACRO macros[]={{"NR_WINDOW_REUSE_FEED",reuse},{"NR_WINDOW_PARALLEL_NORM","1"},{"NR_NATIVE_HALF","0"},{"NR_TILE_BATCH","0"},{nullptr,nullptr}};
            for(const char* entry:{"WindowFeedPack","WindowCropResidual"}){compileNativeShader("window_ops.hlsl",entry,macros);printf("WINDOW_SHADER_COMPILE_PASS %s reuse=%s\n",entry,reuse);}
        }return 0;
    }
    if(argc!=5)throw std::runtime_error("Usage: window_segment_unit assets output policy pattern");
    std::string folder=argv[1],output=argv[2],policy=argv[3];UINT pattern=std::stoul(argv[4]);
    if(policy!="baseline"&&policy!="fused")throw std::runtime_error("Invalid segment policy");
    UINT shape[9];std::ifstream config(nativePath(folder+"/shape.txt"));for(auto& x:shape)if(!(config>>x))throw std::runtime_error("Unit shape");
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=1;ctx->linAlg=true;
    ctx->linAlgGrouped=true;ctx->linAlgWindowFused=true;ctx->linAlgWindowNormalizeFused=true;
    ctx->branchCompact=true;ctx->c32CompactPolicy="compact-f16";ctx->globalNormalizationPolicy="fused-native-round";
    ctx->attentionPartitionPolicy="wg256";ctx->denseTileN=32;ctx->normWavePolicy="wave8";ctx->softmaxWavePolicy="wave8";
    ctx->c32ProjectionPolicy="baseline";ctx->windowSegmentPolicy=policy;
    auto input=probe.make(UINT64(shape[0])*shape[1]*shape[8]);std::vector<float> values(size_t(input->elements));
    for(size_t i=0;i<values.size();++i){
        if(pattern==0)values[i]=float(int((i*17+i/shape[8]*13)%129)-64)/64;
        else if(pattern==1)values[i]=float(int(i%17)-8)*0.0009765625f;
        else values[i]=float(int((i*11)%41)-20)*0.0625f+0.00390625f;
    }
    probe.uploadLanes(input,values);WindowHarness window(folder,ctx,input,true);probe.submit();
    std::vector<float> first,skip;
    for(UINT repeat=0;repeat<3;++repeat){window.record();probe.submit();auto actual=probe.downloadLanes(window.resultTensor()),saved=probe.downloadLanes(window.skipTensor());
        for(float x:actual)if(!std::isfinite(x))throw std::runtime_error("Nonfinite segment output");
        for(float x:saved)if(!std::isfinite(x))throw std::runtime_error("Nonfinite retained skip");
        if(repeat&&(memcmp(first.data(),actual.data(),actual.size()*4)||memcmp(skip.data(),saved.data(),saved.size()*4)))throw std::runtime_error("Unit repeat mismatch");
        if(!repeat){first=std::move(actual);skip=std::move(saved);}
    }
    std::filesystem::create_directories(nativePath(output));
    for(auto item:std::vector<std::pair<std::string,const std::vector<float>*>>{{"output",&first},{"skip",&skip}}){
        std::ofstream f(nativePath(output+"/"+item.first+".bin"),std::ios::binary);f.write(reinterpret_cast<const char*>(item.second->data()),item.second->size()*4);if(!f)throw std::runtime_error("Unit output write");}
    // Repeat after changing an input pixel, then restore: no dependence on stale padding/history.
    auto changed=values;for(UINT c=0;c<shape[8];++c)changed[changed.size()-shape[8]+c]+=0.5f;
    probe.uploadLanes(input,changed);window.record();probe.submit();auto alt=probe.downloadLanes(window.resultTensor());
    if(!memcmp(first.data(),alt.data(),first.size()*4))throw std::runtime_error("Segment input insensitivity");
    probe.uploadLanes(input,values);window.record();probe.submit();auto restored=probe.downloadLanes(window.resultTensor());
    if(memcmp(first.data(),restored.data(),first.size()*4))throw std::runtime_error("Stale segment output after input restoration");
    printf("WINDOW_SEGMENT_UNIT_PASS C=%u pattern=%u policy=%s reused=%u fused=%u repeat_exact=1 restored_exact=1\n",shape[8],pattern,policy.c_str(),ctx->windowFeedReused,ctx->windowC32Fused);
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
