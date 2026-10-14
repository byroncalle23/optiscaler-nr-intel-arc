#include "dml_window.h"
int main(int argc,char** argv){try{
    if(argc<2 || argc>3 || (argc==3 && std::string(argv[2])!="--capture"))throw std::runtime_error("Usage: dml_window.exe fixture-directory [--capture]");
    WindowHarness harness(argv[1]);auto first=harness.run(),second=harness.run();
    if(memcmp(first.data(),second.data(),first.size()*4))throw std::runtime_error("Window block is not repeatable");
    for(float v:first)if(!std::isfinite(v))throw std::runtime_error("Non-finite window output");
    std::ofstream file(std::string(argv[1])+"/native.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(first.data()),std::streamsize(first.size()*4));
    if(!file)throw std::runtime_error("Cannot save window output");if(argc==3)harness.capture(argv[1]);
    printf("{\"finite\":true,\"repeat_byte_identical\":true,\"runs\":2,\"channels\":%u,\"diagnostic_stage_submits\":%s,\"compensated_split_projection\":%s,\"backend\":\"D3D12 DirectML FP32 + HLSL\"}\n",harness.channels(),harness.diagnosticIsolation() ? "true" : "false",harness.diagnosticCompensatedProjection()?"true":"false");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
