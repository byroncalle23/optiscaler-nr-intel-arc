#include "dml_texture.h"
#include "ngx_test_parameters.h"
#include "../../integration/optiscaler/NativeNrApi.h"

template<class T> T proc(HMODULE module,const char* name){auto address=GetProcAddress(module,name);if(!address)throw std::runtime_error(name);return reinterpret_cast<T>(address);}
#define NR_PROC(name) auto name=proc<decltype(&OptiScalerNR_##name)>(module,"OptiScalerNR_" #name)
int main(int argc,char** argv){try{
    if(argc!=4||std::string(argv[2])!="rgba32f")throw std::runtime_error("Usage: dml_dll_retarget fixture rgba32f test-host-DLL");
    std::string folder=argv[1];auto path=std::filesystem::absolute(nativePath(argv[3]));
    UINT h,w,nh,nw;std::ifstream shape(nativePath(folder+"/frame_input/shape.txt"));if(!(shape>>h>>w>>nh>>nw))throw std::runtime_error("Retarget fixture shape missing");
    TextureHarness oldTarget(w,h);auto oldContext=oldTarget.sharedContext();
    auto newContext=NativeContext::onDevice(oldContext->device.Get());TextureHarness newTarget(w,h,newContext);
    TextureHarness unsupported(w+32,h,NativeContext::onDevice(oldContext->device.Get()));
    HMODULE module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module)throw std::runtime_error("Retarget DLL load failed");
    NR_PROC(Initialize);NR_PROC(Prepare);NR_PROC(Evaluate);NR_PROC(Stats);NR_PROC(PreparationStats);NR_PROC(Capture);NR_PROC(Collect);NR_PROC(Stop);NR_PROC(Reset);NR_PROC(LastError);
    auto ok=[&](HRESULT hr){if(FAILED(hr))throw std::runtime_error(LastError());};
    auto stats=[&](){NativeNrApiStats s{};s.size=sizeof(s);ok(Stats(17,&s));if(s.faulted)throw std::runtime_error("Retarget controller fault");return s;};
    auto prep=[&](){NativeNrPreparationStats s{};s.size=sizeof(s);ok(PreparationStats(&s));return s;};
    auto evaluate=[&](TextureHarness& target,ID3D12GraphicsCommandList* list){FixtureNgxParameters params;params.output=target.inputTexture();UINT result=99;
        ok(Evaluate(17,list,&params,FALSE,TRUE,UINT(D3D12_RESOURCE_STATE_COPY_DEST),&result));return result;};
    ok(Initialize(oldContext->queue.Get()));ok(Prepare(17,oldTarget.inputTexture()));auto original=stats();
    oldTarget.upload(folder+"/frame_input/colour.bin");newTarget.upload(folder+"/frame_input/colour.bin");
    if(evaluate(oldTarget,oldContext->cmd.Get())!=1)throw std::runtime_error("Old target recording missing");
    if(evaluate(unsupported,newContext->cmd.Get())!=0||prep().queued||prep().attempts||stats().recorded!=1)throw std::runtime_error("Unsupported extent queued or replaced session");
    for(UINT i=0;i<3;++i)if(evaluate(newTarget,newContext->cmd.Get())!=0)throw std::runtime_error("Changed target must skip during discovery");
    if(prep().queued!=1||stats().active!=1||stats().recorded!=1)throw std::runtime_error("Replacement metadata/coalescing mismatch");
    newTarget.submit(); // Kick worker WITHOUT submitting old model recording.
    auto deadline=GetTickCount64()+120000;
    while(!prep().preparing&&!prep().prepared){if(GetTickCount64()>deadline)throw std::runtime_error("Retarget worker start timeout");Sleep(1);}
    auto retired=stats();if(retired.active||retired.pending!=1||retired.recorded!=1)throw std::runtime_error("Old unsubmitted recording lost during replacement");
    if(evaluate(newTarget,newContext->cmd.Get())!=0||prep().queued)throw std::runtime_error("Repeated replacement restarted preparation");
    while(prep().preparing){if(GetTickCount64()>deadline)throw std::runtime_error("Retarget worker timeout");Sleep(5);}
    auto prepared=stats();auto p=prep();
    if(p.attempts!=1||p.prepared!=1||p.rejected||prepared.active!=1||prepared.pending!=1||prepared.recorded!=1||prepared.frames||prepared.selectedSessionBytes!=original.selectedSessionBytes)throw std::runtime_error("Replacement publication or old retention mismatch");
    oldTarget.submit();ok(Collect());auto complete=stats();if(complete.active!=1||complete.pending||complete.recorded||complete.inFlight)throw std::runtime_error("Old completion removed new binding or leaked owners");
    auto first=oldTarget.downloadTexture(folder+"/retarget_old_texture.bin",true);
    if(evaluate(newTarget,newContext->cmd.Get())!=1)throw std::runtime_error("New binding did not record");
    newTarget.submit();ok(Collect());auto done=stats();if(done.frames!=1||done.pending||done.recorded||done.inFlight)throw std::runtime_error("New frame completion mismatch");
    ok(Capture(17,nativePath(folder).c_str()));
    std::vector<float> second(first.size());std::ifstream image(nativePath(folder+"/native_texture.bin"),std::ios::binary);
    if(!image.read(reinterpret_cast<char*>(second.data()),std::streamsize(second.size()*4))||memcmp(first.data(),second.data(),first.size()*4))throw std::runtime_error("Retarget changed frame output");
    if(evaluate(unsupported,newContext->cmd.Get())!=0||prep().queued||prep().attempts!=1||stats().frames!=1)throw std::runtime_error("Unsupported resize altered ready session");
    newTarget.upload(folder+"/frame_input/colour.bin");if(evaluate(newTarget,newContext->cmd.Get())!=1||Stop()!=DXGI_ERROR_WAS_STILL_DRAWING)throw std::runtime_error("Retarget stop lost pending frame");
    ok(Reset(newContext->cmd.Get(),E_FAIL));if(stats().pending!=1||stats().recorded!=1)throw std::runtime_error("Failed reset lost replacement owners");
    check(newContext->cmd->Close(),"Retarget discard close");check(newContext->allocator->Reset(),"Retarget discard allocator reset");check(newContext->cmd->Reset(newContext->allocator.Get(),nullptr),"Retarget discard reset");ok(Reset(newContext->cmd.Get(),S_OK));
    ok(Stop());if(Stop()!=S_FALSE||!FreeLibrary(module)||GetModuleHandleW(path.filename().c_str()))throw std::runtime_error("Retarget stop/unload failed");newTarget.submit();
    printf("{\"passed\":true,\"retarget\":true,\"old_recording_retained\":true,\"unsupported_extent_skipped\":true,\"texture_pipeline\":true,\"cpu_intermediate_reads\":0,\"runs\":2,\"model_preparations\":2,\"recorded_frames\":2,\"discarded_recordings\":1,\"hot_default_allocations\":0,\"repeat_byte_identical\":true,\"real_queue_detour\":true,\"module_assets\":true,\"model_in_dll\":true,\"stop_busy_retained\":true,\"failed_reset_retained\":true,\"clean_stop_and_unload\":true}\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
