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
    if(argc!=5)throw std::runtime_error("Usage: slm_unit output reference_shader candidate1 candidate2");
    std::string folder=argv[1];std::filesystem::create_directories(nativePath(folder));
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=2;
    linAlgCheckWaveFP16(ctx->device.Get());auto root=linAlgWindowRoot(ctx->device.Get());
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    for(const char* name:{argv[2],argv[3],argv[4]})pipelines.push_back(linAlgPso(ctx->device.Get(),root.Get(),name));
    for(UINT channels:{32u,64u,128u,256u,512u}){
        UINT heads=channels/32;auto input=probe.make(UINT64(128)*channels*3);auto output=probe.make(UINT64(128)*channels);
        auto bias=probe.make(UINT64(heads)*4096,false);auto scale=probe.make(heads,false);
        std::vector<float> values(size_t(input->elements*2)),biasValues(size_t(bias->elements)),scales(heads);
        for(size_t i=0;i<values.size();++i)values[i]=float(int((i*17+(i/32)*31+(i/input->elements)*73)%257)-128)/64;
        for(size_t i=0;i<biasValues.size();++i)biasValues[i]=float(int((i*11+i/64*7)%17)-8)/32;
        for(UINT i=0;i<heads;++i)scales[i]=.5f+float(i)/16;
        probe.uploadLanes(input,values);probe.uploadLanes(bias,biasValues);probe.uploadLanes(scale,scales);
        std::vector<float> reference;
        auto run=[&](UINT policy){
            input->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);bias->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);scale->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);output->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx->cmd->SetComputeRootSignature(root.Get());ctx->cmd->SetPipelineState(pipelines[policy].Get());
            ctx->cmd->SetComputeRootShaderResourceView(0,input->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(1,bias->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(4,scale->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootUnorderedAccessView(2,output->resource->GetGPUVirtualAddress());
            UINT constants[]={2*heads,heads,UINT(input->elements),UINT(output->elements)};ctx->cmd->SetComputeRoot32BitConstants(3,4,constants,0);ctx->cmd->Dispatch(2*heads,1,2);uavBarrier(ctx->cmd.Get());probe.submit();return probe.downloadLanes(output);
        };
        for(UINT policy=0;policy<3;++policy){
            probe.uploadLanes(input,values);std::vector<float> first;
            for(UINT repeat=0;repeat<3;++repeat){auto actual=run(policy);
                for(float v:actual)if(!std::isfinite(v))throw std::runtime_error("Nonfinite partition unit");
                if(!repeat)first=actual;else if(memcmp(first.data(),actual.data(),actual.size()*4))throw std::runtime_error("Partition unit repeat mismatch");}
            if(!policy)reference=first;else if(memcmp(first.data(),reference.data(),first.size()*4))throw std::runtime_error("Partition context differs from accepted WG256");
            auto changed=values;UINT selected=heads-1;
            for(UINT token=64;token<128;++token)for(UINT plane=0;plane<3;++plane)for(UINT c=0;c<32;++c)changed[size_t(input->elements)+token*channels*3+plane*channels+selected*32+c]*=-.75f;
            probe.uploadLanes(input,changed);auto alt=run(policy);bool affected=false;
            for(size_t i=0;i<first.size();++i){bool target=i>=output->elements&&((i-output->elements)/channels)>=64&&(i%channels)/32==selected;
                if(!target&&memcmp(&first[i],&alt[i],4))throw std::runtime_error("Partition batch/window/head contamination");
                if(target&&first[i]!=alt[i])affected=true;}
            if(!affected)throw std::runtime_error("Partition unit insensitive to changed values");
            std::string name=folder+"/context-C"+std::to_string(channels)+"-P"+std::to_string(policy)+".bin";std::ofstream f(nativePath(name),std::ios::binary);f.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!f)throw std::runtime_error("Partition unit output");
            printf("PARTITION_UNIT_PASS C=%u policy=%u exact_reference=1 repeat_exact=1 isolation=1\n",channels,policy);
        }
    }
    puts("PARTITION_UNITS_PASS cases=15");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}

