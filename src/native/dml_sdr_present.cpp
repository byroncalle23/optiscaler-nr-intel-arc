#include "dml_texture.h"
#include "nr_present_bench.h"
#include "../../integration/optiscaler/NativeNrApi.h"
#include <filesystem>
#include <thread>

template<class T> T symbol(HMODULE module,const char* name){auto p=GetProcAddress(module,name);if(!p)throw std::runtime_error(name);return reinterpret_cast<T>(p);}
int main(int argc,char** argv){try{
    if(argc!=3&&argc!=4&&argc!=6&&argc!=7)throw std::runtime_error("Usage: dml_sdr_present.exe dxgi.dll fixture-folder [rgb10a2 width height --burst]");
    UINT width=argc>=6?UINT(std::stoul(argv[4])):384,height=argc>=6?UINT(std::stoul(argv[5])):216;
    bool burst=argc==7&&std::string(argv[6])=="--burst";
    bool disabled=argc==7&&std::string(argv[6])=="--disabled";
    DXGI_FORMAT format=argc>=4&&std::string(argv[3])=="rgb10a2"?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    std::string folder=argv[2];TextureHarness caller(width,height,nullptr,format,nullptr,nullptr,320,320);
    caller.uploadPacked(folder+"/colour_texture.bin");auto context=caller.sharedContext();
    auto module=LoadLibraryW(std::filesystem::absolute(nativePath(argv[1])).c_str());if(!module)throw std::runtime_error("Load SDR DLL");
    using CreateFactory=HRESULT(WINAPI*)(UINT,REFIID,void**);
    auto create=symbol<CreateFactory>(module,"CreateDXGIFactory2");ComPtr<IDXGIFactory2> factory;check(create(0,IID_PPV_ARGS(&factory)),"SDR proxy factory");
    HWND window=CreateWindowExW(0,L"STATIC",L"NR SDR Present bank",WS_OVERLAPPEDWINDOW,0,0,384,216,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!window)throw std::runtime_error("Hidden window");
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=format;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;check(factory->CreateSwapChainForHwnd(context->queue.Get(),window,&desc,nullptr,nullptr,&chain),"Automatic SDR swapchain");
    ComPtr<IDXGISwapChain3> chain3;check(chain.As(&chain3),"SDR chain3");check(chain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"Actual SDR color space");
    auto prep=symbol<decltype(&OptiScalerNR_PreparationStats)>(module,"OptiScalerNR_PreparationStats");
    auto stop=symbol<decltype(&OptiScalerNR_Stop)>(module,"OptiScalerNR_Stop");
    auto presenter=std::make_unique<NativeNrPresentBench>(context,chain.Get());
    presenter->submitAndPresent(caller.inputTexture(),folder+"/sdr_initial.bin",true);
    if(disabled){
        for(UINT i=0;i<3;++i)presenter->submitAndPresent(caller.inputTexture(),folder+"/sdr_disabled"+std::to_string(i)+".bin",true);
        NativeNrPreparationStats disabledState{};disabledState.size=sizeof(disabledState);
        if(SUCCEEDED(prep(&disabledState)))throw std::runtime_error("Disabled startup initialized the model controller");
        auto readDisabled=[&](const std::string& name){std::ifstream f(nativePath(folder+"/"+name),std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Disabled capture missing");std::vector<char> data(size_t(f.tellg()));f.seekg(0);f.read(data.data(),std::streamsize(data.size()));return data;};
        auto input=readDisabled("colour_texture.bin");
        if(readDisabled("sdr_initial.bin")!=input)throw std::runtime_error("Disabled startup changed pixels");
        for(UINT i=0;i<3;++i)if(readDisabled("sdr_disabled"+std::to_string(i)+".bin")!=input)throw std::runtime_error("Disabled Present changed pixels");
        presenter.reset();check(chain->ResizeBuffers(2,width,height,format,0),"Disabled resize");
        chain3.Reset();chain.Reset();factory.Reset();DestroyWindow(window);
        puts("{\"passed\":true,\"disabled_startup\":true,\"controller_not_initialized\":true,\"four_frames_byte_exact\":true,\"resize_passed\":true}");return 0;
    }
    NativeNrPreparationStats state{};state.size=sizeof(state);auto deadline=GetTickCount64()+120000;
    do{check(prep(&state),"Initial preparation start");if(GetTickCount64()>deadline)throw std::runtime_error("No preparation start");if(!state.preparing&&!state.prepared)Sleep(10);}while(!state.preparing&&!state.prepared);
    if(!state.preparing)throw std::runtime_error("Preparation unexpectedly finished before cancellation test");
    presenter.reset();check(chain->ResizeBuffers(2,width,height,format,0),"Resize during preparation");
    presenter=std::make_unique<NativeNrPresentBench>(context,chain.Get());
    presenter->submitAndPresent(caller.inputTexture(),folder+"/sdr_after_prepare_resize.bin",true);
    do{check(prep(&state),"SDR preparation state");if(state.rejected||GetTickCount64()>deadline)throw std::runtime_error("SDR automatic preparation failed");if(!state.prepared)Sleep(25);}while(!state.prepared);
    if(state.prepared!=1||state.attempts!=2)throw std::runtime_error("Preparation cancellation/replacement mismatch");
    presenter->submitAndPresent(caller.inputTexture(),folder+"/sdr_frame0.bin",true);
    presenter->submitAndPresent(caller.inputTexture(),folder+"/sdr_frame1.bin",true);
    auto read=[&](const std::string& name){std::ifstream f(nativePath(folder+"/"+name),std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Missing capture");std::vector<char> data(size_t(f.tellg()));f.seekg(0);f.read(data.data(),std::streamsize(data.size()));return data;};
    auto original=read("colour_texture.bin"),first=read("sdr_frame0.bin"),second=read("sdr_frame1.bin");
    if(first!=second||first==original)throw std::runtime_error("SDR model did not modify both buffers identically");
    auto changedInput=original;
    for(size_t i=0;i<changedInput.size();i+=4){uint32_t pixel;memcpy(&pixel,changedInput.data()+i,4);
        pixel^=format==DXGI_FORMAT_R10G10B10A2_UNORM?0x1ff00000u:0x00550000u;memcpy(changedInput.data()+i,&pixel,4);}
    {std::ofstream alternate(nativePath(folder+"/alternate_texture.bin"),std::ios::binary);alternate.write(changedInput.data(),std::streamsize(changedInput.size()));if(!alternate)throw std::runtime_error("Changed fixture write failed");}
    caller.uploadPacked(folder+"/alternate_texture.bin");
    presenter->submitAndPresent(caller.inputTexture(),folder+"/sdr_changed0.bin",true);
    presenter->submitAndPresent(caller.inputTexture(),folder+"/sdr_changed1.bin",true);
    auto changed=read("sdr_changed0.bin");if(changed==first||changed!=read("sdr_changed1.bin"))throw std::runtime_error("Normal DLL cached output ignored changed input or differs between buffers");
    caller.uploadPacked(folder+"/colour_texture.bin");
    if(burst){
        ComPtr<ID3D12Fence> gate;check(context->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"Burst diagnostic gate");
        check(context->queue->Wait(gate.Get(),1),"Burst queued GPU wait");
        struct ReleaseGate {std::thread thread;~ReleaseGate(){if(thread.joinable())thread.join();}} release;
        release.thread=std::thread([gate](){Sleep(3000);gate->Signal(1);});
        std::vector<ComPtr<ID3D12CommandAllocator>> allocators;std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
        std::vector<ComPtr<ID3D12Resource>> buffers;
        for(UINT i=0;i<2;++i){
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> target;
            check(context->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Burst allocator");
            check(context->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Burst list");
            check(chain3->GetBuffer(chain3->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&target)),"Burst buffer");
            transition(list.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(target.Get(),caller.inputTexture());
            transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
            transition(list.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            check(list->Close(),"Burst close");ID3D12CommandList* submitted[]={list.Get()};context->queue->ExecuteCommandLists(1,submitted);
            auto hr=chain->Present(0,i==0?DXGI_PRESENT_DO_NOT_WAIT:0);
            if(hr!=S_OK&&hr!=DXGI_STATUS_OCCLUDED&&!(i==0&&hr==DXGI_ERROR_WAS_STILL_DRAWING))throw std::runtime_error("Burst Present failed");
            allocators.push_back(allocator);lists.push_back(list);buffers.push_back(target);
        }
        caller.submit();
    }
    // Drop bank references before actual resize. The production callback retires
    // the session and releases its backbuffer owners before calling DXGI.
    presenter.reset();check(chain->ResizeBuffers(2,width,height,format,0),"SDR resize after model");
    check(stop(),"SDR stop");chain3.Reset();chain.Reset();factory.Reset();DestroyWindow(window);
    printf("{\"passed\":true,\"automatic_queue_capture\":true,\"prepare_api_called\":false,\"evaluate_api_called\":false,\"model_preparations\":1,\"preparation_attempts\":2,\"present_calls\":%u,\"processed_buffers\":4,\"burst_frames\":%u,\"outputs_byte_identical\":true,\"output_changed\":true,\"changed_input_verified\":true,\"resize_during_preparation\":true,\"resize_after_inference\":true,\"sdr_color_space\":true,\"game_test\":false}\n",burst?8:6,burst?2:0);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
