#include "pch.h"
#define NATIVE_NR_API_IMPLEMENTATION
#include "NativeNrApi.h"
#include "NativeNrHardware.h"
#include "nr_session.h"
#include "nr_submission.h"
#include "nr_pair_capture.h"
#include "nr_command_cache.h"
#include "native_assets.h"
#include "NativeNrQueueHook.h"
#include "NativeNrControl.h"
#include <proxies/Streamline_Proxy.h>
#include <condition_variable>
#include <chrono>
#include <thread>

namespace {
HRESULT resolveStreamlineNative(IUnknown* object,IUnknown** native){
    if(!native)return E_POINTER;*native=nullptr;
    // Use the loaded integration's API; do not load/init another interposer.
    auto getNative=StreamlineProxy::GetNativeInterface();
    if(!getNative)if(auto module=GetModuleHandleW(L"sl.interposer.dll"))getNative=reinterpret_cast<StreamlineProxy::PFN_slGetNativeInterface>(KernelBaseProxy::GetProcAddress_()(module,"slGetNativeInterface"));
    if(!getNative)return E_NOINTERFACE;
    static std::mutex resolveMutex;std::lock_guard<std::mutex> lock(resolveMutex);
    void* base=nullptr;auto result=getNative(object,&base);
    if(result!=sl::Result::eOk||!base)return E_NOINTERFACE;
    ComPtr<IUnknown> before,after;
    if(SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&before)))&&SUCCEEDED(static_cast<IUnknown*>(base)->QueryInterface(IID_PPV_ARGS(&after)))&&before.Get()!=after.Get()){
        static UINT resolved=0;if(++resolved<=6)LOG_INFO("Native NR Streamline native interface resolved: proxy={}, native={}",static_cast<void*>(before.Get()),static_cast<void*>(after.Get()));
    }
    *native=static_cast<IUnknown*>(base);return S_OK;
}
thread_local std::string lastError;
struct Controller : std::enable_shared_from_this<Controller> {
    NativeNrAssets assets;
    ComPtr<ID3D12Device> device;
    std::unique_ptr<NativeNrSubmissions> submissions;
    NativeNrSessions sessions;
    std::map<UINT,int> requestModes;
    std::filesystem::path modeRoot;
    std::recursive_mutex mutex;
    bool stopping=false,faulted=false;
    bool preparing=false;
    bool workerExit=false,scheduled=false;
    bool gameProxy=false;
    bool presentSdr=false;
    bool globalB1=false;
    bool temporalWanted=false;
    float residualStability=0;bool boundedResidual=false,adaptiveResidual=false,motionAwareResidual=false,edgeGuidedResidual=false;
    std::map<UINT,NativeMotionGeometry> motionRequests;
    std::map<UINT,NativeMotionGeometry> observedMotionGeometry;
    std::set<UINT> motionPreparing;
    std::map<UINT,NativeMotionGeometry> failedMotionGeometry;
    static bool sameMotionGeometry(const NativeMotionGeometry& a,const NativeMotionGeometry& b){
        auto same=[](const D3D12_RESOURCE_DESC& x,const D3D12_RESOURCE_DESC& y){return x.Dimension==y.Dimension&&x.Width==y.Width&&x.Height==y.Height&&x.Format==y.Format&&x.MipLevels==y.MipLevels&&x.DepthOrArraySize==y.DepthOrArraySize&&x.SampleDesc.Count==y.SampleDesc.Count;};
        return same(a.motion,b.motion)&&same(a.depth,b.depth)&&same(a.reactive,b.reactive);
    }
    bool synchronousPresent=false,nativeTiles=false,capturePair=false,reuseCommands=true;
    UINT captureFrames=1,captureInterval=15,captureStartFrame=0;
    std::filesystem::path captureFolder;
    std::vector<std::thread> captureWriters;
    struct PresentSlot {
        UINT handle;
        int mode=-1;
        UINT displayWidth=0,displayHeight=0;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12CommandQueue> queue;
        UINT64 serial=0,frames=0;
        UINT64 readyPresents=0,preparationSkips=0,busySkips=0,waits=0;
        UINT64 cacheBuilds=0,cacheReplays=0;
        std::shared_ptr<NativeGpuProfile> lastProfile;
        std::map<ID3D12Resource*,std::unique_ptr<NativeNrCommandCache>> caches;
        HANDLE event=nullptr;
        std::shared_ptr<NativeNrPairCapture> pair;
        UINT64 pairSerial=0;
        UINT pairGraphPasses=0,capturedPairs=0;
        UINT64 pairPresent=0,pairProducer=0;bool pairTemporal=false;
        bool pairLaunched=false;
        ~PresentSlot(){if(event)CloseHandle(event);}
    };
    UINT nextPresentHandle=0xf0000000u;
    std::map<IDXGISwapChain*,std::unique_ptr<PresentSlot>> presentSlots;
    std::set<DXGI_FORMAT> unsupportedPresentFormats;
    std::condition_variable_any wake;
    std::thread worker;
    std::map<UINT,UINT64> generations;
    UINT preparingHandle=0;
    ComPtr<ID3D12Resource> preparingTarget;
    std::map<UINT,ComPtr<ID3D12Resource>> requests;
    std::set<UINT> failedRequests;
    UINT preparationAttempts=0,preparedRequests=0,rejectedRequests=0;
    HRESULT preparationResult=S_OK;
    explicit Controller(HMODULE module,ID3D12CommandQueue* queue):assets(NativeNrAssets::fromModule(module)){
        nrNativeInterfaceResolver.store(resolveStreamlineNative);
        std::vector<wchar_t> modulePath(32768);auto length=GetModuleFileNameW(module,modulePath.data(),UINT(modulePath.size()));
        if(!length||length>=modulePath.size())throw std::runtime_error("NR config path missing");
        auto config=NativeNrAssets::moduleDirectory(module)/L"native_nr.ini";
        modeRoot=NativeNrAssets::moduleDirectory(module)/"native_nr_modes";
        gameProxy=GetPrivateProfileIntW(L"NativeNR",L"GameProxy320",0,config.c_str())==1;
        presentSdr=GetPrivateProfileIntW(L"NativeNR",L"PresentSDR",0,config.c_str())==1;
        synchronousPresent=GetPrivateProfileIntW(L"NativeNR",L"Synchronous",0,config.c_str())==1;
        nativeTiles=GetPrivateProfileIntW(L"NativeNR",L"NativeTiles",0,config.c_str())==1;
        globalB1=GetPrivateProfileIntW(L"NativeNR",L"GlobalB1",0,config.c_str())==1;
        temporalWanted=globalB1&&GetPrivateProfileIntW(L"NativeNR",L"Temporal",0,config.c_str())==1;
        auto residualPolicy=GetPrivateProfileIntW(L"NativeNR",L"ResidualStability",0,config.c_str());
        residualStability=(residualPolicy>=1&&residualPolicy<=4)?.25f:0.f;boundedResidual=residualPolicy>=2&&residualPolicy<=4;adaptiveResidual=residualPolicy>=3&&residualPolicy<=4;motionAwareResidual=residualPolicy==4;
        edgeGuidedResidual=GetPrivateProfileIntW(L"NativeNR",L"EdgeGuidedResidual",0,config.c_str())==1;
        NativeNrControl::temporalRequested.store(temporalWanted);
        if(globalB1){gameProxy=true;nativeTiles=false;if(!presentSdr)throw std::runtime_error("Global B1 requires PresentSDR=1");}
        capturePair=GetPrivateProfileIntW(L"NativeNR",L"CapturePair",0,config.c_str())==1;
        captureFrames=std::clamp<UINT>(GetPrivateProfileIntW(L"NativeNR",L"CaptureFrames",1,config.c_str()),1,8);
        captureInterval=std::clamp<UINT>(GetPrivateProfileIntW(L"NativeNR",L"CaptureInterval",15,config.c_str()),1,120);
        captureStartFrame=std::min<UINT>(GetPrivateProfileIntW(L"NativeNR",L"CaptureStartFrame",0,config.c_str()),1200);
        reuseCommands=GetPrivateProfileIntW(L"NativeNR",L"ReuseCommands",1,config.c_str())==1;
        captureFolder=NativeNrAssets::moduleDirectory(module)/L"native_nr_capture";
        if(!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)throw std::runtime_error("NR requires a DIRECT queue");
        check(queue->GetDevice(IID_PPV_ARGS(&device)),"NR queue device");submissions=std::make_unique<NativeNrSubmissions>(device.Get());
    }
    void collect(){submissions->collect();sessions.collect();
        for(auto& entry:presentSlots){auto& slot=*entry.second;
            if(slot.pair&&slot.pairSerial&&!slot.pairLaunched){auto done=slot.fence->GetCompletedValue();
                if(done!=~UINT64(0)&&done>=slot.pairSerial){auto pair=slot.pair;auto passes=slot.pairGraphPasses;auto present=slot.pairPresent,producer=slot.pairProducer;auto temporal=slot.pairTemporal;
                    auto folder=captureFolder/("pid"+std::to_string(GetCurrentProcessId())+"-handle"+std::to_string(slot.handle))/std::to_string(present);
                    captureWriters.emplace_back([pair,folder,passes,present,producer,temporal](){try{pair->save(folder,passes,present,producer,temporal);LOG_INFO("Native NR same-frame pair saved: {}",folder.string());}catch(const std::exception& e){LOG_ERROR("Native NR pair save failed: {}",e.what());}});
                    ++slot.capturedPairs;slot.pair.reset();slot.pairSerial=0;slot.pairLaunched=false;
                    if(slot.capturedPairs>=captureFrames)if(auto session=sessions.lookup(slot.handle))session->enableStabilityDiagnostic(false);
                }
            }
        }
    }
    void finishCaptures(){for(auto& writer:captureWriters)if(writer.joinable())writer.join();}
    ~Controller(){
        {std::lock_guard<std::recursive_mutex> lock(mutex);workerExit=true;wake.notify_all();}
        if(worker.joinable())worker.join();
        finishCaptures();
    }
    void startWorker(){worker=std::thread([this](){runWorker();});}
    void observe(UINT handle,ID3D12Resource* output){
        if(stopping||faulted||!output||failedRequests.count(handle))return;
        if(auto session=sessions.lookup(handle))if(session->outputResource()==output)return;
        auto desc=output->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||(!gameProxy&&(desc.Width!=assets.width()||desc.Height!=assets.height()))||!desc.Width||!desc.Height||desc.Width>3840||desc.Height>2160||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||
            (desc.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT&&desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM))return;
        ComPtr<ID3D12Device> other;check(output->GetDevice(IID_PPV_ARGS(&other)),"Observed output device");
        if(!sameNativeDevice(other.Get(),device.Get()))return;
        if(requests.size()>=4&&!requests.count(handle))return;
        if(preparing&&preparingHandle==handle&&preparingTarget.Get()==output&&!requests.count(handle))return;
        if(!requests.count(handle)||requests[handle].Get()!=output)++generations[handle];
        requests[handle]=output; // Coalesce repeated observations, retain COM metadata target.
    }
    void pump(){
        if(stopping||faulted||(requests.empty()&&motionRequests.empty()))return;
        scheduled=true;wake.notify_one();
    }
    bool frameInputs(ID3D12GraphicsCommandList* list,const NativeNrTemporal::FrameInput& f){
        if(!temporalWanted||!NativeNrIsEnabled()||stopping||faulted)return false;
        if(!NativeNrTemporal::CanReproject(f)){NativeNrControl::temporalProgress.store(0);if(f.featureFrame<6||f.featureFrame%120==0)LOG_WARN("Native NR temporal inputs unavailable: snapshot={}, convention={}, motion={}, depth={}, motionStateKnown={}, depthStateKnown={}, scalesKnown={}, jitterKnown={}, render={}x{}, display={}x{}, MVreference={}x{}",NativeNrTemporal::CanSnapshot(f),f.motionConventionVerified,f.motion.resource!=nullptr,f.depth.resource!=nullptr,f.motion.stateKnown,f.depth.stateKnown,f.scalesKnown,f.jitterKnown,f.renderWidth,f.renderHeight,f.displayWidth,f.displayHeight,f.motionReferenceWidth,f.motionReferenceHeight);return false;}
        for(auto& entry:presentSlots){auto& slot=*entry.second;
#ifdef NR_GAME_MODES
            if(slot.mode!=NativeNrModes::requested.load()||requests.count(slot.handle)||(preparing&&preparingHandle==slot.handle)||failedRequests.count(slot.handle))continue;
#endif
            if(slot.displayWidth!=f.displayWidth||slot.displayHeight!=f.displayHeight)continue;
            NativeMotionGeometry geometry{f.motion.resource->GetDesc(),f.depth.resource->GetDesc()};if(f.reactive.resource&&f.reactive.stateKnown)geometry.reactive=f.reactive.resource->GetDesc();observedMotionGeometry[slot.handle]=geometry;
            auto session=sessions.lookup(slot.handle);if(!session)continue;
            auto d=session->outputResource()->GetDesc();
            if(UINT64(d.Width)*f.displayHeight!=UINT64(d.Height)*f.displayWidth)continue;
            if(!session->motionCompatible(f)){
                auto failed=failedMotionGeometry.find(slot.handle);
                if(failed!=failedMotionGeometry.end()&&sameMotionGeometry(failed->second,geometry)){session->invalidateTemporalHistory();NativeNrControl::temporalProgress.store(3);return false;}
                motionRequests[slot.handle]=geometry;session->invalidateTemporalHistory();pump();return false;
            }
            // Single owned arena: finish the previous consumer before reusing it.
            // This waits only on a fence, never downloads image/tensor data.
            if(!session->waitReady(slot.event,120000)){session->invalidateTemporalHistory();if(f.featureFrame<6||f.featureFrame%120==0)LOG_WARN("Native NR temporal capture rejected: session not ready, handle={}, frame={}",slot.handle,f.featureFrame);return false;}
            bool recorded=session->recordEngineMotion(list,f,[&]{submissions->track(list,session);});
            NativeNrControl::temporalProgress.store(recorded?2:0);
            auto trace=session->temporalTrace();
            if(!recorded&&(f.featureFrame<6||f.featureFrame%120==0))LOG_WARN("Native NR temporal capture record rejected: handle={}, frame={}, captures={}",slot.handle,f.featureFrame,trace.captures);
            if(recorded&&(trace.captures<=6||trace.captures%120==0))LOG_INFO("Native NR temporal input: feature={}, frame={}, render={}x{}, display={}x{}, MVreference={}x{}, jittered={}, reset={}",f.handle,f.featureFrame,f.renderWidth,f.renderHeight,f.displayWidth,f.displayHeight,f.motionReferenceWidth,f.motionReferenceHeight,f.jitteredMotion,f.reset);
            return recorded;
        }
        return false;
    }
    void present(ID3D12CommandQueue* queue,IDXGISwapChain* swapchain,bool sdrOutput,UINT processingWidth,UINT processingHeight){
        std::lock_guard<std::recursive_mutex> lock(mutex);collect();
        if(!presentSdr||!gameProxy||stopping||faulted||!sdrOutput||!queue||!swapchain||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return;
        ComPtr<ID3D12Device> owner;
        check(queue->GetDevice(IID_PPV_ARGS(&owner)),"Present queue device");if(!sameNativeDevice(owner.Get(),device.Get()))return;
        ComPtr<IDXGISwapChain3> chain;check(swapchain->QueryInterface(IID_PPV_ARGS(&chain)),"Present swapchain3");
        ComPtr<ID3D12Resource> target;check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&target)),"Present backbuffer");
        auto desc=target->GetDesc();
        // Restrict initial post-tonemap route to ordinary 8-bit SDR swapchains.
        if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM){if(unsupportedPresentFormats.insert(desc.Format).second)LOG_WARN("Native NR SDR Present skipped: unsupported backbuffer format={}; bridge requires RGBA8 or RGB10A2 UNORM SDR",UINT(desc.Format));return;}
        auto& slot=presentSlots[swapchain];
        if(!slot){slot=std::make_unique<PresentSlot>();slot->handle=++nextPresentHandle;
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot->allocator)),"NR present allocator");
            check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.Get(),nullptr,IID_PPV_ARGS(&slot->list)),"NR present list");
            check(slot->list->Close(),"NR present initial close");check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&slot->fence)),"NR present fence");
            slot->event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!slot->event)throw std::runtime_error("NR present event");
            LOG_INFO("Native NR SDR backbuffer observed: {}x{}, format={}, handle={}",desc.Width,desc.Height,UINT(desc.Format),slot->handle);
        }
        auto session=sessions.lookup(slot->handle);
        slot->queue=queue;
        slot->displayWidth=processingWidth?processingWidth:UINT(desc.Width);slot->displayHeight=processingHeight?processingHeight:desc.Height;
#ifdef NR_GAME_MODES
        int desired=NativeNrModes::requested.load();
        if(slot->mode!=desired){
            slot->mode=desired;requestModes[slot->handle]=desired;++generations[slot->handle];
            failedRequests.erase(slot->handle);failedMotionGeometry.erase(slot->handle);motionRequests.erase(slot->handle);
            NativeNrModes::failure.store(0);
            requests[slot->handle]=target;NativeNrModes::active.store(-1);NativeNrControl::temporalApplied.store(false);pump();
            LOG_INFO("Native NR mode requested: mode={}, handle={}",desired,slot->handle);
        }
        if(requests.count(slot->handle)||(preparing&&preparingHandle==slot->handle)){++slot->preparationSkips;return;}
        if(failedRequests.count(slot->handle)){NativeNrControl::progress.store(3);return;}
#endif
        if(motionPreparing.count(slot->handle)){++slot->preparationSkips;return;}
        if(!session){++slot->preparationSkips;if(!(preparing&&preparingHandle==slot->handle)&&!requests.count(slot->handle)){observe(slot->handle,target.Get());pump();}return;}
        auto original=session->outputResource()->GetDesc();
        if(original.Width!=desc.Width||original.Height!=desc.Height||original.Format!=desc.Format){observe(slot->handle,target.Get());pump();return;}
        auto done=slot->fence->GetCompletedValue();if(done==~UINT64(0))throw std::runtime_error("Present device lost");
        if(!session->ready()||done<slot->serial){
            if(!synchronousPresent){++slot->busySkips;return;}
            ++slot->waits;check(slot->fence->SetEventOnCompletion(slot->serial,slot->event),"Synchronous NR marker");
            if(WaitForSingleObject(slot->event,120000)!=WAIT_OBJECT_0)throw std::runtime_error("Synchronous NR completion timed out");
            check(device->GetDeviceRemovedReason(),"Synchronous NR device");collect();
            if(!session->waitReady(slot->event,120000))throw std::runtime_error("Synchronous temporal producer/session unavailable after fence");
        }
        ++slot->readyPresents;
        auto commandStart=std::chrono::steady_clock::now();
        if(nativeTiles){
            bool selected=processingWidth&&processingHeight&&processingWidth<=3840&&processingHeight<=2160;
            UINT width=selected?processingWidth:UINT(desc.Width),height=selected?processingHeight:desc.Height;
            if(session->processingWidth()!=width||session->processingHeight()!=height){
                slot->caches.clear(); // both session and slot fences completed above
                session->setProcessingExtent(width,height);
                LOG_INFO("Native NR adaptive resolution: processing={}x{}, backbuffer={}x{}, graphPassesPerFrame={}, source={}",width,height,desc.Width,desc.Height,session->passesPerFrame(),selected?"upscaler display extent":"backbuffer fallback");
            }
        }
        if(capturePair&&!slot->pair&&slot->capturedPairs<captureFrames&&slot->frames>=captureStartFrame&&(slot->frames-captureStartFrame)%captureInterval==0)slot->pair=std::make_shared<NativeNrPairCapture>(device.Get(),target.Get());
        bool recordPair=slot->pair&&!slot->pairSerial;
        ID3D12GraphicsCommandList* executeList=nullptr;
        if(reuseCommands&&(nativeTiles||globalB1)&&!session->temporalEnabled()&&!recordPair&&session->recordedFrames()>0){
            // Proxy resources may expose changing COM pointers. Bound retained
            // recordings even then; completion was checked above before clearing.
            if(slot->caches.size()>=16&&!slot->caches.count(target.Get()))slot->caches.clear();
            auto& cached=slot->caches[target.Get()];
            if(cached&&!cached->matches(session,target.Get()))cached.reset();
            if(cached){if(cached->markReadyForExecute())++slot->cacheReplays;else cached.reset();}
            if(globalB1&&slot->lastProfile&&(slot->frames<7||slot->frames%60==0)){
                auto totals=slot->lastProfile->readAfterFence(queue);
                LOG_INFO("Native NR Global B1 GPU: frameMs={}, output={}x{}, model={}x{}, graphPassesPerFrame=1",totals.at("frame").ms,desc.Width,desc.Height,session->lowWidth(),session->lowHeight());
            }
            if(!cached){cached=std::make_unique<NativeNrCommandCache>(session,target.Get(),globalB1?std::make_shared<NativeGpuProfile>(device.Get(),8):nullptr);if(!cached->markReadyForExecute())throw std::runtime_error("New NR cache was not ready");++slot->cacheBuilds;}
            slot->lastProfile=cached->gpuProfile();
            executeList=cached->commandList();submissions->track(executeList,session);
        }else{
            // Diagnostic pair is one-shot and must never be captured in a replay.
            slot->caches.clear();
            check(slot->allocator->Reset(),"NR present allocator reset");check(slot->list->Reset(slot->allocator.Get(),nullptr),"NR present list reset");
            submissions->track(slot->list.Get(),session);
            if(recordPair)slot->pair->recordBefore(slot->list.Get(),target.Get());
            if(!session->record(slot->list.Get(),target.Get(),D3D12_RESOURCE_STATE_PRESENT)){faulted=true;throw std::runtime_error("NR present recording failed");}
            if(recordPair){auto diagnostic=session->lastInferenceWasTemporal()?session->motionDiagnosticTensors():std::make_pair(static_cast<Tensor*>(nullptr),static_cast<Tensor*>(nullptr));slot->pair->recordAfter(slot->list.Get(),target.Get(),session->lastInferenceWasTemporal()?session->stabilityDiagnosticTensor():nullptr,diagnostic.first,diagnostic.second);}
            check(slot->list->Close(),"NR present close");executeList=slot->list.Get();
        }
        ID3D12CommandList* lists[]={executeList};
        double commandCpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-commandStart).count();
        queue->ExecuteCommandLists(1,lists);check(queue->Signal(slot->fence.Get(),++slot->serial),"NR present reuse signal");
        NativeNrControl::progress.store(2);NativeNrControl::temporalApplied.store(session->lastInferenceWasTemporal());
        if(slot->frames<6||slot->frames%120==0){auto t=session->temporalTrace();LOG_INFO("Native NR temporal consumer: present={}, captures={}, captureSubmits={}, producerFrame={}, consumerProducer={}, hadInput={}, reset={}, applied={}, temporalFrames={}, spatialFrames={}",slot->frames+1,t.captures,t.captureSubmissions,t.producerFrame,t.consumerProducerFrame,t.consumerHadInput,t.consumerReset,session->lastInferenceWasTemporal(),t.temporalConsumers,t.spatialConsumers);}
        if(recordPair){slot->pairSerial=slot->serial;slot->pairGraphPasses=session->passesPerFrame();auto t=session->temporalTrace();slot->pairPresent=slot->frames+1;slot->pairProducer=t.consumerProducerFrame;slot->pairTemporal=session->lastInferenceWasTemporal();}
        if(++slot->frames<=6||slot->frames%60==0)LOG_INFO("Native NR SDR Present recorded: handle={}, frames={}, output={}x{}, processing={}x{}, GlobalB1={}, nativeTiles={}, graphPassesPerFrame={}, readyPresents={}, busySkipped={}, preparationSkipped={}, fenceWaits={}, commandCacheBuilds={}, commandCacheReplays={}, commandCpuMs={}",slot->handle,slot->frames,desc.Width,desc.Height,session->processingWidth(),session->processingHeight(),globalB1,nativeTiles,session->passesPerFrame(),slot->readyPresents,slot->busySkips,slot->preparationSkips,slot->waits,slot->cacheBuilds,slot->cacheReplays,commandCpuMs);
    }
    void runWorker(){
        std::unique_lock<std::recursive_mutex> lock(mutex);
        for(;;){
            wake.wait(lock,[&](){return workerExit||(scheduled&&(!requests.empty()||!motionRequests.empty()));});
            if(workerExit)return;
            if(!motionRequests.empty()){
                auto request=*motionRequests.begin();motionRequests.erase(motionRequests.begin());auto session=sessions.lookup(request.first);
                motionPreparing.insert(request.first);
                NativeNrControl::temporalProgress.store(1);
                // Isolated one-time arena setup. Release the controller lock so
                // private queue hooks cannot deadlock with Present. Identical
                // failed geometry is cached and never retries every frame.
                lock.unlock();
                bool setupFailed=false;
                HANDLE prepEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                try{if(!session||!prepEvent||!session->waitReady(prepEvent,120000))throw std::runtime_error("Temporal setup fence/event unavailable");{session->prepareTemporalEngine(request.second);LOG_INFO("Native NR temporal arena prepared: handle={}, motion={}x{}, depth={}x{}, reactiveFormat={}, reactiveUsed={}",request.first,request.second.motion.Width,request.second.motion.Height,request.second.depth.Width,request.second.depth.Height,UINT(request.second.reactive.Format),NativeMotionBridge::usableReactive(request.second.reactive));}}
                catch(const std::exception& e){setupFailed=true;NativeNrControl::temporalProgress.store(3);LOG_WARN("Native NR temporal preparation skipped: {}",e.what());}
                if(prepEvent)CloseHandle(prepEvent);lock.lock();if(setupFailed)failedMotionGeometry[request.first]=request.second;else failedMotionGeometry.erase(request.first);motionPreparing.erase(request.first);continue;
            }
            auto request=*requests.begin();requests.erase(requests.begin());
            auto generation=generations[request.first];
            int requestedMode=requestModes.count(request.first)?requestModes[request.first]:0;
            if(presentSdr&&request.first>=0xf0000000u){
                // Do not pin a real swapchain buffer throughout weight loading.
                // Preparation uses an owned shape proxy; record() binds the actual
                // current backbuffer only after the session becomes ready.
                auto desc=request.second->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;props.CreationNodeMask=props.VisibleNodeMask=1;
                ComPtr<ID3D12Resource> proxy;
                auto hr=device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&proxy));
                if(FAILED(hr)){preparationResult=hr;++rejectedRequests;failedRequests.insert(request.first);continue;}
                request.second=proxy;
            }
            auto previous=sessions.lookup(request.first);
            if(previous){
                preparing=true;preparingHandle=request.first;preparingTarget=request.second;
                lock.unlock();HANDLE drain=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                bool drained=false;try{drained=drain&&previous->waitQuiescent(drain,120000);}catch(...){}
                if(drain)CloseHandle(drain);lock.lock();
                if(!drained){preparing=false;preparingTarget.Reset();failedRequests.insert(request.first);NativeNrModes::failure.store(2);LOG_ERROR("Native NR mode switch deferred: previous GPU session not ready");continue;}
                for(auto& item:presentSlots)if(item.second->handle==request.first){item.second->caches.clear();item.second->lastProfile.reset();}
            }
            if(previous){sessions.retire(request.first);sessions.collect();}
            preparing=true;preparingHandle=request.first;preparingTarget=request.second;++preparationAttempts;
            lock.unlock();
            // Outstanding old recordings remain owned by the pending registry/tracker.
            // A ready old graph is destroyed on this worker, never in Evaluate.
            previous.reset();
            std::shared_ptr<NativeNrSession> session;
            int requestFailure=0;
            try{std::string folder=assets.folder();
#ifdef NR_GAME_MODES
                NativeNrAssets selected(modeRoot/NativeNrModes::modes[requestedMode].folder);folder=selected.folder();
#endif
                bool present=presentSdr&&request.first>=0xf0000000u;session=std::make_shared<NativeNrSession>(request.second.Get(),folder,gameProxy,present,present&&nativeTiles,present&&globalB1);session->setResidualStability(residualStability,boundedResidual,0,adaptiveResidual,motionAwareResidual);if(present&&globalB1)session->setEdgeGuidedResidual(edgeGuidedResidual);session->enableStabilityDiagnostic(capturePair);LOG_INFO("Native NR graph initialized: handle={}, output={}x{}, model={}x{}, GlobalB1={}, NativeTiles={}",request.first,request.second->GetDesc().Width,request.second->GetDesc().Height,session->lowWidth(),session->lowHeight(),globalB1,nativeTiles);}catch(const std::exception& e){requestFailure=std::string(e.what()).find("GPU budget")!=std::string::npos?1:3;LOG_ERROR("Native NR preparation failed: {}",e.what());}
            lock.lock();
            bool current=!stopping&&!faulted&&generations[request.first]==generation&&!sessions.lookup(request.first);
#ifdef NR_GAME_MODES
            current=current&&requestedMode==NativeNrModes::requested.load();
#endif
            if(current){
                NativeNrModes::failure.store(requestFailure);
                auto motion=observedMotionGeometry.find(request.first);
                if(temporalWanted&&session&&motion!=observedMotionGeometry.end()){
                    auto geometry=motion->second;bool failed=false;lock.unlock();
                    LOG_INFO("Native NR temporal geometry: motionFormat={}, depthFormat={}, reactiveFormat={}, reactiveUsed={}",UINT(geometry.motion.Format),UINT(geometry.depth.Format),UINT(geometry.reactive.Format),NativeMotionBridge::usableReactive(geometry.reactive));
                    try{session->prepareTemporalEngine(geometry);}catch(const std::exception& e){failed=true;LOG_WARN("Native NR initial temporal setup skipped: {}",e.what());}
                    lock.lock();if(failed)failedMotionGeometry[request.first]=geometry;else failedMotionGeometry.erase(request.first);
                }
                try{if(stopping||faulted||generations[request.first]!=generation)throw std::runtime_error("Stale NR worker preparation");if(!session)throw std::runtime_error("NR worker preparation failed");
                    sessions.adopt(request.first,session,false);publish(request.first,session);
                    preparationResult=S_OK;++preparedRequests;
#ifdef NR_GAME_MODES
                    NativeNrModes::active.store(requestedMode);
#endif
                    LOG_INFO("Native NR prepared: handle={}, published=true, PresentSDR={}",request.first,presentSdr);
                }catch(...){preparationResult=E_FAIL;++rejectedRequests;failedRequests.insert(request.first);}
            }
            // Dispose unpublished GPU resources outside the submission lock too.
            lock.unlock();session.reset();lock.lock();
            preparing=false;preparingTarget.Reset();scheduled=scheduled&&!requests.empty();
        }
    }
    struct RecordOwner {std::weak_ptr<Controller> controller;std::shared_ptr<NativeNrSession> session;};
    void prepare(UINT handle,ID3D12Resource* output){
        std::lock_guard<std::recursive_mutex> lock(mutex);collect();
        if(stopping||faulted||!output)throw std::runtime_error("NR controller unavailable");
        requests.erase(handle);
        ++generations[handle];
        ComPtr<ID3D12Device> targetDevice;check(output->GetDevice(IID_PPV_ARGS(&targetDevice)),"NR target device");
        auto desc=output->GetDesc();if(!sameNativeDevice(targetDevice.Get(),device.Get())||desc.Width!=assets.width()||desc.Height!=assets.height())throw std::runtime_error("NR package/target mismatch");
        auto session=sessions.prepare(handle,output,assets.folder(),false);
        publish(handle,session);
    }
    void publish(UINT handle,const std::shared_ptr<NativeNrSession>& session){
        try{
        auto owner=std::make_shared<RecordOwner>();owner->controller=shared_from_this();owner->session=session;
        auto binding=std::make_shared<NativeNrHook::Binding>(*session->binding(handle));binding->owner=owner;binding->user=owner.get();
        binding->ready=[](void* user){auto& state=*static_cast<RecordOwner*>(user);auto controller=state.controller.lock();if(!controller)return false;
            std::lock_guard<std::recursive_mutex> lock(controller->mutex);controller->collect();return !controller->stopping&&!controller->faulted&&state.session->ready();};
        binding->record=[](void* user,ID3D12GraphicsCommandList* list,ID3D12Resource* target,D3D12_RESOURCE_STATES state){
            auto& owner=*static_cast<RecordOwner*>(user);auto controller=owner.controller.lock();if(!controller)return false;
            std::lock_guard<std::recursive_mutex> lock(controller->mutex);controller->collect();
            if(controller->stopping||controller->faulted||!owner.session->ready()||!list||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return false;
            // Allocate ownership bookkeeping before recording any GPU command.
            // Failed/partial recording remains tracked until submit or confirmed Reset.
            try{controller->submissions->track(list,owner.session);}catch(...){return false;}
            bool success=owner.session->record(list,target,state);if(!success)controller->faulted=true;return success;
        };
        NativeNrHook::RegisterOwned(binding);
        }catch(...){sessions.retire(handle);sessions.collect();throw;}
    }
    std::shared_ptr<const NativeNrLifecycle::Provider> provider(){auto value=std::make_shared<NativeNrLifecycle::Provider>();value->owner=shared_from_this();value->user=this;
        value->frameInputs=[](void* user,ID3D12GraphicsCommandList* list,const NativeNrTemporal::FrameInput& f){auto& c=*static_cast<Controller*>(user);std::lock_guard<std::recursive_mutex> lock(c.mutex);
            try{return c.frameInputs(list,f);}catch(const std::exception& e){if(f.featureFrame<6||f.featureFrame%120==0)LOG_WARN("Native NR temporal capture exception: frame={}, reason={}",f.featureFrame,e.what());return false;}
            catch(...){if(f.featureFrame<6||f.featureFrame%120==0)LOG_WARN("Native NR temporal capture exception: frame={}, unknown failure",f.featureFrame);return false;}};
        value->execute=[](void* user,ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists,NativeNrLifecycle::Forward original,void* args){
            auto& c=*static_cast<Controller*>(user);std::lock_guard<std::recursive_mutex> lock(c.mutex);c.collect();
            // Worker initialization uses private queues; the heavy construction
            // never holds this lock. Explicit Prepare remains a blocking API.
            if(!c.submissions->hasRecorded(count,lists)){original(args);c.pump();return true;}
            try{c.submissions->execute(queue,count,lists,[&](){original(args);});c.pump();return true;}catch(...){c.faulted=true;throw;}
        };
        value->recorded=[](void* user,unsigned handle,ID3D12GraphicsCommandList* list){auto& c=*static_cast<Controller*>(user);std::lock_guard<std::recursive_mutex> lock(c.mutex);
            auto session=c.sessions.lookup(handle);return session&&c.submissions->contains(list,session.get());};
        value->reset=[](void* user,ID3D12GraphicsCommandList* list){auto& c=*static_cast<Controller*>(user);std::lock_guard<std::recursive_mutex> lock(c.mutex);c.submissions->discard(list);c.collect();};
        value->retire=[](void* user,unsigned handle){auto& c=*static_cast<Controller*>(user);std::lock_guard<std::recursive_mutex> lock(c.mutex);++c.generations[handle];c.requests.erase(handle);c.failedRequests.erase(handle);c.sessions.retire(handle);c.collect();};
        value->shutdown=[](void* user){auto& c=*static_cast<Controller*>(user);std::lock_guard<std::recursive_mutex> lock(c.mutex);c.stopping=true;c.workerExit=true;c.wake.notify_all();c.requests.clear();c.sessions.requestShutdown();c.collect();};
        value->observe=[](void* user,unsigned handle,ID3D12Resource* output){auto& c=*static_cast<Controller*>(user);std::lock_guard<std::recursive_mutex> lock(c.mutex);c.observe(handle,output);};return value;
    }
};
std::shared_ptr<Controller> controller;
std::recursive_mutex apiMutex;
HMODULE heldModule=nullptr;
HRESULT configuredFailure=S_OK;
std::string configuredError;
template<class F> HRESULT boundary(F action){std::lock_guard<std::recursive_mutex> lock(apiMutex);
    try{lastError.clear();return action();}catch(const std::exception& e){lastError=e.what();return E_FAIL;}catch(...){lastError="Unknown NR backend error";return E_FAIL;}}
std::shared_ptr<Controller> current(){if(!controller)throw std::runtime_error("NR backend not initialized");return controller;}
void save(const std::string& path,const std::vector<float>& values){std::ofstream stream(nativePath(path),std::ios::binary);
    stream.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(float)));if(!stream)throw std::runtime_error("NR diagnostic write failed");}
}

HRESULT WINAPI OptiScalerNR_Initialize(ID3D12CommandQueue* queue){return boundary([&]()->HRESULT{
    if(controller)return E_UNEXPECTED;
    HMODULE module=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&OptiScalerNR_Initialize),&module))throw std::runtime_error("NR module retention failed");
    try{auto value=std::make_shared<Controller>(module,queue);NativeNrLifecycle::Register(value->provider());
        if(!NativeNrQueueHook::Install(queue)){NativeNrLifecycle::Register(nullptr);throw std::runtime_error("NR queue observer install failed");}
        value->startWorker();controller=std::move(value);heldModule=module;return S_OK;
    }catch(...){NativeNrQueueHook::Remove();NativeNrLifecycle::Register(nullptr);FreeLibrary(module);throw;}
});}
HRESULT WINAPI OptiScalerNR_Prepare(UINT handle,ID3D12Resource* output){return boundary([&](){current()->prepare(handle,output);return S_OK;});}
HRESULT WINAPI OptiScalerNR_Evaluate(UINT handle,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* params,BOOL hdr,BOOL knownState,UINT state,UINT* result){return boundary([&]()->HRESULT{
    if(!result)return E_POINTER;auto c=current();if(c->presentSdr){*result=0;return S_OK;}*result=UINT(NativeNrHook::Evaluate(handle,list,params,hdr!=FALSE,knownState!=FALSE,D3D12_RESOURCE_STATES(state)));return S_OK;});}
HRESULT WINAPI OptiScalerNR_Reset(ID3D12GraphicsCommandList* list,HRESULT result){return boundary([&](){current();return NativeNrLifecycle::Reset(list,result)?S_OK:E_FAIL;});}
HRESULT WINAPI OptiScalerNR_Retire(UINT handle){return boundary([&](){current();return NativeNrLifecycle::Retire(handle)?S_OK:E_FAIL;});}
HRESULT WINAPI OptiScalerNR_Collect(){return boundary([&](){auto c=current();std::lock_guard<std::recursive_mutex> lock(c->mutex);c->collect();return S_OK;});}
HRESULT WINAPI OptiScalerNR_Stats(UINT handle,NativeNrApiStats* stats){return boundary([&]()->HRESULT{
    if(!stats||stats->size!=sizeof(*stats))return E_INVALIDARG;auto c=current();std::lock_guard<std::recursive_mutex> lock(c->mutex);c->collect();
    *stats={sizeof(*stats),1,UINT(c->sessions.activeCount()),UINT(c->sessions.pendingCount()),UINT(c->submissions->recordedCount()),UINT(c->submissions->inFlightCount()),0,UINT(c->faulted),0};
    if(auto session=c->sessions.lookup(handle)){stats->frames=session->recordedFrames();stats->selectedSessionBytes=session->allocatedBytes();}return S_OK;});}
HRESULT WINAPI OptiScalerNR_Capture(UINT handle,const wchar_t* folder){return boundary([&]()->HRESULT{
    if(!folder)return E_POINTER;auto c=current();std::lock_guard<std::recursive_mutex> lock(c->mutex);c->collect();auto session=c->sessions.lookup(handle);if(!session)return E_INVALIDARG;
    auto utf8=std::filesystem::path(folder).u8string();std::string path(reinterpret_cast<const char*>(utf8.data()),utf8.size());
    save(path+"/native_texture.bin",session->captureTexture(path+"/native_texture_packed.bin"));save(path+"/native_features.bin",session->captureFeatures());save(path+"/native_rgb.bin",session->captureRgb());
    auto& graph=session->captureGraph();save(path+"/native.bin",graph.publicNodes().back()->runtime()->download(graph.resultTensor()));
    for(auto& node:graph.publicNodes()){save(path+"/native_"+node->name+".bin",node->runtime()->download(node->output()));
        if(node->window&&node->window->skipTensor())save(path+"/native_skip_"+node->name+".bin",node->runtime()->download(node->window->skipTensor()));}return S_OK;});}
HRESULT WINAPI OptiScalerNR_Stop(){return boundary([&]()->HRESULT{
    if(!controller)return S_FALSE;auto c=controller;
    {std::lock_guard<std::recursive_mutex> lock(c->mutex);c->stopping=true;c->workerExit=true;c->wake.notify_all();c->requests.clear();c->sessions.requestShutdown();c->collect();
    if(c->preparing||c->submissions->recordedCount()||c->submissions->inFlightCount()||c->sessions.pendingCount())return DXGI_ERROR_WAS_STILL_DRAWING;}
    // Join outside the mutex: initialization queues may re-enter the observer.
    if(c->worker.joinable())c->worker.join();
    c->finishCaptures();
    if(!NativeNrQueueHook::Remove())return E_FAIL;NativeNrLifecycle::Register(nullptr);controller.reset();HMODULE module=heldModule;heldModule=nullptr;FreeLibrary(module);return S_OK;});}
const char* WINAPI OptiScalerNR_LastError(){return lastError.c_str();}
HRESULT WINAPI OptiScalerNR_PreparationStats(NativeNrPreparationStats* stats){return boundary([&]()->HRESULT{
    if(!stats||stats->size!=sizeof(*stats))return E_INVALIDARG;auto c=current();std::lock_guard<std::recursive_mutex> lock(c->mutex);
    *stats={sizeof(*stats),1,UINT(c->requests.size()),UINT(c->preparing),c->preparationAttempts,c->preparedRequests,c->rejectedRequests,c->preparationResult};return S_OK;});}
HRESULT WINAPI OptiScalerNR_ConfigureQueue(ID3D12CommandQueue* queue){return boundary([&]()->HRESULT{
    if(!NativeNrIsEnabled())return S_FALSE;
    if(controller){
        if(!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
        ComPtr<ID3D12Device> device;check(queue->GetDevice(IID_PPV_ARGS(&device)),"Configured queue device");
        if(!sameNativeDevice(device.Get(),controller->device.Get())||controller->stopping||controller->faulted)return E_UNEXPECTED;
        if(!NativeNrQueueHook::Install(queue))return E_FAIL;return S_FALSE;
    }
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&OptiScalerNR_ConfigureQueue),&module))return E_FAIL;
    std::vector<wchar_t> path(32768);auto length=GetModuleFileNameW(module,path.data(),UINT(path.size()));
    if(!length||length>=path.size())return E_FAIL;
    auto config=NativeNrAssets::moduleDirectory(module)/L"native_nr.ini";
    if(FAILED(configuredFailure)){lastError=configuredError;return configuredFailure;}
    if(!queue||queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return E_INVALIDARG;
    ComPtr<ID3D12Device> device;check(queue->GetDevice(IID_PPV_ARGS(&device)),"Configured queue device");
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Configured adapter factory");
    ComPtr<IDXGIAdapter1> adapter;check(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"Configured adapter");
    DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Configured adapter description");
    if(!NativeNrSupportedAdapter(desc.VendorId,desc.DeviceId))return E_NOTIMPL;
    auto result=OptiScalerNR_Initialize(queue);
    if(FAILED(result)){configuredFailure=result;configuredError=lastError;}
    return result;
});}

void NativeNrPresentSdr(ID3D12CommandQueue* queue,IDXGISwapChain* swapchain,bool sdrOutput,UINT processingWidth,UINT processingHeight){
    NativeNrControl::progress.store(0);NativeNrControl::temporalApplied.store(false);
    std::lock_guard<std::recursive_mutex> apiLock(apiMutex);
    if(!NativeNrIsEnabled()){
        if(controller){std::lock_guard<std::recursive_mutex> lock(controller->mutex);controller->collect();}
        return;
    }
    if(!controller){
        NativeNrControl::progress.store(1);
        auto hr=OptiScalerNR_ConfigureQueue(queue);
        if(FAILED(hr)){NativeNrControl::progress.store(3);return;}
    }
    if(!controller||!controller->presentSdr)return;
    try{controller->present(queue,swapchain,sdrOutput,processingWidth,processingHeight);
        std::lock_guard<std::recursive_mutex> lock(controller->mutex);
        if(controller->faulted)NativeNrControl::progress.store(3);
    }catch(const std::exception& e){std::lock_guard<std::recursive_mutex> lock(controller->mutex);controller->faulted=true;NativeNrControl::progress.store(3);LOG_ERROR("Native NR SDR Present failed: {}",e.what());}
}
bool NativeNrIsEnabled(){
    if(NativeNrControl::requested.load()==-1){
        HMODULE module=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&NativeNrIsEnabled),&module)){
            auto config=NativeNrAssets::moduleDirectory(module)/L"native_nr.ini";
#ifdef NR_GAME_MODES
            NativeNrControl::Startup(false);
#else
            NativeNrControl::Startup(GetPrivateProfileIntW(L"NativeNR",L"Enabled",0,config.c_str())==1);
#endif
        }else NativeNrControl::Startup(false);
    }
    return NativeNrControl::Enabled();
}
void NativeNrSetEnabled(bool enabled){NativeNrControl::Set(enabled);NativeNrControl::temporalProgress.store(0);{std::lock_guard<std::recursive_mutex> guard(apiMutex);if(controller){std::lock_guard<std::recursive_mutex> lock(controller->mutex);controller->failedMotionGeometry.clear();for(auto& entry:controller->presentSlots)if(auto session=controller->sessions.lookup(entry.second->handle))session->invalidateTemporalHistory();}}LOG_INFO("Native NR menu request: enabled={} (not persisted)",enabled);}
bool NativeNrUsesPresentSdr(){std::lock_guard<std::recursive_mutex> lock(apiMutex);return controller&&controller->presentSdr;}
HRESULT NativeNrReleasePresent(IDXGISwapChain* swapchain){
    std::lock_guard<std::recursive_mutex> apiLock(apiMutex);if(!controller||!controller->presentSdr)return S_FALSE;
    auto c=controller;
    std::lock_guard<std::recursive_mutex> lock(c->mutex);c->collect();auto found=c->presentSlots.find(swapchain);if(found==c->presentSlots.end())return S_FALSE;
    auto handle=found->second->handle;++c->generations[handle];c->requests.erase(handle);c->failedRequests.erase(handle);
    // Cover DXGI/overlay work queued after the model's own reuse signal too.
    if(found->second->queue){auto hr=found->second->queue->Signal(found->second->fence.Get(),++found->second->serial);if(FAILED(hr))return hr;}
    auto done=found->second->fence->GetCompletedValue();if(done==~UINT64(0))return DXGI_ERROR_DEVICE_REMOVED;
    if(done<found->second->serial){
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return E_FAIL;
        auto hr=found->second->fence->SetEventOnCompletion(found->second->serial,event);
        auto wait=SUCCEEDED(hr)?WaitForSingleObject(event,120000):WAIT_FAILED;CloseHandle(event);
        if(wait!=WAIT_OBJECT_0)return DXGI_ERROR_WAS_STILL_DRAWING;
        c->collect();
    }
    c->sessions.retire(handle);c->collect();
    if(c->sessions.pendingCount())return DXGI_ERROR_WAS_STILL_DRAWING;
    c->presentSlots.erase(found);return S_OK;
}
