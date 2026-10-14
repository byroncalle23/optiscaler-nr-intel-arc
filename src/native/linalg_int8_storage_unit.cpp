#define NOMINMAX
#include <windows.h>
extern "C" {__declspec(dllexport) extern const UINT D3D12SDKVersion=721;__declspec(dllexport) extern const char* D3D12SDKPath=".\\native_nr_runtime\\";}
#ifndef NR_EXPERIMENTAL_LINALG
#define NR_EXPERIMENTAL_LINALG
#endif
#define main nr_unused_graph_main
#include "dml_batch_graph.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: shared_unit shader");
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=2;
    auto root=linAlgWindowRoot(ctx->device.Get());auto pso=linAlgPso(ctx->device.Get(),root.Get(),argv[1]);
    auto input=probe.make(768);auto output=probe.make(1024);
    for(UINT pattern=0;pattern<4;++pattern){
        std::vector<float> values(1536);
        for(size_t i=0;i<values.size();++i)values[i]=pattern==0?0:pattern==1?1:pattern==2?float(int((i*17+i/32*7)%255)-127):float(int((i*31+i/16*13)%17)-8);
        probe.uploadLanes(input,values);
        input->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        output->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for(UINT layout=0;layout<2;++layout){
        ctx->cmd->SetComputeRootSignature(root.Get());ctx->cmd->SetPipelineState(pso.Get());
        ctx->cmd->SetComputeRootShaderResourceView(0,input->resource->GetGPUVirtualAddress());
        ctx->cmd->SetComputeRootUnorderedAccessView(2,output->resource->GetGPUVirtualAddress());
        UINT constants[]={layout,0,768,1024};ctx->cmd->SetComputeRoot32BitConstants(3,4,constants,0);
        ctx->cmd->Dispatch(1,1,2);uavBarrier(ctx->cmd.Get());probe.submit();}
        auto actual=probe.downloadLanes(output);
        for(UINT lane=0;lane<2;++lane)for(UINT transpose=0;transpose<2;++transpose)for(UINT r=0;r<8;++r)for(UINT col=0;col<16;++col){
            int expected=0;for(UINT k=0;k<32;++k)expected+=int(values[lane*768+r*32+k])*int(values[lane*768+256+(transpose?col*32+k:k*16+col)]);
            if(actual[lane*1024+transpose*128+r*16+col]!=float(expected)){fprintf(stderr,"pattern=%u lane=%u trans=%u r=%u c=%u expected=%d actual=%g\n",pattern,lane,transpose,r,col,expected,actual[lane*1024+transpose*128+r*16+col]);throw std::runtime_error("Packed shared INT8 CPU oracle mismatch");}
        }
        printf("SHARED_I8_PASS pattern=%u lanes=2 layouts=2 exact_cpu=1\n",pattern);
    }
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
