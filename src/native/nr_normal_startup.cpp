#include "../../integration/optiscaler/NativeNrApi.h"
#include <filesystem>
#include <cstdio>
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    auto path=std::filesystem::absolute(argv[1]);
    HMODULE module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module){fprintf(stderr,"Normal DLL startup failed: %lu\n",GetLastError());return 1;}
    auto stats=reinterpret_cast<decltype(&OptiScalerNR_Stats)>(GetProcAddress(module,"OptiScalerNR_Stats"));
    auto error=reinterpret_cast<decltype(&OptiScalerNR_LastError)>(GetProcAddress(module,"OptiScalerNR_LastError"));
    if(!stats||!error)return 1;
    NativeNrApiStats value{};value.size=sizeof(value);
    if(SUCCEEDED(stats(17,&value)))return 1; // Startup alone must not enable the model.
    // Stock OptiScaler retains automatic hooks until process termination. Do
    // not FreeLibrary or reuse patched APIs after a mid-process unload.
    Sleep(2000);
    printf("{\"passed\":true,\"normal_dllmain\":true,\"recognized_proxy_name\":true,\"nr_api_visible\":true,\"model_initialized\":false,\"process_lifetime_only\":true}\n");
    return 0;
}
