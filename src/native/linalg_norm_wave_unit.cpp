#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: norm_wave_unit output");
    std::string folder=argv[1];std::filesystem::create_directories(nativePath(folder));
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=2;
    linAlgCheckWaveFP16(ctx->device.Get());auto root=linAlgWindowRoot(ctx->device.Get());
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    for(const char* name:{"attention-wg256.dxil","attention-norm-wave8.dxil","norm-capture0.dxil","norm-capture1.dxil"})pipelines.push_back(linAlgPso(ctx->device.Get(),root.Get(),name));
    for(UINT channels:{32u,64u,128u,256u,512u}){
        UINT heads=channels/32;auto input=probe.make(UINT64(128)*channels*3);auto contextOutput=probe.make(UINT64(128)*channels);auto normOutput=probe.make(UINT64(128)*channels*2);Tensor* output=contextOutput;
        auto bias=probe.make(UINT64(heads)*4096,false);auto scale=probe.make(heads,false);
        for(UINT pattern=0;pattern<3;++pattern){
        std::vector<float> values(size_t(input->elements*2)),biasValues(size_t(bias->elements)),scales(heads);
        for(size_t i=0;i<values.size();++i)values[i]=float(int((i*17+(i/32)*31+(i/input->elements)*73)%257)-128)/64;
        if(pattern)for(size_t i=0;i<values.size();++i){
            UINT plane=UINT(i%(channels*3))/channels;
            if(plane<2)values[i]=pattern==1?0.0f:((i%channels)%8==0?float(int((i/32)%7)-3)/8192.0f:0.0f);
        }
        for(size_t i=0;i<biasValues.size();++i)biasValues[i]=float(int((i*11+i/64*7)%17)-8)/32;
        for(UINT i=0;i<heads;++i)scales[i]=.5f+float(i)/16;
        probe.uploadLanes(input,values);probe.uploadLanes(bias,biasValues);probe.uploadLanes(scale,scales);
        std::vector<float> references[2];
        auto run=[&](UINT policy){
            input->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);bias->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);scale->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);output->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx->cmd->SetComputeRootSignature(root.Get());ctx->cmd->SetPipelineState(pipelines[policy].Get());
            ctx->cmd->SetComputeRootShaderResourceView(0,input->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(1,bias->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(4,scale->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootUnorderedAccessView(2,output->resource->GetGPUVirtualAddress());
            UINT constants[]={2*heads,heads,UINT(input->elements),UINT(output->elements)};ctx->cmd->SetComputeRoot32BitConstants(3,4,constants,0);ctx->cmd->Dispatch(2*heads,1,2);uavBarrier(ctx->cmd.Get());probe.submit();return probe.downloadLanes(output);
        };
        for(UINT policy=0;policy<4;++policy){
            output=policy>=2?normOutput:contextOutput;
            probe.uploadLanes(input,values);std::vector<float> first;
            for(UINT repeat=0;repeat<3;++repeat){auto actual=run(policy);
                for(float v:actual)if(!std::isfinite(v))throw std::runtime_error("Nonfinite partition unit");
                if(!repeat)first=actual;else if(memcmp(first.data(),actual.data(),actual.size()*4))throw std::runtime_error("Partition unit repeat mismatch");}
            if(policy%2==0)references[policy/2]=first;else if(memcmp(first.data(),references[policy/2].data(),first.size()*4))throw std::runtime_error("Wave normalization differs from serial reference");
            auto changed=values;UINT selected=heads-1;
            for(UINT token=64;token<128;++token)for(UINT plane=0;plane<3;++plane)for(UINT c=0;c<32;++c)changed[size_t(input->elements)+token*channels*3+plane*channels+selected*32+c]=changed[size_t(input->elements)+token*channels*3+plane*channels+selected*32+c]*-.75f+.125f;
            probe.uploadLanes(input,changed);auto alt=run(policy);bool affected=false;
            for(size_t i=0;i<first.size();++i){bool target=i>=output->elements&&((i-output->elements)/(channels*(policy>=2?2:1)))>=64&&(i%channels)/32==selected;
                if(!target&&memcmp(&first[i],&alt[i],4))throw std::runtime_error("Partition batch/window/head contamination");
                if(target&&first[i]!=alt[i])affected=true;}
            if(!affected)throw std::runtime_error("Partition unit insensitive to changed values");
            std::string name=folder+"/pattern"+std::to_string(pattern)+"-C"+std::to_string(channels)+"-P"+std::to_string(policy)+".bin";std::ofstream f(nativePath(name),std::ios::binary);f.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!f)throw std::runtime_error("Partition unit output");
            printf("NORM_WAVE_UNIT_PASS pattern=%u C=%u policy=%u exact_reference=1 repeat_exact=1 isolation=1\n",pattern,channels,policy);
        }
        }
    }
    puts("NORM_WAVE_UNITS_PASS cases=60");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
