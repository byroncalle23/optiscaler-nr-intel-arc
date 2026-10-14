#include "native_assets.h"
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: native_assets_probe.exe package-folder");
    auto assets=std::string(argv[1])=="--module"?NativeNrAssets::fromModule(nullptr):NativeNrAssets(nativePath(argv[1]));
    printf("{\"integrity_verified\":true,\"width\":%u,\"height\":%u}\n",assets.width(),assets.height());return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
