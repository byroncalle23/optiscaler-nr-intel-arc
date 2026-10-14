#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: softmax_wave_unit output fixtures");
    std::string folder=argv[1];std::filesystem::create_directories(nativePath(folder));
    BatchGraphProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=2;
    linAlgCheckWaveFP16(ctx->device.Get());auto root=linAlgWindowRoot(ctx->device.Get());
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    for(const char* name:{"attention-norm-wave8.dxil","attention-softmax-wave8.dxil","softmax-capture0.dxil","softmax-capture1.dxil"})pipelines.push_back(linAlgPso(ctx->device.Get(),root.Get(),name));
    { // Independent FP64 positive-sum bound; rounding/publication checked in Python.
        ctx->tileBatch=1;auto injected=probe.make(4096),sumOutput=probe.make(4224);
        auto serial=linAlgPso(ctx->device.Get(),root.Get(),"softmax-sum0.dxil"),wave=linAlgPso(ctx->device.Get(),root.Get(),"softmax-sum1.dxil");
        for(UINT fixture=0;fixture<6;++fixture){
            std::ifstream f(nativePath(std::string(argv[2])+"/terms"+std::to_string(fixture)+".bin"),std::ios::binary);
            std::vector<float> values(4096);if(!f.read(reinterpret_cast<char*>(values.data()),values.size()*4))throw std::runtime_error("Sum fixture missing");probe.uploadLanes(injected,values);
            for(UINT policy=0;policy<2;++policy){std::vector<float> first;
                for(UINT repeat=0;repeat<3;++repeat){
                    injected->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);sumOutput->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    ctx->cmd->SetComputeRootSignature(root.Get());ctx->cmd->SetPipelineState(policy?wave.Get():serial.Get());
                    ctx->cmd->SetComputeRootShaderResourceView(0,injected->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(1,injected->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(4,injected->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootUnorderedAccessView(2,sumOutput->resource->GetGPUVirtualAddress());
                    UINT constants[]={64,1,4096,4224};ctx->cmd->SetComputeRoot32BitConstants(3,4,constants,0);ctx->cmd->Dispatch(1,1,1);uavBarrier(ctx->cmd.Get());probe.submit();auto actual=probe.downloadLanes(sumOutput);
                    for(float value:actual)if(!std::isfinite(value))throw std::runtime_error("Nonfinite sum unit");
                    for(UINT row=0;row<64;++row){double exact=0;for(UINT key=0;key<64;++key)exact+=values[row*64+key];
                        if(exact<=0||std::abs(double(actual[4096+row])-exact)>exact*0.000008)throw std::runtime_error("Independent FP64 sum bound failed");}
                    if(!repeat)first=actual;else if(memcmp(first.data(),actual.data(),actual.size()*4))throw std::runtime_error("Sum repeat mismatch");
                }
                std::ofstream out(nativePath(folder+"/sum"+std::to_string(fixture)+"-P"+std::to_string(policy)+".bin"),std::ios::binary);out.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!out)throw std::runtime_error("Sum output");
                printf("SOFTMAX_SUM_PASS fixture=%u policy=%u fp64_bound=1 repeat_exact=1\n",fixture,policy);
            }
        }
        ctx->tileBatch=2;
    }
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
            if(policy%2==0)references[policy/2]=first;
            double relative=0;
            if(policy%2){double diff=0,energy=0;auto& ref=references[policy/2];for(size_t i=0;i<first.size();++i){double delta=double(first[i])-ref[i];diff+=delta*delta;energy+=double(ref[i])*ref[i];}relative=sqrt(diff/std::max(energy,1e-30));}
            auto changed=values;UINT selected=heads-1;
            for(UINT token=64;token<128;++token)for(UINT plane=0;plane<3;++plane)for(UINT c=0;c<32;++c)changed[size_t(input->elements)+token*channels*3+plane*channels+selected*32+c]=changed[size_t(input->elements)+token*channels*3+plane*channels+selected*32+c]*-.75f+.125f+float(int(token%5)-2)/8;
            probe.uploadLanes(input,changed);auto alt=run(policy);bool affected=false;
            for(size_t i=0;i<first.size();++i){bool target=policy>=2?i>=output->elements+UINT64(2*heads-1)*4096:(i>=output->elements&&((i-output->elements)/channels)>=64&&(i%channels)/32==selected);
                if(!target&&memcmp(&first[i],&alt[i],4))throw std::runtime_error("Partition batch/window/head contamination");
                if(target&&first[i]!=alt[i])affected=true;}
            if(!affected)throw std::runtime_error("Partition unit insensitive to changed values");
            std::string name=folder+"/pattern"+std::to_string(pattern)+"-C"+std::to_string(channels)+"-P"+std::to_string(policy)+".bin";std::ofstream f(nativePath(name),std::ios::binary);f.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!f)throw std::runtime_error("Partition unit output");
            printf("SOFTMAX_UNIT_PASS pattern=%u C=%u policy=%u rmse=%.9g repeat_exact=1 isolation=1\n",pattern,channels,policy,relative);
        }
        }
    }
    puts("SOFTMAX_UNITS_PASS cases=60");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
