#pragma once
#include "dml_graph_pipeline.h"
#include "dml_texture.h"
#include "dml_frame.h"
#include "nr_temporal.h"
#include "nr_motion.h"
#include "../../integration/optiscaler/NativeNrModes.h"
#include <functional>
#include "../../integration/optiscaler/NativeNrHook.h"
#include <mutex>
#include <condition_variable>
#include <chrono>

class NativeNrSession : public std::enable_shared_from_this<NativeNrSession> {
public:
    enum class Phase { Ready, Recorded, InFlight, Poisoned, Retired };
    struct TemporalTrace {
        UINT64 captures=0,captureSubmissions=0,temporalConsumers=0,spatialConsumers=0;
        UINT64 producerFrame=0,consumerProducerFrame=0;
        bool pendingInput=false,consumerHadInput=false,consumerReset=true;
    };
private:
    std::shared_ptr<NativeContext> context;
    ComPtr<ID3D12Resource> temporary;
    ComPtr<ID3D12Resource> snapshot,externalTarget;
    D3D12_RESOURCE_STATES snapshotState=D3D12_RESOURCE_STATE_COMMON;
    std::unique_ptr<TextureHarness> textures;
    std::unique_ptr<FrameHarness> features,composition;
    std::unique_ptr<GraphPipeline> graph;
    std::shared_ptr<NativeTemporalInputs> temporalInputs;
    std::unique_ptr<NativeTemporalPass> temporal;
    bool temporalFrameValid=false,temporalReset=true;
    std::unique_ptr<NativeMotionBridge> motionBridge;
    bool motionCaptureOnly=false,inferenceTemporalUsed=false;
    TemporalTrace temporalTraceState;
    float residualStability=0;bool boundedResidual=false,stabilityDiagnostic=false,adaptiveResidual=false,motionAwareResidual=false;UINT dampHistory=0;
    UINT64 previousMotionFrame=0;unsigned previousMotionHandle=0;
    float previousJitterX=0,previousJitterY=0;
    bool motionSequenceValid=false;
    ComPtr<ID3D12Fence> completion;
    UINT64 completionValue=0;
    D3D12_RESOURCE_STATES temporaryState=D3D12_RESOURCE_STATE_COMMON;
    Phase phase=Phase::Ready;
    bool retiring=false;
    bool unsubmitted=false;
    bool damaged=false;
    UINT records=0;
    bool gameProxy=false;
    bool nativeTiles=false;
    bool globalB1=false;
    UINT modelWidth=640,modelHeight=360;
    UINT modelPasses=0;
    mutable std::mutex mutex;
    std::condition_variable submissionChanged;
    std::vector<NativeTrackedState> trackedStates(){
        std::vector<NativeTrackedState> states;textures->appendTextureStates(states);features->appendTrackedStates(states);composition->appendTrackedStates(states);
        if(temporal){temporal->appendTrackedStates(states);temporalInputs->appendTrackedStates(states);}
        if(motionBridge)motionBridge->appendMotionStates(states);
        for(auto& node:graph->publicNodes())node->runtime()->appendTrackedStates(states);
        for(auto& tensor:context->tensorScratch)states.push_back({tensor.second->resource.Get(),&tensor.second->state});
        states.push_back({temporary.Get(),&temporaryState});if(snapshot)states.push_back({snapshot.Get(),&snapshotState});return states;
    }
    void pollLocked(){
        if(phase==Phase::InFlight){UINT64 done=completion->GetCompletedValue();
            if(done==~UINT64(0)){phase=Phase::Poisoned;return;}
            if(done>=completionValue)phase=retiring||damaged?Phase::Retired:Phase::Ready;
        }
    }
    void requireCapture(){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(phase!=Phase::Ready&&phase!=Phase::Retired)throw std::runtime_error("Session capture requires completed caller fence");}
public:
    struct CacheState {ID3D12Resource* resource;D3D12_RESOURCE_STATES before;std::vector<D3D12_RESOURCE_STATES*> trackers;};
    std::vector<CacheState> cacheState(){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(phase!=Phase::Ready||retiring||temporal||!snapshot||(!nativeTiles&&!globalB1)||context->profiler||records==0)throw std::runtime_error("Cache requires a warmed ready spatial NR session; temporal replay needs parity variants");
        std::map<ID3D12Resource*,CacheState> grouped;
        for(auto cell:trackedStates()){auto found=grouped.find(cell.resource);if(found==grouped.end())found=grouped.emplace(cell.resource,CacheState{cell.resource,*cell.state,{}}).first;
            if(found->second.before!=*cell.state)throw std::runtime_error("Inconsistent aliased NR state trackers");found->second.trackers.push_back(cell.state);}
        std::vector<CacheState> result;for(auto& entry:grouped)result.push_back(std::move(entry.second));return result;
    }
    void closeCacheCycle(ID3D12GraphicsCommandList* list,const std::vector<CacheState>& states){std::lock_guard<std::mutex> lock(mutex);
        if(phase!=Phase::Recorded||!unsubmitted)throw std::runtime_error("Cache cycle requires an unsubmitted recording");
        for(const auto& cell:states){auto after=*cell.trackers.front();for(auto tracker:cell.trackers)if(*tracker!=after)throw std::runtime_error("Cache alias state diverged");
            if(after!=cell.before)transition(list,cell.resource,after,cell.before);for(auto tracker:cell.trackers)*tracker=cell.before;}
    }
    bool markCacheReplay(ID3D12Resource* target,UINT width,UINT height,const std::vector<CacheState>& states){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(phase!=Phase::Ready||retiring||temporal||!snapshot||(!nativeTiles&&!globalB1)||context->profiler||textures->workWidth()!=width||textures->workHeight()!=height||!textures->compatibleInput(target,true))return false;
        for(const auto& cell:states)for(auto tracker:cell.trackers)if(*tracker!=cell.before)return false;
        externalTarget=target;phase=Phase::Recorded;unsubmitted=true;++records;modelPasses+=nativeTiles?textures->tileCount():1;return true;
    }
    NativeNrSession(ID3D12Resource* target,const std::string& folder,bool scaledGameProxy=false,bool presentSnapshot=false,bool tiledDetail=false,bool global=false):gameProxy(scaledGameProxy),nativeTiles(tiledDetail),globalB1(global){
        if(globalB1&&(!gameProxy||!presentSnapshot||nativeTiles))throw std::runtime_error("Global B1 requires PresentSDR snapshot without tiles");
        if(nativeTiles&&(!gameProxy||!presentSnapshot))throw std::runtime_error("Native detail requires a GPU snapshot and the 320 model");
        if(!target)throw std::runtime_error("Session target missing");
        ComPtr<ID3D12Device> device;check(target->GetDevice(IID_PPV_ARGS(&device)),"Session device");context=NativeContext::onDevice(device.Get());
#ifdef NR_GAME_GLOBAL_B1
        if(globalB1){
            context->acceptedGameGlobalPolicy=true;
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_10};
            check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm)),"Game SM6.10 query");
            if(sm.HighestShaderModel<D3D_SHADER_MODEL_6_10)throw std::runtime_error("Game device lacks SM6.10; use Global B1 launcher");
            linAlgCheckWaveFP16(device.Get());nrLinAlgShaderDirectory=nativePath(folder)/"shaders";
            // Frozen accepted policy. User environment cannot select rejected experiments.
            context->tileBatch=1;context->linAlg=true;context->linAlgAttention=true;
            context->linAlgWindowFused=true;context->linAlgWindowNormalizeFused=true;
            context->branchCompact=true;context->globalNormalizationPolicy="fused-native-round";
            context->c32CompactPolicy="compact-f16";context->attentionPartitionPolicy="wg256";
            context->denseTileN=32;context->normWavePolicy="wave8";context->softmaxWavePolicy="wave8";
            context->c32ProjectionPolicy="baseline";context->windowSegmentPolicy="fused";
#if defined(NR_CLEAN_PROFILE) && defined(NR_DENSE_WAVE)
            if(GetEnvironmentVariableA("NR_STUDY_DENSE",nullptr,0))context->denseWavePolicy="register32";
#endif
#if defined(NR_CLEAN_PROFILE) && defined(NR_C32_FULL_FFN)
            if(GetEnvironmentVariableA("NR_STUDY_FFN",nullptr,0))context->c32FullFfnPolicy="fused";
#endif
            context->allowHalfPrecision=false;context->tensorHalfPrecision=false;context->prepackHalfWeights=false;
            context->directComputeGemm=false;context->convHalf=false;
        }
#else
        if(globalB1)throw std::runtime_error("DLL does not contain Global B1");
#endif
        auto desc=target->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width>3840||desc.Height>2160||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1)throw std::runtime_error("Session target geometry unsupported");
        D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;props.CreationNodeMask=props.VisibleNodeMask=1;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&temporary)),"Session temporary texture");
        if(presentSnapshot){externalTarget=target;check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&snapshot)),"Present snapshot texture");}
        UINT modelH=UINT(desc.Height),modelW=UINT(desc.Width);
        if(gameProxy){std::ifstream config(nativePath(folder+"/frame_input/shape.txt"));if(!(config>>modelH>>modelW))throw std::runtime_error("Game proxy model shape missing");}
        bool acceptedGeometry=modelW==640&&modelH==360;
#ifdef NR_GAME_MODES
        acceptedGeometry=NativeNrModes::Geometry(modelW,modelH);
#endif
#ifdef NR_GAME_COMPACT512
        acceptedGeometry=modelW==512&&modelH==288;
        if(!globalB1)throw std::runtime_error("Compact512 requires Global B1");
#endif
#ifdef NR_GAME_COMPACT320
        acceptedGeometry=modelW==320&&modelH==180;
        if(!globalB1)throw std::runtime_error("Compact320 requires Global B1");
#endif
#ifdef NR_COMPACT_GEOMETRY
        acceptedGeometry=acceptedGeometry||NativeNrModes::Geometry(modelW,modelH)||(modelW==448&&modelH==252)||(modelW==576&&modelH==324);
#endif
        if(globalB1&&!acceptedGeometry)throw std::runtime_error("Global B1 package geometry unsupported");
        modelWidth=modelW;modelHeight=modelH;
        textures=std::make_unique<TextureHarness>(UINT(desc.Width),desc.Height,context,desc.Format,snapshot?snapshot.Get():target,temporary.Get(),modelW,modelH,nativeTiles,globalB1);
        features=std::make_unique<FrameHarness>(folder+"/frame_input",context,nullptr,nullptr,textures->colourTensor());
        features->constrainToGpuBudget();context->reuseScratch=true;
        graph=std::make_unique<GraphPipeline>(folder,context,features->resultTensor());
        composition=std::make_unique<FrameHarness>(folder+"/frame_output",context,graph->resultTensor(),textures->colourTensor());
#ifdef NR_GAME_GLOBAL_B1
        if(globalB1){
            if(context->linAlgJobs!=212||context->linAlgFusedWindows!=62||context->windowFeedReused!=52||context->windowC32Fused!=10||context->branchCompactWindows!=36||context->c32CompactWindows!=10)
                throw std::runtime_error("Global B1 prepared graph does not match accepted policy counts");
            const UINT expectedTokens=NativeNrModes::Tokens(modelWidth,modelHeight)?NativeNrModes::Tokens(modelWidth,modelHeight):modelWidth==448?(features->paddedHeight()==256?32:64):96;
            for(auto& node:graph->publicNodes())if(node->bottleneck&&node->bottleneck->tokenCount()!=expectedTokens)throw std::runtime_error("Global B1 token count mismatch");
        }
#endif
        if(composition->resultTensor()->elements!=textures->colourTensor()->elements)throw std::runtime_error("Session output geometry mismatch");
    }
    ~NativeNrSession(){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(phase!=Phase::Ready&&phase!=Phase::Retired)std::terminate();}
    bool ready(){std::lock_guard<std::mutex> lock(mutex);pollLocked();return phase==Phase::Ready&&!retiring;}
    // Worker-only setup. Inputs must be produced in GPU memory and paired with
    // this Present frame before attestTemporalFrame; no implicit zero-motion path.
    std::shared_ptr<NativeTemporalInputs> prepareTemporal(){
        std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(!globalB1||phase!=Phase::Ready||retiring)throw std::runtime_error("Temporal preparation requires a ready Global B1 session");
        if(!temporal){
            auto inputs=std::make_shared<NativeTemporalInputs>(context,modelWidth,modelHeight);
            auto pass=std::make_unique<NativeTemporalPass>(modelWidth,modelHeight,features->paddedWidth(),features->paddedHeight(),context,
                textures->colourTensor(),features->noiseTensor(),graph->resultTensor(),inputs->motionTensor(),inputs->confidenceTensor());
            pass->stability(residualStability,boundedResidual,dampHistory,adaptiveResidual,motionAwareResidual);pass->diagnostic(stabilityDiagnostic);temporalInputs=std::move(inputs);temporal=std::move(pass);
        }
        return temporalInputs;
    }
    UINT lowWidth()const{return modelWidth;}UINT lowHeight()const{return modelHeight;}
    void setResidualStability(float value,bool bounded=false,UINT damp=0,bool adaptive=false,bool motionAware=false){std::lock_guard<std::mutex> lock(mutex);if(!std::isfinite(value)||value<0||value>.4f||damp>3)throw std::runtime_error("Invalid session residual stability");residualStability=value;boundedResidual=bounded;dampHistory=damp;adaptiveResidual=adaptive;motionAwareResidual=motionAware;if(temporal)temporal->stability(value,bounded,damp,adaptive,motionAware);}
    void enableStabilityDiagnostic(bool enabled){std::lock_guard<std::mutex> lock(mutex);stabilityDiagnostic=enabled;if(temporal)temporal->diagnostic(enabled);}
    Tensor* stabilityDiagnosticTensor(){std::lock_guard<std::mutex> lock(mutex);return temporal&&stabilityDiagnostic?temporal->diagnosticTensor():nullptr;}
    std::pair<Tensor*,Tensor*> motionDiagnosticTensors(){std::lock_guard<std::mutex> lock(mutex);return temporalInputs&&stabilityDiagnostic?std::make_pair(temporalInputs->motionTensor(),temporalInputs->confidenceTensor()):std::make_pair(static_cast<Tensor*>(nullptr),static_cast<Tensor*>(nullptr));}
    bool temporalEnabled() const{std::lock_guard<std::mutex> lock(mutex);return bool(temporal);}
    void setEdgeGuidedResidual(bool enabled){std::lock_guard<std::mutex> lock(mutex);if(!globalB1||records||phase!=Phase::Ready)throw std::runtime_error("Edge guidance must be configured before Global B1 recording");textures->edgeGuidance(enabled);}
    TemporalTrace temporalTrace() const{std::lock_guard<std::mutex> lock(mutex);auto value=temporalTraceState;value.pendingInput=temporalFrameValid;return value;}
    bool lastInferenceWasTemporal() const{std::lock_guard<std::mutex> lock(mutex);return inferenceTemporalUsed;}
    bool motionPrepared() const{std::lock_guard<std::mutex> lock(mutex);return bool(motionBridge);}
    void prepareTemporalEngine(const NativeMotionGeometry& geometry){
        std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(!globalB1||phase!=Phase::Ready||retiring)throw std::runtime_error("Motion preparation requires ready Global B1 session");
        if(motionBridge&&motionBridge->sameGeometry(geometry))return;
        // Publish only after ALL required resources and shaders have succeeded.
        auto inputs=temporalInputs?temporalInputs:std::make_shared<NativeTemporalInputs>(context,modelWidth,modelHeight);
        auto bridge=std::make_unique<NativeMotionBridge>(context,geometry,inputs);
        std::unique_ptr<NativeTemporalPass> pass;
        if(!temporal)pass=std::make_unique<NativeTemporalPass>(modelWidth,modelHeight,features->paddedWidth(),features->paddedHeight(),context,
            textures->colourTensor(),features->noiseTensor(),graph->resultTensor(),inputs->motionTensor(),inputs->confidenceTensor());
        if(pass){pass->stability(residualStability,boundedResidual,dampHistory,adaptiveResidual,motionAwareResidual);pass->diagnostic(stabilityDiagnostic);}temporalInputs=std::move(inputs);if(pass)temporal=std::move(pass);motionBridge=std::move(bridge);
        temporalReset=true;temporalFrameValid=false;motionSequenceValid=false;
    }

    void invalidateTemporalHistory(){std::lock_guard<std::mutex> lock(mutex);temporalReset=true;temporalFrameValid=false;motionSequenceValid=false;}
    bool motionCompatible(const NativeNrTemporal::FrameInput& f){std::lock_guard<std::mutex> lock(mutex);return motionBridge&&motionBridge->compatible(f);}
    bool waitReady(HANDLE event,DWORD timeout){ComPtr<ID3D12Fence> fence;UINT64 value;
        {std::lock_guard<std::mutex> lock(mutex);pollLocked();if(phase==Phase::Ready)return true;if(phase!=Phase::InFlight)return false;fence=completion;value=completionValue;}
        check(fence->SetEventOnCompletion(value,event),"Temporal producer completion");
        if(WaitForSingleObject(event,timeout)!=WAIT_OBJECT_0)return false;return ready();
    }
    // A mode switch may reach a recorded engine list before Execute submits it.
    // Wait for its submit/discard notification, then for the actual GPU fence.
    // This never attests cancellation or frees an unsubmitted recording.
    bool waitQuiescent(HANDLE event,DWORD timeout){
        auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        {std::unique_lock<std::mutex> lock(mutex);pollLocked();
            if(!submissionChanged.wait_until(lock,deadline,[&]{return phase!=Phase::Recorded&&!(phase==Phase::Poisoned&&unsubmitted);}))return false;
            if(phase==Phase::Ready||phase==Phase::Retired)return true;
        }
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        return remaining>0&&waitReady(event,DWORD(remaining));
    }
    bool recordEngineMotion(ID3D12GraphicsCommandList* list,const NativeNrTemporal::FrameInput& f,const std::function<void()>& retain){
        std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(phase!=Phase::Ready||retiring||!motionBridge||!list||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||!motionBridge->compatible(f)){
            temporalReset=true;temporalFrameValid=false;return false;
        }
        // Allocate submission ownership BEFORE enqueuing any borrowed-resource copy.
        retain();phase=Phase::Recorded;unsubmitted=true;motionCaptureOnly=true;
        bool consecutive=motionSequenceValid&&previousMotionHandle==f.handle&&f.featureFrame==previousMotionFrame+1;
        temporalReset=temporalReset||f.reset||!consecutive;
        float jx=0,jy=0;
        if(f.jitteredMotion&&consecutive){
            // Match bundled FSR: mv -= (previous jitter - current jitter)/extent.
            jx=(f.jitterX-previousJitterX)/f.motionReferenceWidth;jy=(f.jitterY-previousJitterY)/f.motionReferenceHeight;
        }
        try{ExternalCommandRecording scope(context,list);motionBridge->record(f,jx,jy);
            ++temporalTraceState.captures;temporalTraceState.producerFrame=f.featureFrame;
            temporalFrameValid=true;motionSequenceValid=true;previousMotionFrame=f.featureFrame;previousMotionHandle=f.handle;previousJitterX=f.jitterX;previousJitterY=f.jitterY;return true;
        }catch(...){phase=Phase::Poisoned;damaged=true;temporalFrameValid=false;return false;}
    }
    std::shared_ptr<NativeGpuProfile> temporalProfileForTest(){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(!temporal||phase!=Phase::Ready)throw std::runtime_error("Temporal profiling requires ready temporal session");
        return std::make_shared<NativeGpuProfile>(context->device.Get(),16);
    }
#ifdef NR_CLEAN_PROFILE
    // Standalone profiling only; the deployed DLL is not rebuilt for this study.
    void setStudyProfiler(std::shared_ptr<NativeGpuProfile> profile){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(phase!=Phase::Ready)throw std::runtime_error("Study profiler requires completed session");context->profiler=std::move(profile);
    }
#endif
    void attestTemporalFrame(bool matchingFrame,bool reset){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(!temporal||phase!=Phase::Ready)throw std::runtime_error("Temporal attestation requires a ready prepared session");
        temporalFrameValid=matchingFrame;temporalReset=temporalReset||reset||!matchingFrame;
    }
    bool record(ID3D12GraphicsCommandList* list,ID3D12Resource* target,D3D12_RESOURCE_STATES state){
        std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(phase!=Phase::Ready||retiring||!list||(!gameProxy&&target!=textures->inputTexture())||!textures->compatibleInput(target,bool(snapshot)))return false;
        if(snapshot)externalTarget=target;else if(gameProxy)textures->rebindInput(target);
        phase=Phase::Recorded;unsubmitted=true;motionCaptureOnly=false;inferenceTemporalUsed=false;
        temporalTraceState.consumerHadInput=temporalFrameValid;temporalTraceState.consumerReset=temporalReset;temporalTraceState.consumerProducerFrame=temporalTraceState.producerFrame;
        try{ExternalCommandRecording scope(context,list);NativeProfileRange frameRange(context->profiler.get(),list,"frame");
            auto sourceState=state;
            if(snapshot){NativeProfileRange profile(context->profiler.get(),list,"snapshot");transition(list,target,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
                if(snapshotState!=D3D12_RESOURCE_STATE_COPY_DEST)transition(list,snapshot.Get(),snapshotState,D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyResource(snapshot.Get(),target);snapshotState=D3D12_RESOURCE_STATE_COPY_DEST;
                transition(list,target,D3D12_RESOURCE_STATE_COPY_SOURCE,state);sourceState=snapshotState;}
            if(nativeTiles){
                {NativeProfileRange profile(context->profiler.get(),list,"clear_tiles");textures->clearTiles();}
                for(UINT y=0;y<textures->workHeight();y+=textures->tileStride())for(UINT x=0;x<textures->workWidth();x+=textures->tileStride()){
                    textures->selectTile(x,y);
                    {NativeProfileRange profile(context->profiler.get(),list,"texture_input");textures->recordExternalInput(sourceState,sourceState);}
                    {NativeProfileRange profile(context->profiler.get(),list,"features");features->record();}
                    {NativeProfileRange profile(context->profiler.get(),list,"graph");graph->record();}
                    {NativeProfileRange profile(context->profiler.get(),list,"composition");composition->record();}
                    {NativeProfileRange profile(context->profiler.get(),list,"accumulate");textures->accumulateTile(composition->resultTensor());}++modelPasses;
                }
            }else{
                textures->recordExternalInput(sourceState,sourceState);
                if(temporal&&temporalFrameValid){
                    inferenceTemporalUsed=true;++temporalTraceState.temporalConsumers;
                    temporal->settings(temporalFrameValid,temporalReset,.02f,true);temporal->recordFeatures();
                    auto source=temporal->featureTensor(),dest=features->resultTensor();
                    source->to(list,D3D12_RESOURCE_STATE_COPY_SOURCE);dest->to(list,D3D12_RESOURCE_STATE_COPY_DEST);
                    list->CopyBufferRegion(dest->resource.Get(),0,source->resource.Get(),0,source->elements*4);
                    graph->record();temporal->recordComposition();
                    source=temporal->composedTensor();dest=composition->resultTensor();
                    source->to(list,D3D12_RESOURCE_STATE_COPY_SOURCE);dest->to(list,D3D12_RESOURCE_STATE_COPY_DEST);
                    list->CopyBufferRegion(dest->resource.Get(),0,source->resource.Get(),0,source->elements*4);
                }else{++temporalTraceState.spatialConsumers;features->record();graph->record();composition->record();}
                ++modelPasses;
            }
            {NativeProfileRange profile(context->profiler.get(),list,"texture_output");textures->recordExternalOutput(composition->resultTensor(),temporaryState,D3D12_RESOURCE_STATE_COPY_SOURCE);}
            temporaryState=D3D12_RESOURCE_STATE_COPY_SOURCE;
            if(snapshot){transition(list,target,state,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyResource(target,temporary.Get());transition(list,target,D3D12_RESOURCE_STATE_COPY_DEST,state);}
            else textures->recordCopyBackToInput(state,state);++records;return true;
        }catch(...){phase=Phase::Poisoned;damaged=true;return false;} // Never retry an advanced/failed tracker.
    }
    void notifySubmitted(ID3D12Fence* fence,UINT64 value){
        std::lock_guard<std::mutex> lock(mutex);
        if((phase!=Phase::Recorded&&!(phase==Phase::Poisoned&&unsubmitted))||!fence||!value||value==~UINT64(0))throw std::runtime_error("Invalid session submission marker");
        ComPtr<ID3D12Device> owner;check(fence->GetDevice(IID_PPV_ARGS(&owner)),"Completion fence device");
        ComPtr<IUnknown> a,b;check(owner.As(&a),"Fence device identity");check(context->device.As(&b),"Session device identity");
        if(a.Get()!=b.Get()||(completion.Get()==fence&&value<=completionValue))throw std::runtime_error("Session fence mismatch or nonmonotonic value");
        completion=fence;completionValue=value;phase=Phase::InFlight;unsubmitted=false;
        submissionChanged.notify_all();
        if(motionCaptureOnly)++temporalTraceState.captureSubmissions;
        if(temporal&&!damaged&&!motionCaptureOnly){if(inferenceTemporalUsed){temporal->advanceSubmittedFrame();temporalReset=false;}else temporalReset=true;temporalFrameValid=false;}
    }
    void requestRetire(){std::lock_guard<std::mutex> lock(mutex);retiring=true;pollLocked();if(phase==Phase::Ready)phase=Phase::Retired;}
    bool retired(){std::lock_guard<std::mutex> lock(mutex);pollLocked();return phase==Phase::Retired;}
    // Caller attests the recorded/failed list was discarded and will never execute.
    void cancelUnsubmitted(){std::lock_guard<std::mutex> lock(mutex);
        if(!unsubmitted||(phase!=Phase::Recorded&&phase!=Phase::Poisoned))throw std::runtime_error("Cannot cancel submitted session");
        retiring=true;phase=Phase::Retired;
        submissionChanged.notify_all();
    }
    std::shared_ptr<const NativeNrHook::Binding> binding(unsigned handle){
        auto desc=textures->inputTexture()->GetDesc();auto result=std::make_shared<NativeNrHook::Binding>();
        result->handleId=handle;result->width=UINT(desc.Width);result->height=desc.Height;result->format=desc.Format;
        result->output=textures->inputTexture();result->user=this;result->owner=shared_from_this();
        result->allowCompatibleRotation=gameProxy;
        result->record=[](void* user,ID3D12GraphicsCommandList* list,ID3D12Resource* target,D3D12_RESOURCE_STATES state){return static_cast<NativeNrSession*>(user)->record(list,target,state);};
        result->ready=[](void* user){return static_cast<NativeNrSession*>(user)->ready();};return result;
    }
    UINT64 allocatedBytes()const{return context->defaultBufferBytes;}
    ID3D12Resource* outputResource()const{return snapshot?externalTarget.Get():textures->inputTexture();}
    UINT recordedFrames()const{return records;}
    UINT graphPasses()const{return modelPasses;}
    UINT passesPerFrame()const{return nativeTiles?textures->tileCount():1;}
    UINT processingWidth()const{return textures->workWidth();}
    UINT processingHeight()const{return textures->workHeight();}
    void setProcessingExtent(UINT width,UINT height){std::lock_guard<std::mutex> lock(mutex);pollLocked();
        if(!nativeTiles||phase!=Phase::Ready||retiring)throw std::runtime_error("Resolution change requires a ready tile session");
        textures->setProcessingExtent(width,height);}
    std::vector<float> captureTexture(const std::string& packedPath){requireCapture();return textures->downloadTexture(packedPath,true);}
    std::vector<float> captureFeatures(){requireCapture();return features->download(features->resultTensor());}
    std::vector<float> captureRgb(){requireCapture();return composition->download(composition->resultTensor());}
    GraphPipeline& captureGraph(){requireCapture();return *graph;}
};

class NativeNrSessions {
    std::map<unsigned,std::shared_ptr<NativeNrSession>> active;
    std::vector<std::shared_ptr<NativeNrSession>> pending;
public:
    // Registry preparation/retirement/collection is serialized by its caller.
    NativeNrSessions()=default;
    NativeNrSessions(const NativeNrSessions&)=delete;
    NativeNrSessions& operator=(const NativeNrSessions&)=delete;
    std::shared_ptr<NativeNrSession> prepare(unsigned handle,ID3D12Resource* target,const std::string& folder,bool publish=true){
        if(active.count(handle))throw std::runtime_error("Duplicate NR feature handle");
        auto session=std::make_shared<NativeNrSession>(target,folder);
        return adopt(handle,session,publish);
    }
    // Construction may run on a worker; publication remains caller-serialized.
    std::shared_ptr<NativeNrSession> adopt(unsigned handle,const std::shared_ptr<NativeNrSession>& session,bool publish=true){
        if(!session||active.count(handle))throw std::runtime_error("Duplicate or missing NR session");
        if(publish)NativeNrHook::RegisterOwned(session->binding(handle));
        try{active.emplace(handle,session);}catch(...){if(publish)NativeNrHook::Unregister(handle);throw;}return session;
    }
    void retire(unsigned handle){auto found=active.find(handle);if(found==active.end())return;
        NativeNrHook::Unregister(handle);found->second->requestRetire();pending.push_back(found->second);active.erase(found);}
    size_t collect(){size_t before=pending.size(),cancelled=0;
        for(auto it=active.begin();it!=active.end();){if(it->second->retired()){NativeNrHook::Unregister(it->first);it=active.erase(it);++cancelled;}else ++it;}
        pending.erase(std::remove_if(pending.begin(),pending.end(),[](auto& s){return s->retired();}),pending.end());return cancelled+before-pending.size();}
    size_t pendingCount()const{return pending.size();}
    std::shared_ptr<NativeNrSession> lookup(unsigned handle)const{auto found=active.find(handle);return found==active.end()?nullptr:found->second;}
    void requestShutdown(){while(!active.empty())retire(active.begin()->first);}
    size_t activeCount()const{return active.size();}
    ~NativeNrSessions(){
        // Destruction with pending GPU work is a caller contract error: fail closed
        // instead of silently releasing buffers the GPU may still access.
        for(auto& entry:active){NativeNrHook::Unregister(entry.first);entry.second->requestRetire();if(!entry.second->retired())std::terminate();}
        for(auto& s:pending)if(!s->retired())std::terminate();
    }
};
