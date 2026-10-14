#pragma once
#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <stdexcept>
#include "NativeNrLifecycle.h"

// Experimental post-upscale callback. Registration/initialization must finish
// outside Evaluate; the immutable binding and GPU objects live through the fence.
namespace NativeNrHook {
enum class Result { Skipped, Recorded, Failed };
struct Binding {
    unsigned handleId,width,height;
    DXGI_FORMAT format;
    ID3D12Resource* output;
    void* user;
    bool (*record)(void*,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
    std::shared_ptr<void> owner;
    bool (*ready)(void*)=nullptr;
    bool allowCompatibleRotation=false;
};
inline std::atomic<const Binding*> active{nullptr};
inline std::mutex bindingsMutex;
inline std::map<unsigned,std::shared_ptr<const Binding>> bindings;
inline void Register(const Binding* binding){active.store(binding,std::memory_order_release);}
inline void RegisterOwned(std::shared_ptr<const Binding> binding){
    if(!binding)return;std::lock_guard<std::mutex> lock(bindingsMutex);
    if(bindings.count(binding->handleId))throw std::runtime_error("Duplicate owned NR binding");
    bindings.emplace(binding->handleId,std::move(binding));
}
inline void Unregister(unsigned handle){std::lock_guard<std::mutex> lock(bindingsMutex);bindings.erase(handle);}
inline ID3D12Resource* GetOutput(const NVSDK_NGX_Parameter* params){
    if(!params)return nullptr;ID3D12Resource* output=nullptr;
    if(params->Get(NVSDK_NGX_Parameter_Output,&output)==NVSDK_NGX_Result_Success)return output;
    void* pointer=nullptr;if(params->Get(NVSDK_NGX_Parameter_Output,&pointer)!=NVSDK_NGX_Result_Success)return nullptr;
    return static_cast<ID3D12Resource*>(pointer);
}
inline ID3D12Resource* GetResource(const NVSDK_NGX_Parameter* params,const char* key){
    ID3D12Resource* resource=nullptr;if(params->Get(key,&resource)==NVSDK_NGX_Result_Success)return resource;
    void* pointer=nullptr;if(params->Get(key,&pointer)!=NVSDK_NGX_Result_Success)return nullptr;return static_cast<ID3D12Resource*>(pointer);
}
inline NativeNrTemporal::FrameInput ReadFrameInputs(unsigned handle,UINT64 frame,const NVSDK_NGX_Parameter* params,unsigned flags){
    NativeNrTemporal::FrameInput f{};f.handle=handle;f.featureFrame=frame;if(!params)return f;
    f.colour.resource=GetResource(params,NVSDK_NGX_Parameter_Color);f.depth.resource=GetResource(params,NVSDK_NGX_Parameter_Depth);
    f.motion.resource=GetResource(params,NVSDK_NGX_Parameter_MotionVectors);f.output.resource=GetOutput(params);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,&f.renderWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,&f.renderHeight);
    params->Get("FSR.upscaleSize.width",&f.displayWidth);params->Get("FSR.upscaleSize.height",&f.displayHeight);
    f.scalesKnown=params->Get(NVSDK_NGX_Parameter_MV_Scale_X,&f.motionScaleX)==NVSDK_NGX_Result_Success;
    f.scalesKnown=(params->Get(NVSDK_NGX_Parameter_MV_Scale_Y,&f.motionScaleY)==NVSDK_NGX_Result_Success)&&f.scalesKnown;
    f.jitterKnown=params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&f.jitterX)==NVSDK_NGX_Result_Success;
    f.jitterKnown=(params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&f.jitterY)==NVSDK_NGX_Result_Success)&&f.jitterKnown;
    unsigned reset=0;params->Get(NVSDK_NGX_Parameter_Reset,&reset);f.reset=reset!=0;
    f.cameraRangeKnown=params->Get("FSR.cameraNear",&f.cameraNear)==NVSDK_NGX_Result_Success;
    f.cameraRangeKnown=(params->Get("FSR.cameraFar",&f.cameraFar)==NVSDK_NGX_Result_Success)&&f.cameraRangeKnown&&std::isfinite(f.cameraNear)&&std::isfinite(f.cameraFar)&&f.cameraNear>0&&f.cameraFar>f.cameraNear;
    // Only explicit FFX-dispatch states are accepted here, not backend guesses.
    auto state=[&](const char* known,const char* key,NativeNrTemporal::ResourceView& view){unsigned valid=0,value=0;
        if(params->Get(known,&valid)==NVSDK_NGX_Result_Success&&valid&&params->Get(key,&value)==NVSDK_NGX_Result_Success){view.state=static_cast<D3D12_RESOURCE_STATES>(value);view.stateKnown=true;}};
    state("NR.MotionStateKnown","NR.MotionState",f.motion);state("NR.DepthStateKnown","NR.DepthState",f.depth);
    state("NR.ReactiveStateKnown","NR.ReactiveState",f.reactive);
    if(f.reactive.stateKnown)f.reactive.resource=GetResource(params,"FSR.reactive");
    f.lowResolutionMotion=(flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)!=0;
    f.jitteredMotion=(flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;
    f.invertedDepth=(flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0;
    // Declared FFX API convention, matching the bundled FSR3 backend's normalization.
    // Live quality still needs validation; unknown APIs cannot enable this path.
    unsigned ffxContract=0;params->Get("NR.FfxMotionContract",&ffxContract);
    if(ffxContract==1){f.motionConventionVerified=true;f.motionReferenceWidth=f.lowResolutionMotion?f.renderWidth:f.displayWidth;f.motionReferenceHeight=f.lowResolutionMotion?f.renderHeight:f.displayHeight;}
    return f;
}
inline Result Evaluate(unsigned handle,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* params,
                       bool hdr,bool knownState,D3D12_RESOURCE_STATES outputState){
    std::shared_ptr<const Binding> retained;
    {std::lock_guard<std::mutex> lock(bindingsMutex);auto found=bindings.find(handle);if(found!=bindings.end())retained=found->second;}
    const Binding* binding=retained?retained.get():active.load(std::memory_order_acquire);
    if(hdr||!knownState)return Result::Skipped;
    if(!binding||handle!=binding->handleId){
        if(list&&list->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT)if(auto output=GetOutput(params))NativeNrLifecycle::Observe(handle,output);
        return Result::Skipped;
    }
    if(!list||!params||!binding->record||!binding->output)return Result::Failed;
    ID3D12Resource* output=GetOutput(params);
    if(output!=binding->output&&!binding->allowCompatibleRotation){
        if(output&&list->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT)NativeNrLifecycle::Observe(handle,output);
        return Result::Skipped; // Worker retires/rebuilds; never change descriptors on an outstanding list.
    }
    if(binding->ready&&!binding->ready(binding->user))return Result::Skipped;
    auto desc=output->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=binding->width||desc.Height!=binding->height||desc.Format!=binding->format||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1){
        if(binding->allowCompatibleRotation&&list->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT)NativeNrLifecycle::Observe(handle,output);
        return Result::Skipped;
    }
    if(!binding->record(binding->user,list,output,outputState))return Result::Failed;
    // A failed observer must retain the registered session: GPU commands have
    // already been recorded. Do not release it or replay to recover here.
    return NativeNrLifecycle::Recorded(handle,list)?Result::Recorded:Result::Failed;
}
}
