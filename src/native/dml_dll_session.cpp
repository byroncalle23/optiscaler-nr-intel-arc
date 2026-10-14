#include "dml_texture.h"
#include "ngx_test_parameters.h"
#include "nr_present_bench.h"
#include "../../integration/optiscaler/NativeNrApi.h"

static const char* resizePhase="startup";
static ID3D12InfoQueue* resizeDiagnostics=nullptr;
static LONG CALLBACK ResizeExceptionTrace(EXCEPTION_POINTERS* info){
    if(info->ExceptionRecord->ExceptionCode==0x87d){
        fprintf(stderr,"Resize native exception 0x87d, phase=%s\n",resizePhase);
        if(resizeDiagnostics){auto messages=resizeDiagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
            for(UINT64 i=messages>5?messages-5:0;i<messages;++i){SIZE_T bytes=0;resizeDiagnostics->GetMessage(i,nullptr,&bytes);
                std::vector<unsigned char> storage(bytes);auto message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if(SUCCEEDED(resizeDiagnostics->GetMessage(i,message,&bytes)))fprintf(stderr,"D3D12 diagnostic id=%u: %s\n",UINT(message->ID),message->pDescription);}}
        void* frames[40]{};auto count=CaptureStackBackTrace(0,40,frames,nullptr);
        for(USHORT i=0;i<count;++i){HMODULE module=nullptr;wchar_t path[MAX_PATH]{};
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(frames[i]),&module))GetModuleFileNameW(module,path,MAX_PATH);
            fprintf(stderr,"  %ls + 0x%llx\n",path,static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(frames[i])-reinterpret_cast<UINT_PTR>(module)));}
        fflush(stderr);
    }return EXCEPTION_CONTINUE_SEARCH;
}

template<class T> T symbol(HMODULE module,const char* name){auto pointer=GetProcAddress(module,name);if(!pointer)throw std::runtime_error(std::string("DLL export missing: ")+name);return reinterpret_cast<T>(pointer);}
struct Api {
    decltype(&OptiScalerNR_Initialize) init;
    decltype(&OptiScalerNR_Prepare) prepare;
    decltype(&OptiScalerNR_Evaluate) evaluate;
    decltype(&OptiScalerNR_Reset) reset;
    decltype(&OptiScalerNR_Retire) retire;
    decltype(&OptiScalerNR_Collect) collect;
    decltype(&OptiScalerNR_Stats) stats;
    decltype(&OptiScalerNR_Capture) capture;
    decltype(&OptiScalerNR_Stop) stop;
    decltype(&OptiScalerNR_LastError) error;
    decltype(&OptiScalerNR_PreparationStats) preparation;
    decltype(&OptiScalerNR_ConfigureQueue) configure;
    explicit Api(HMODULE module):init(symbol<decltype(init)>(module,"OptiScalerNR_Initialize")),prepare(symbol<decltype(prepare)>(module,"OptiScalerNR_Prepare")),
        evaluate(symbol<decltype(evaluate)>(module,"OptiScalerNR_Evaluate")),reset(symbol<decltype(reset)>(module,"OptiScalerNR_Reset")),
        retire(symbol<decltype(retire)>(module,"OptiScalerNR_Retire")),collect(symbol<decltype(collect)>(module,"OptiScalerNR_Collect")),stats(symbol<decltype(stats)>(module,"OptiScalerNR_Stats")),
        capture(symbol<decltype(capture)>(module,"OptiScalerNR_Capture")),stop(symbol<decltype(stop)>(module,"OptiScalerNR_Stop")),error(symbol<decltype(error)>(module,"OptiScalerNR_LastError")),preparation(symbol<decltype(preparation)>(module,"OptiScalerNR_PreparationStats")),configure(symbol<decltype(configure)>(module,"OptiScalerNR_ConfigureQueue")){}
    void ok(HRESULT result){if(FAILED(result))throw std::runtime_error(std::string("NR DLL error: ")+error());}
    NativeNrApiStats state(){NativeNrApiStats value{};value.size=sizeof(value);ok(stats(17,&value));if(value.version!=1||value.faulted)throw std::runtime_error("NR DLL stats/fault mismatch");return value;}
};
int main(int argc,char** argv){try{
    if(argc<4||argc>5)throw std::runtime_error("Usage: dml_dll_session.exe fixture-folder rgba32f|rgba16f|rgba8 OptiScaler.dll [--normal-lifetime]");
    bool configured=argc==5&&std::string(argv[4])=="--configured-prepare";
    bool resizeRetarget=argc==5&&std::string(argv[4])=="--swapchain-resize-retarget";
    bool resizing=resizeRetarget||(argc==5&&std::string(argv[4])=="--swapchain-resize");
    if(resizing)AddVectoredExceptionHandler(1,ResizeExceptionTrace);
    bool presenting=resizing||(argc==5&&std::string(argv[4])=="--swapchain-present");
    bool swapchain=presenting||(argc==5&&(std::string(argv[4])=="--swapchain-prepare"||std::string(argv[4])=="--swapchain-disabled"));
    bool swapDisabled=swapchain&&std::string(argv[4])=="--swapchain-disabled";
    bool disabled=argc==5&&std::string(argv[4])=="--configured-disabled";
    bool invalid=argc==5&&std::string(argv[4])=="--configured-invalid";
    bool normal=swapchain||configured||disabled||invalid||(argc==5&&std::string(argv[4])=="--normal-lifetime");
    bool retireOnly=argc==5&&std::string(argv[4])=="--auto-retire";
    bool cancel=retireOnly||(argc==5&&std::string(argv[4])=="--auto-cancel");
    bool automatic=swapchain||configured||cancel||(argc==5&&std::string(argv[4])=="--auto-prepare");
    if(argc==5&&!normal&&!automatic)throw std::runtime_error("Unknown DLL mode");
    std::string folder=argv[1],formatName=argv[2];auto dll=std::filesystem::absolute(nativePath(argv[3]));
    DXGI_FORMAT format=formatName=="rgba32f"?DXGI_FORMAT_R32G32B32A32_FLOAT:formatName=="rgba16f"?DXGI_FORMAT_R16G16B16A16_FLOAT:formatName=="rgba8"?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_UNKNOWN;
    if(presenting&&format!=DXGI_FORMAT_R8G8B8A8_UNORM)throw std::runtime_error("Presentation mode requires rgba8");
    UINT h,w,nh,nw;std::ifstream config(nativePath(folder+"/frame_input/shape.txt"));if(!(config>>h>>w>>nh>>nw))throw std::runtime_error("DLL fixture geometry missing");
    TextureHarness caller(w,h,nullptr,format);auto owner=caller.sharedContext();
    TextureHarness* frameCaller=&caller;
    std::unique_ptr<TextureHarness> replacement;
    if(resizing)resizeDiagnostics=owner->diagnostics.Get();
    HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module)throw std::runtime_error("Test-host DLL load failed");Api api(module);
    ComPtr<IDXGIFactory2> swapFactory;ComPtr<IDXGISwapChain1> realSwapchain;HWND hiddenWindow=nullptr;
    std::unique_ptr<NativeNrPresentBench> presenter;
    UINT presentedResults[2]{},presentedIndices[2]{},fallbackResult=99,fallbackIndex=99;
    if(swapchain){
        NativeNrApiStats initial{};initial.size=sizeof(initial);
        if(SUCCEEDED(api.stats(17,&initial)))throw std::runtime_error("NR initialized before swapchain creation");
        using CreateFactory=HRESULT(WINAPI*)(UINT,REFIID,void**);
        auto create=symbol<CreateFactory>(module,"CreateDXGIFactory2");check(create(0,IID_PPV_ARGS(&swapFactory)),"Proxy DXGI factory");
        hiddenWindow=CreateWindowExW(0,L"STATIC",L"Native NR standalone bank",WS_OVERLAPPEDWINDOW,0,0,int(w),int(h),nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hiddenWindow)throw std::runtime_error("Hidden swapchain window failed");
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=w;desc.Height=h;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        check(swapFactory->CreateSwapChainForHwnd(owner->queue.Get(),hiddenWindow,&desc,nullptr,nullptr,&realSwapchain),"Proxy HWND swapchain");
        if(swapDisabled){
            if(SUCCEEDED(api.stats(17,&initial))||api.stop()!=S_FALSE)throw std::runtime_error("Disabled swapchain initialized NR");
            realSwapchain.Reset();swapFactory.Reset();DestroyWindow(hiddenWindow);caller.submit();
            printf("{\"passed\":true,\"real_swapchain_capture\":true,\"model_initialized\":false,\"process_lifetime_only\":true}\n");return 0;
        }
        if(api.state().active)throw std::runtime_error("Swapchain creation prepared model before output discovery");
        if(presenting)presenter=std::make_unique<NativeNrPresentBench>(owner,realSwapchain.Get());
    }
    if(disabled||invalid){
        if(api.configure(owner->queue.Get())!=(invalid?E_INVALIDARG:S_FALSE))throw std::runtime_error("Configured disabled/invalid result mismatch");
        NativeNrApiStats state{};state.size=sizeof(state);if(SUCCEEDED(api.stats(17,&state))||api.stop()!=S_FALSE)throw std::runtime_error("Disabled config initialized NR");
        caller.submit();Sleep(2000);
        printf("{\"passed\":true,\"configured_queue\":true,\"model_initialized\":false,\"invalid_config_rejected\":%s,\"process_lifetime_only\":true}\n",invalid?"true":"false");return 0;
    }
    if(configured){if(api.configure(owner->queue.Get())!=S_OK||api.configure(owner->queue.Get())!=S_FALSE)throw std::runtime_error("Configured queue initialization mismatch");}
    else if(!swapchain)api.ok(api.init(owner->queue.Get()));
    if(automatic){FixtureNgxParameters discover;discover.output=caller.inputTexture();UINT result;
        for(UINT i=0;i<3;++i){api.ok(api.evaluate(17,owner->cmd.Get(),&discover,FALSE,TRUE,UINT(D3D12_RESOURCE_STATE_COMMON),&result));if(result!=0)throw std::runtime_error("Discovery must skip NR");}
        NativeNrPreparationStats pending{};pending.size=sizeof(pending);api.ok(api.preparation(&pending));
        if(pending.queued!=1||pending.attempts||pending.prepared||api.state().active)throw std::runtime_error("Model prepared inside Evaluate or duplicate request");
        caller.submit(); // Notify worker after actual Execute; do not build on this thread.
        auto deadline=GetTickCount64()+120000;
        do{api.ok(api.preparation(&pending));if(GetTickCount64()>deadline)throw std::runtime_error("Preparation worker did not start");
            if(!pending.preparing)Sleep(1);
        }while(!pending.preparing&&!pending.prepared&&!pending.rejected);
        if(!pending.preparing||pending.prepared||api.state().active)throw std::runtime_error("First submit waited for model preparation");
        // A second real submission and API stats must finish before the model is ready.
        caller.submit();api.ok(api.preparation(&pending));
        if(!pending.preparing||pending.prepared)throw std::runtime_error("Unrelated submission blocked on model preparation");
        if(cancel){
            api.ok(api.retire(17));
            if(!retireOnly&&api.stop()!=DXGI_ERROR_WAS_STILL_DRAWING)throw std::runtime_error("Stop did not retain active preparation worker");
            do{Sleep(5);api.ok(api.preparation(&pending));if(GetTickCount64()>deadline)throw std::runtime_error("Cancelled worker timeout");}while(pending.preparing);
            auto state=api.state();if(pending.queued||pending.attempts!=1||pending.prepared||pending.rejected||state.active||state.pending||state.recorded||state.inFlight)throw std::runtime_error("Cancelled session was published or retained");
            api.ok(api.stop());if(api.stop()!=S_FALSE)throw std::runtime_error("Cancelled repeated stop mismatch");
            if(!FreeLibrary(module)||GetModuleHandleW(dll.filename().c_str()))throw std::runtime_error("Cancelled worker DLL unload failed");caller.submit();
            printf("{\"passed\":true,\"background_worker\":true,\"retire_during_preparation\":true,\"stop_during_preparation\":%s,\"stop_busy_retained\":%s,\"cancelled_result_not_published\":true,\"clean_stop_and_unload\":true}\n",retireOnly?"false":"true",retireOnly?"false":"true");return 0;
        }
        do{Sleep(5);api.ok(api.preparation(&pending));if(GetTickCount64()>deadline)throw std::runtime_error("Preparation worker timeout");}while(pending.preparing||pending.queued);
        if(pending.attempts!=1||pending.prepared!=1||pending.rejected)throw std::runtime_error("Background preparation failed");
    }else api.ok(api.prepare(17,caller.inputTexture()));
    UINT rejects=0;auto reject=[&](HRESULT result){if(SUCCEEDED(result))throw std::runtime_error("DLL contract rejection missing");++rejects;};
    reject(api.prepare(17,caller.inputTexture()));reject(api.init(owner->queue.Get()));
    auto prepared=api.state();if(prepared.active!=1||prepared.frames||prepared.recorded||prepared.inFlight)throw std::runtime_error("DLL preparation state mismatch");
    FixtureNgxParameters params;params.output=caller.inputTexture();std::vector<float> first,last;
    ComPtr<ID3D12CommandAllocator> copyAllocator;ComPtr<ID3D12GraphicsCommandList> copyList;
    check(owner->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&copyAllocator)),"DLL copy allocator");
    check(owner->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,copyAllocator.Get(),nullptr,IID_PPV_ARGS(&copyList)),"DLL copy list");
    UINT unsupported=99;api.ok(api.evaluate(17,copyList.Get(),&params,FALSE,TRUE,UINT(D3D12_RESOURCE_STATE_COMMON),&unsupported));
    if(unsupported!=2||api.state().recorded||api.state().frames)throw std::runtime_error("Unsupported list recorded DLL commands");++rejects;check(copyList->Close(),"DLL unused copy list close");
    auto evaluate=[&](BOOL hdr=FALSE,BOOL known=TRUE){UINT result=99;api.ok(api.evaluate(17,owner->cmd.Get(),&params,hdr,known,UINT(D3D12_RESOURCE_STATE_COPY_DEST),&result));return result;};
    for(UINT run=0;run<2;++run){
        if(format==DXGI_FORMAT_R32G32B32A32_FLOAT)frameCaller->upload(folder+"/frame_input/colour.bin");else frameCaller->uploadPacked(folder+"/frame_input/colour_texture.bin");
        params.voidOnly=run!=0;
        if(evaluate(TRUE)!=0||evaluate(FALSE,FALSE)!=0||evaluate()!=1||evaluate()!=0)throw std::runtime_error("DLL Evaluate guards failed");
        UINT selectedFrames=resizeRetarget&&run?1:run+1;
        auto recorded=api.state();if(recorded.recorded!=1||recorded.frames!=selectedFrames||recorded.selectedSessionBytes!=prepared.selectedSessionBytes)throw std::runtime_error("DLL hot-path state mismatch");
        reject(api.capture(17,nativePath(folder).c_str()));
        // Actual caller queue Execute is intercepted by the DLL's hook.
        if(presenting){presenter->submitAndPresent(frameCaller->inputTexture(),folder+"/present_backbuffer_"+std::to_string(run)+".bin");
            presentedResults[run]=presenter->results.back();presentedIndices[run]=presenter->indices.back();}
        else frameCaller->submit();api.ok(api.collect());auto done=api.state();
        if(done.recorded||done.inFlight||done.pending||done.frames!=selectedFrames)throw std::runtime_error("DLL completion state mismatch");
        api.ok(api.capture(17,nativePath(folder).c_str()));
        std::ifstream texture(nativePath(folder+"/native_texture.bin"),std::ios::binary);last.resize(size_t(w)*h*4);
        if(!texture.read(reinterpret_cast<char*>(last.data()),std::streamsize(last.size()*4)))throw std::runtime_error("DLL texture capture missing");
        if(!run)first=last;else if(memcmp(first.data(),last.data(),last.size()*4))throw std::runtime_error("DLL frames differ");
        if(resizing&&!run){
            // submitAndPresent has completed the caller fence. Release ALL bench
            // backbuffer references before resizing the actual wrapped swapchain.
            resizePhase="release old presenter";presenter.reset();
            resizePhase="ResizeBuffers larger";
            check(realSwapchain->ResizeBuffers(2,w+32,h,DXGI_FORMAT_R8G8B8A8_UNORM,0),"Resize larger backbuffers");
            resizePhase="acquire larger backbuffers";
            presenter=std::make_unique<NativeNrPresentBench>(owner,realSwapchain.Get());
            TextureHarness fallback(w+32,h,owner,DXGI_FORMAT_R8G8B8A8_UNORM);
            resizePhase="upload fallback";
            fallback.uploadPacked(folder+"/frame_input/resize_fallback_rgba8.bin");
            FixtureNgxParameters unsupportedParams;unsupportedParams.output=fallback.inputTexture();UINT skipped=99;
            auto before=api.state();NativeNrPreparationStats loading{};loading.size=sizeof(loading);api.ok(api.preparation(&loading));
            UINT attempts=loading.attempts,preparedCount=loading.prepared;
            api.ok(api.evaluate(17,owner->cmd.Get(),&unsupportedParams,FALSE,TRUE,UINT(D3D12_RESOURCE_STATE_COPY_DEST),&skipped));
            api.ok(api.preparation(&loading));auto unchanged=api.state();
            if(skipped!=0||loading.queued||loading.preparing||loading.attempts!=attempts||loading.prepared!=preparedCount||unchanged.active!=1||unchanged.frames!=before.frames||unchanged.selectedSessionBytes!=before.selectedSessionBytes||unchanged.recorded||unchanged.inFlight)throw std::runtime_error("Unsupported resize changed NR session");
            resizePhase="present fallback";presenter->submitAndPresent(fallback.inputTexture(),folder+"/resize_fallback_backbuffer.bin");
            fallbackResult=presenter->results.back();fallbackIndex=presenter->indices.back();
            resizePhase="release larger presenter";presenter.reset();
            resizePhase="ResizeBuffers original";
            check(realSwapchain->ResizeBuffers(2,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,0),"Restore original backbuffers");
            presenter=std::make_unique<NativeNrPresentBench>(owner,realSwapchain.Get());
            resizePhase="resumed original size";
            auto restored=api.state();api.ok(api.preparation(&loading));
            if(restored.active!=1||restored.frames!=before.frames||restored.selectedSessionBytes!=before.selectedSessionBytes||loading.attempts!=attempts||loading.prepared!=preparedCount)throw std::runtime_error("Resize back replaced retained NR session");
            if(resizeRetarget){
                resizePhase="prepare replacement output after resize";
                replacement=std::make_unique<TextureHarness>(w,h,owner,DXGI_FORMAT_R8G8B8A8_UNORM);
                if(replacement->inputTexture()==caller.inputTexture())throw std::runtime_error("Replacement did not change resource identity");
                frameCaller=replacement.get();params.output=frameCaller->inputTexture();
                frameCaller->uploadPacked(folder+"/frame_input/colour_texture.bin");
                for(UINT i=0;i<3;++i)if(evaluate()!=0)throw std::runtime_error("Resize replacement did not skip during preparation request");
                api.ok(api.preparation(&loading));
                if(loading.queued!=1||loading.attempts!=attempts||loading.prepared!=preparedCount||api.state().active!=1)throw std::runtime_error("Resize replacement request coalescing failed");
                frameCaller->submit();auto replacementDeadline=GetTickCount64()+120000;
                do{api.ok(api.preparation(&loading));if(loading.rejected)throw std::runtime_error("Resize replacement preparation failed");if(GetTickCount64()>replacementDeadline)throw std::runtime_error("Resize replacement preparation timeout");Sleep(5);}while(loading.queued||loading.preparing||loading.prepared==preparedCount);
                auto ready=api.state();
                if(loading.attempts!=attempts+1||loading.prepared!=preparedCount+1||loading.rejected||ready.active!=1||ready.pending||ready.recorded||ready.inFlight||ready.frames||ready.selectedSessionBytes!=before.selectedSessionBytes)throw std::runtime_error("Resize replacement session publication mismatch");
                resizePhase="resumed with replacement output";
            }
        }
    }
    // Shutdown while a third recording is unsubmitted: never free its owners.
    if(format==DXGI_FORMAT_R32G32B32A32_FLOAT)frameCaller->upload(folder+"/frame_input/colour.bin");else frameCaller->uploadPacked(folder+"/frame_input/colour_texture.bin");
    if(evaluate()!=1||api.stop()!=DXGI_ERROR_WAS_STILL_DRAWING)throw std::runtime_error("DLL stopped with unsubmitted work");
    api.ok(api.reset(owner->cmd.Get(),E_FAIL));auto pending=api.state();if(pending.pending!=1||pending.recorded!=1)throw std::runtime_error("Failed reset released DLL owners");
    check(owner->cmd->Close(),"DLL discard close");check(owner->allocator->Reset(),"DLL discard allocator reset");check(owner->cmd->Reset(owner->allocator.Get(),nullptr),"DLL discard list reset");
    api.ok(api.reset(owner->cmd.Get(),S_OK));auto retired=api.state();if(retired.active||retired.pending||retired.recorded||retired.inFlight)throw std::runtime_error("DLL discard retirement failed");
    api.ok(api.stop());if(api.stop()!=S_FALSE)throw std::runtime_error("DLL repeated stop mismatch");
    if(!normal&&(!FreeLibrary(module)||GetModuleHandleW(dll.filename().c_str())))throw std::runtime_error("DLL unload failed or module still retained");caller.submit();
    if(normal)printf("{\"normal_startup_inference\":true,\"dll_kept_until_process_exit\":true}\n");
    if(resizing)printf("{\"real_resize_buffers\":true,\"resize_calls\":2,\"sizes\":[[%u,%u],[%u,%u],[%u,%u]],\"fallback_nr_skipped\":true,\"fallback_preparation_unchanged\":true,\"model_preparation_unchanged\":%s,\"fallback_present_hresult\":%u,\"fallback_backbuffer_index\":%u}\n",w,h,w+32,h,w,h,resizeRetarget?"false":"true",fallbackResult,fallbackIndex);
    if(resizeRetarget)printf("{\"resize_retarget\":true,\"replacement_resource_distinct\":true,\"replacement_preparations\":1,\"coalesced_replacement_observations\":3,\"new_session_frames_before_recording\":0}\n");
    if(presenting){printf("{\"backbuffer_gpu_copy\":true,\"present_calls\":2,\"cpu_fence_wait_before_present\":false,\"cpu_readback_after_present\":true,\"present_hresult\":[%u,%u],\"backbuffer_indices\":[%u,%u]}\n",presentedResults[0],presentedResults[1],presentedIndices[0],presentedIndices[1]);presenter.reset();}
    if(swapchain){realSwapchain.Reset();swapFactory.Reset();DestroyWindow(hiddenWindow);
        printf("{\"real_swapchain_capture\":true,\"initialized_before_present\":true,\"explicit_initialize_called\":false,\"explicit_configure_called\":false}\n");}
    if(configured)printf("{\"configured_queue\":true,\"explicit_initialize_called\":false,\"repeated_configure_idempotent\":true}\n");
    if(automatic)printf("{\"automatic_preparation\":true,\"coalesced_observations\":3,\"prepared_in_evaluate\":false,\"background_worker\":true,\"submission_during_preparation\":true}\n");
    printf("{\"passed\":true,\"texture_pipeline\":true,\"cpu_intermediate_reads\":0,\"runs\":2,\"model_preparations\":%u,\"recorded_frames\":2,\"discarded_recordings\":1,\"hot_default_allocations\":0,\"owned_default_buffer_bytes\":%llu,\"repeat_byte_identical\":true,\"real_queue_detour\":true,\"module_assets\":true,\"model_in_dll\":true,\"stop_busy_retained\":true,\"failed_reset_retained\":true,\"clean_stop_and_unload\":true,\"rejections_verified\":%u}\n",resizeRetarget?2u:1u,prepared.selectedSessionBytes,rejects);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
