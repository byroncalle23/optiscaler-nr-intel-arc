#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: branch_unit assets output");
    std::string assets=argv[1],folder=argv[2];std::filesystem::create_directories(nativePath(folder));
    std::ifstream config(nativePath(assets+"/shape.txt"));UINT h,w,rest[7];if(!(config>>h>>w))throw std::runtime_error("Unit shape missing");
    for(UINT& v:rest)if(!(config>>v))throw std::runtime_error("Unit shape incomplete");UINT channels=rest[6];
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->branchCompact=true;ctx->tileBatch=1;
    auto input=probe.make(UINT64(h)*w*channels,false);std::vector<float> values(size_t(input->elements));
    std::ifstream f(nativePath(assets+"/input.bin"),std::ios::binary);if(!f.read(reinterpret_cast<char*>(values.data()),values.size()*4))throw std::runtime_error("Unit input read");
    probe.uploadLanes(input,values);WindowHarness window(assets,ctx,input,false);std::vector<float> first;
    for(UINT run=0;run<3;++run){window.recordCompactBranchUnit();probe.submit();auto out=probe.downloadLanes(window.compactBranchUnitResult());
        if(!run)first=out;else if(memcmp(first.data(),out.data(),out.size()*4))throw std::runtime_error("Compact branch unit not repeatable");}
    std::ofstream output(nativePath(folder+"/output.bin"),std::ios::binary);output.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!output)throw std::runtime_error("Unit output write");
    printf("COMPACT_BRANCH_UNIT_PASS heads=%u pixels=%u repeat_exact=1\n",channels/32,h*w);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
