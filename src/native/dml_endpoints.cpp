#include "dml_endpoints.h"
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: dml_endpoints.exe fixture-directory (from repository root)");
    EndpointHarness harness(argv[1]);auto first=harness.run(),second=harness.run();
    if(memcmp(first.data(),second.data(),first.size()*4))throw std::runtime_error("Endpoint result is not repeatable");
    for(float value:first)if(!std::isfinite(value))throw std::runtime_error("Non-finite endpoint output");
    std::ofstream file(std::string(argv[1])+"/native.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(first.data()),std::streamsize(first.size()*4));
    if(!file)throw std::runtime_error("Endpoint result write failed");harness.capture(argv[1]);
    printf("{\"finite\":true,\"repeat_byte_identical\":true,\"runs\":2,\"backend\":\"D3D12 DirectML FP32 + HLSL\"}\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
