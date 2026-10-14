#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("Usage: c32_unit assets output policy");
    std::string assets=argv[1],folder=argv[2],policy=argv[3];
    if(policy!="compact-f16"&&policy!="compact-partials")throw std::runtime_error("C32 unit policy");
    std::filesystem::create_directories(nativePath(folder));std::ifstream config(nativePath(assets+"/shape.txt"));UINT h,w;
    if(!(config>>h>>w))throw std::runtime_error("C32 shape");
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=1;ctx->c32CompactPolicy=policy;
    ctx->linAlgWindowFused=true;ctx->linAlgWindowNormalizeFused=true;ctx->globalNormalizationPolicy="fused-native-round";
    auto input=probe.make(UINT64(h)*w*32,false);std::vector<float> values(size_t(input->elements));
    std::ifstream f(nativePath(assets+"/input.bin"),std::ios::binary);if(!f.read(reinterpret_cast<char*>(values.data()),values.size()*4))throw std::runtime_error("C32 input");
    probe.uploadLanes(input,values);WindowHarness window(assets,ctx,input,false);std::vector<float> first;
    for(UINT run=0;run<3;++run){window.recordCompactC32Unit();probe.submit();auto out=probe.downloadLanes(window.compactC32UnitResult());
        if(!run)first=out;else if(memcmp(first.data(),out.data(),out.size()*4))throw std::runtime_error("C32 repeat mismatch");}
    std::ofstream output(nativePath(folder+"/output.bin"),std::ios::binary);output.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!output)throw std::runtime_error("C32 output write");
    printf("COMPACT_C32_UNIT_PASS policy=%s pixels=%u repeat_exact=1\n",policy.c_str(),h*w);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
