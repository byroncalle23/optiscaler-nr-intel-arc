#include "dml_block.h"
int main(int argc,char** argv){try{
    if(argc<2)throw std::runtime_error("Usage: dml_block.exe fixture-directory [--chain] [--capture] (run from repository root)");
    bool chain=false,capture=false;
    for(int i=2;i<argc;++i){std::string arg(argv[i]);if(arg=="--chain")chain=true;else if(arg=="--capture")capture=true;else throw std::runtime_error("Unknown argument: "+arg);}
    const int warm=chain ? 2 : 64,runs=chain ? 2 : 12;
    BlockHarness block(argv[1],chain);for(int i=0;i<warm;++i)block.run();
    char batchSetting[4]{};
    bool batchMode=GetEnvironmentVariableA("NR_BATCH_BENCHMARK",batchSetting,sizeof(batchSetting))==1 && batchSetting[0]=='1';
    if(chain)batchMode=false;
    std::vector<double> stageTimes(13,0),runTimes;std::vector<float> output,first;
    if(batchMode){runTimes=block.measure(stageTimes);output=block.download(block.output);}
    else for(int i=0;i<runs;++i){runTimes.push_back(block.run());
        for(size_t j=0;j<13;++j)stageTimes[j]+=block.lastStageMs[j];output=block.download(block.resultTensor());
        if(i==0)first=output;else if(memcmp(first.data(),output.data(),output.size()*4)!=0)throw std::runtime_error("Output is not repeatable");}
    double ms=0;for(double duration:runTimes)ms+=duration;
    std::ofstream file(std::string(argv[1])+"/native.bin",std::ios::binary);file.write(reinterpret_cast<char*>(output.data()),std::streamsize(output.size()*4));
    if(!file)throw std::runtime_error("Cannot save native result");
    for(float v:output)if(!std::isfinite(v))throw std::runtime_error("Non-finite block output");
    if(capture && chain)block.saveChainStates(argv[1]);
    if(capture && !chain){
        const std::pair<const char*,Tensor*> stages[]={{"hidden",block.hidden},{"gate",block.gatedHidden},
            {"branch",block.branch},{"feed",block.feed},{"qkv",block.qkv},{"ready",block.ready},
            {"context",block.context},{"projection",block.projected}};
        for(auto& stage:stages){auto values=block.download(stage.second);
            std::ofstream stageFile(std::string(argv[1])+"/native_"+stage.first+".bin",std::ios::binary);
            stageFile.write(reinterpret_cast<char*>(values.data()),std::streamsize(values.size()*4));
            if(!stageFile)throw std::runtime_error("Cannot write stage capture");}
    }
    printf("{\"block\":31,\"blocks\":%u,\"output_published\":%s,\"tokens\":%u,\"channels\":1024,\"gpu_mean_ms\":%.6f,\"runs\":%d,\"warm\":%d,\"finite\":true,\"repeat_byte_identical\":true,\"measurement_mode\":\"%s\",\"attention_backend\":\"%s\",\"backend\":\"D3D12 DirectML FP32 + HLSL\",\"stages_ms\":{",chain ? 8u : 1u,chain ? "true" : "false",block.tokenCount(),ms/runs,runs,warm,batchMode ? "batch12" : "single_submit",block.usesScalarAttention() ? "scalar_hlsl" : "dml_gemm");
    const char* names[]={"expand","gate","ffn_projection","ffn_residual","qkv","normalize","pack","qk","softmax","pv","merge","projection","attn_residual"};
    for(size_t j=0;j<13;++j)printf("%s\"%s\":%.6f",j ? "," : "",block.usesScalarAttention() && j==6 ? "attention_scalar" : names[j],stageTimes[j]/runs);
    printf("},\"gpu_runs_ms\":[");for(size_t j=0;j<runTimes.size();++j)printf("%s%.6f",j ? "," : "",runTimes[j]);printf("]}\n");
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
