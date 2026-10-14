#define main unusedSessionMain
#include "global_game_session_probe.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("Usage: attention_parity nativePackage capturedScales output");
    std::filesystem::path folder=nativePath(argv[3]);std::filesystem::create_directories(folder);
    NativeRuntime probe;auto ctx=probe.sharedContext();linAlgCheckWaveFP16(ctx->device.Get());
    auto root=linAlgWindowRoot(ctx->device.Get());
    std::vector<ComPtr<ID3D12Resource>> uploads;
    auto upload=[&](Tensor* target,const std::vector<float>& values){
        if(values.size()!=target->elements)throw std::runtime_error("Upload extent");
        auto resource=buffer(ctx->device.Get(),values.size()*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* data;D3D12_RANGE empty{0,0};check(resource->Map(0,&empty,&data),"Parity upload map");memcpy(data,values.data(),values.size()*4);resource->Unmap(0,&empty);
        target->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);ctx->cmd->CopyBufferRegion(target->resource.Get(),0,resource.Get(),0,values.size()*4);uploads.push_back(resource);
    };
    auto read=[&](const std::filesystem::path& path,size_t count){
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file||file.tellg()<std::streamoff(count*4))throw std::runtime_error("Input prefix extent");
        std::vector<float> result(count);file.seekg(0);file.read(reinterpret_cast<char*>(result.data()),count*4);
        if(!file)throw std::runtime_error("Input prefix read");for(float v:result)if(!std::isfinite(v))throw std::runtime_error("Nonfinite input");return result;
    };
    auto input=probe.make(16*64*96);auto bias=probe.make(4096,false);auto scale=probe.make(1,false);
    auto values=read(nativePath(argv[2])/"block0-qkv.bin",input->elements);
    upload(input,values);upload(bias,read(nativePath(argv[1])/"block0/bias.bin",4096));upload(scale,read(nativePath(argv[1])/"block0/scale.bin",1));
    for(int stage=-1;stage<=7;++stage){
        nrLinAlgShaderDirectory=stage<0?nativePath(argv[1])/"shaders":folder/"shaders";
        std::string shader=stage<0?"attention-softmax-wave8.dxil":"stage"+std::to_string(stage)+".dxil";
        auto pipeline=linAlgPso(ctx->device.Get(),root.Get(),shader.c_str());
        UINT extent=stage==1?6144:stage==2||stage==3?4096:stage==5?128:stage==6?4160:stage==7?12288:2048;
        auto output=probe.make(16*extent);
        upload(output,std::vector<float>(size_t(output->elements),std::numeric_limits<float>::quiet_NaN()));
        input->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);bias->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);scale->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);output->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx->cmd->SetComputeRootSignature(root.Get());ctx->cmd->SetPipelineState(pipeline.Get());
        ctx->cmd->SetComputeRootShaderResourceView(0,input->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(1,bias->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootShaderResourceView(4,scale->resource->GetGPUVirtualAddress());ctx->cmd->SetComputeRootUnorderedAccessView(2,output->resource->GetGPUVirtualAddress());
        UINT constants[]={16,1,UINT(input->elements),UINT(output->elements)};ctx->cmd->SetComputeRoot32BitConstants(3,4,constants,0);ctx->cmd->Dispatch(16,1,1);uavBarrier(ctx->cmd.Get());
        auto actual=probe.download(output);for(float v:actual)if(!std::isfinite(v))throw std::runtime_error("Missing/nonfinite diagnostic output");
        saveBytes((folder/(stage<0?"installed.bin":"stage"+std::to_string(stage)+".bin")).string(),actual.data(),actual.size()*4);
        printf("ATTENTION_PARITY_CAPTURE stage=%d elements=%zu timing_eligible=0\n",stage,actual.size());
    }
    puts("ATTENTION_PARITY_CAPTURE_PASS");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
