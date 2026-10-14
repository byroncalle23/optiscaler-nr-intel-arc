#include "dml_texture.h"
#include "ngx_test_parameters.h"
#include "../../integration/optiscaler/NativeNrApi.h"
#include <thread>
#include <chrono>

template<class T> T entry(HMODULE dll,const char* name){auto p=GetProcAddress(dll,name);if(!p)throw std::runtime_error(name);return reinterpret_cast<T>(p);}
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: dml_game_proxy.exe dxgi.dll 1440p-rgba8-fixture-folder");
    TextureHarness first(2560,1440,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,nullptr,320,320);
    auto context=first.sharedContext();first.uploadPacked(std::string(argv[2])+"/colour_texture.bin");
    TextureHarness second(2560,1440,context,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,nullptr,320,320);second.uploadPacked(std::string(argv[2])+"/colour_texture.bin");
    auto dll=LoadLibraryW(std::filesystem::absolute(nativePath(argv[1])).c_str());if(!dll)throw std::runtime_error("Load native proxy DLL failed");
    auto configure=entry<decltype(&OptiScalerNR_ConfigureQueue)>(dll,"OptiScalerNR_ConfigureQueue");
    auto evaluate=entry<decltype(&OptiScalerNR_Evaluate)>(dll,"OptiScalerNR_Evaluate");
    auto stats=entry<decltype(&OptiScalerNR_Stats)>(dll,"OptiScalerNR_Stats");
    auto preparation=entry<decltype(&OptiScalerNR_PreparationStats)>(dll,"OptiScalerNR_PreparationStats");
    auto stop=entry<decltype(&OptiScalerNR_Stop)>(dll,"OptiScalerNR_Stop");
    check(configure(context->queue.Get()),"Configure automatic NR queue");
    FixtureNgxParameters parameters;parameters.output=first.inputTexture();UINT result=~0u;
    auto run=[&](){check(evaluate(17,context->cmd.Get(),&parameters,FALSE,TRUE,D3D12_RESOURCE_STATE_COPY_DEST,&result),"Evaluate proxy");};
    run();if(result!=0)throw std::runtime_error("First observation must skip until ready");first.submit();
    NativeNrApiStats state{};state.size=sizeof(state);
    for(unsigned i=0;i<2400;++i){check(stats(17,&state),"Read proxy state");if(state.faulted)throw std::runtime_error("Proxy fault");if(state.active==1)break;std::this_thread::sleep_for(std::chrono::milliseconds(50));}
    if(state.active!=1)throw std::runtime_error("Automatic proxy preparation timeout");
    auto started=std::chrono::steady_clock::now();run();if(result!=1)throw std::runtime_error("First proxy frame not recorded");first.submit();
    // Caller submit waits on its own fence; DLL observer tracks the model fence.
    auto firstOutput=first.downloadTexture("",true);
    parameters.output=second.inputTexture();run();if(result!=1)throw std::runtime_error("Rotated compatible buffer not recorded");second.submit();
    auto secondOutput=second.downloadTexture("",true);
    check(stats(17,&state),"Read completed proxy state");
    NativeNrPreparationStats prepared{};prepared.size=sizeof(prepared);check(preparation(&prepared),"Read preparation state");
    if(state.frames!=2||prepared.prepared!=1||state.recorded||state.inFlight||state.faulted)throw std::runtime_error("Proxy rotation rebuilt or leaked recordings");
    if(firstOutput!=secondOutput)throw std::runtime_error("Rotated output differs");
    for(float value:secondOutput)if(!std::isfinite(value))throw std::runtime_error("Nonfinite proxy output");
    check(stop(),"Stop completed proxy");
    printf("{\"passed\":true,\"output_width\":2560,\"output_height\":1440,\"model_width\":320,\"model_height\":320,\"automatic_preparation\":true,\"model_preparations\":1,\"recorded_frames\":2,\"compatible_rotation\":true,\"rotation_byte_identical\":true,\"finite\":true,\"cpu_intermediate_reads\":0,\"diagnostic_wall_ms\":%.3f,\"game_test\":false}\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
    // Normal OptiScaler keeps stock hooks until process exit.
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
