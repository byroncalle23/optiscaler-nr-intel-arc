#include "dml_frame.h"
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: dml_frame.exe fixture-directory");
    std::string folder=argv[1];FrameHarness harness(folder);
    std::vector<float> first,last;
    for(int run=0;run<2;++run){harness.record();last=harness.download(harness.resultTensor());
        if(run==0)first=last;else if(memcmp(first.data(),last.data(),last.size()*4))throw std::runtime_error("Frame features not repeatable");}
    for(float value:last)if(!std::isfinite(value))throw std::runtime_error("Non-finite frame features");
    std::ofstream file(folder+"/native.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(last.data()),std::streamsize(last.size()*4));
    if(!file)throw std::runtime_error("Frame output write failed");
    printf("{\"finite\":true,\"repeat_byte_identical\":true}\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
