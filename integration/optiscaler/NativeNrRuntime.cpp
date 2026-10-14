#include "NativeNrRuntime.h"
#include "NativeNrHardware.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <mutex>
#include <fstream>
using Microsoft::WRL::ComPtr;
namespace {
ComPtr<ID3D12DeviceFactory> nrFactory;
ComPtr<ID3D12Device> nrDevice;
std::recursive_mutex nrDeviceMutex;
thread_local bool nrCreating=false;
}
HRESULT NativeNrInitializeRuntime(){
    static std::once_flag once;static HRESULT result=S_FALSE;
    std::call_once(once,[]{
        HMODULE module=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&NativeNrInitializeRuntime),&module)){result=HRESULT_FROM_WIN32(GetLastError());return;}
        wchar_t path[32768]{};auto count=GetModuleFileNameW(module,path,32768);
        if(!count||count>=32768){result=E_FAIL;return;}
        auto folder=std::filesystem::path(path).parent_path();if(folder.filename()==L"_storage_")folder=folder.parent_path();
        auto trace=[&](const char* stage,HRESULT hr){DWORD mode=0,size=sizeof(mode);
            auto query=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",L"AllowDevelopmentWithoutDevLicense",RRF_RT_REG_DWORD,nullptr,&mode,&size);
            std::ofstream log(folder/L"native_nr_runtime.log",std::ios::app);
            log<<"pid="<<GetCurrentProcessId()<<" tick="<<GetTickCount64()<<" developerMode="<<(query==ERROR_SUCCESS?int(mode):-1)<<" "<<stage<<" HRESULT=0x"<<std::hex<<unsigned(hr)<<"\n";};
        if(GetPrivateProfileIntW(L"NativeNR",L"GlobalB1",0,(folder/L"native_nr.ini").c_str())!=1)return;
        // Requiem has already selected its SDK before loading this component.
        // Never call SetSDKVersion or ApplyToGlobalState on that live runtime.
        ComPtr<ID3D12SDKConfiguration1> sdk;
        result=D3D12GetInterface(CLSID_D3D12SDKConfiguration,IID_PPV_ARGS(&sdk));trace("GetSDKConfiguration1",result);if(FAILED(result))return;
        auto runtime=(folder/L"native_nr_runtime").string()+"\\";
        result=sdk->CreateDeviceFactory(721,runtime.c_str(),IID_PPV_ARGS(&nrFactory));trace("CreateSDK721Factory",result);if(FAILED(result))return;
        result=nrFactory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);if(FAILED(result))return;
#ifdef NR_GAME_FACTORY_PROBE
        char debugFlag[4]{};
        if(GetEnvironmentVariableA("NR_D3D12_DEBUG",debugFlag,4)==1&&debugFlag[0]=='1'){
            ComPtr<ID3D12Debug> debug;
            result=nrFactory->GetConfigurationInterface(CLSID_D3D12Debug,IID_PPV_ARGS(&debug));if(FAILED(result))return;
            debug->EnableDebugLayer();
        }
#endif
        UUID feature=D3D12ExperimentalShaderModels;
        result=nrFactory->EnableExperimentalFeatures(1,&feature,nullptr,nullptr);
        trace("FactoryExperimentalShaderModels",result);
    });return result;
}
bool NativeNrCreateDevice(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** output,HRESULT* result){
    if(!result||nrCreating||level==D3D_FEATURE_LEVEL_1_0_CORE)return false;
    ComPtr<IDXGIAdapter1> selected;
    if(adapter){if(FAILED(adapter->QueryInterface(IID_PPV_ARGS(&selected))))return false;}
    else{
        ComPtr<IDXGIFactory4> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 d{};if(candidate&&SUCCEEDED(candidate->GetDesc1(&d))&&NativeNrSupportedAdapter(d.VendorId,d.DeviceId)){selected=candidate;break;}}
    }
    if(!selected)return false;DXGI_ADAPTER_DESC1 desc{};
    if(FAILED(selected->GetDesc1(&desc))||!NativeNrSupportedAdapter(desc.VendorId,desc.DeviceId))return false;
    auto initialized=NativeNrInitializeRuntime();if(initialized==S_FALSE)return false;
    // Optional NR must not make the engine report an unsupported graphics card.
    if(FAILED(initialized)){*result=initialized;return false;}
    std::lock_guard<std::recursive_mutex> lock(nrDeviceMutex);
    struct Creating{Creating(){nrCreating=true;}~Creating(){nrCreating=false;}} creating;
    // All redirected game/library requests share this same primary device;
    // an incidental SDK616 device is never reused or mixed with its resources.
    if(!nrDevice){
        *result=nrFactory->CreateDevice(selected.Get(),level,IID_PPV_ARGS(&nrDevice));
        if(FAILED(*result))return true;
        ComPtr<ID3D12DeviceConfiguration> config;*result=nrDevice.As(&config);
        if(FAILED(*result)||config->GetDesc().SDKVersion!=721){nrDevice.Reset();*result=E_NOINTERFACE;return true;}
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_10};
        *result=nrDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm));
        if(FAILED(*result)||sm.HighestShaderModel<D3D_SHADER_MODEL_6_10){nrDevice.Reset();*result=E_NOINTERFACE;return true;}
    }
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{1,&level,D3D_FEATURE_LEVEL_1_0_CORE};
    *result=nrDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&levels,sizeof(levels));
    if(FAILED(*result)||levels.MaxSupportedFeatureLevel<level){*result=E_INVALIDARG;return true;}
    if(!output){*result=S_FALSE;return true;}
    *output=nullptr;*result=nrDevice->QueryInterface(iid,output);return true;
}
