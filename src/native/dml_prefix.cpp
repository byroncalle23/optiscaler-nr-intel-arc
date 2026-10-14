#include "dml_endpoints.h"
#include "dml_window.h"
#include "dml_transition.h"

static void save(const std::string& file,const std::vector<float>& data){
    std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*4));
    if(!out)throw std::runtime_error("Prefix result write failed");
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: dml_prefix.exe fixture-directory");
    std::string folder(argv[1]);
    EndpointHarness adapter(folder+"/adapter");auto context=adapter.sharedContext();
    WindowHarness block(folder+"/block0",context,adapter.resultTensor());
    TransitionHarness pool(folder+"/pool",context,block.resultTensor());
    if(block.diagnosticIsolation())throw std::runtime_error("Disable NR_WINDOW_ISOLATE for a resident prefix");
    std::vector<float> first,last;
    for(int frame=0;frame<2;++frame){
        adapter.record();block.record();pool.record();
        last=pool.download(pool.resultTensor());
        if(frame==0)first=last;
        else if(memcmp(first.data(),last.data(),last.size()*4))throw std::runtime_error("Prefix is not repeatable");
    }
    for(float value:last)if(!std::isfinite(value))throw std::runtime_error("Non-finite prefix output");
    save(folder+"/native.bin",last);
    // Diagnostic reads are after the complete prefix, not between its inference passes.
    save(folder+"/native_adapter.bin",adapter.download(adapter.resultTensor()));
    save(folder+"/native_block0.bin",block.download(block.resultTensor()));
    printf("{\"finite\":true,\"repeat_byte_identical\":true,\"runs\":2,\"shared_device_and_list\":true,\"cpu_intermediate_reads\":0,\"operators\":[\"adapter\",\"block0\",\"pool\"]}\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
