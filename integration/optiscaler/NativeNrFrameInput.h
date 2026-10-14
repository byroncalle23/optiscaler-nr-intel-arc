#pragma once
#include <d3d12.h>
#include <cstdint>
#include <cmath>

namespace NativeNrTemporal {
struct ResourceView {
    ID3D12Resource* resource=nullptr;
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON;
    bool stateKnown=false;
};
// Borrowed resources are valid only inside the callback on the Evaluate list.
// A Present consumer must own GPU snapshots and associate them with this frame;
// AddRef alone does not prevent the engine from overwriting or aliasing storage.
struct FrameInput {
    unsigned handle=0;
    std::uint64_t featureFrame=0;
    ResourceView colour,depth,motion,output,reactive;
    unsigned renderWidth=0,renderHeight=0,displayWidth=0,displayHeight=0;
    float motionScaleX=0,motionScaleY=0,jitterX=0,jitterY=0;
    float cameraNear=0,cameraFar=0;
    bool cameraRangeKnown=false;
    bool scalesKnown=false,jitterKnown=false,reset=false;
    bool lowResolutionMotion=false,jitteredMotion=false,invertedDepth=false;
    // Must be established by the engine adapter, not inferred from texture size.
    bool motionConventionVerified=false;
    unsigned motionReferenceWidth=0,motionReferenceHeight=0;
};
inline bool CanSnapshot(const FrameInput& f) {
    return f.depth.resource&&f.motion.resource&&f.depth.stateKnown&&f.motion.stateKnown&&
           f.renderWidth&&f.renderHeight&&f.displayWidth&&f.displayHeight&&f.scalesKnown&&
           std::isfinite(f.motionScaleX)&&std::isfinite(f.motionScaleY)&&f.motionScaleX!=0&&f.motionScaleY!=0&&f.jitterKnown&&
           std::isfinite(f.jitterX)&&std::isfinite(f.jitterY);
}
inline bool CanReproject(const FrameInput& f) {
    return CanSnapshot(f)&&f.motionConventionVerified&&f.motionReferenceWidth&&f.motionReferenceHeight;
}
struct NormalizedMotion {float u,v;};
inline NormalizedMotion Normalize(const FrameInput& f,float x,float y,float previousMinusCurrentJitterX=0,float previousMinusCurrentJitterY=0) {
    if(!CanReproject(f)||!std::isfinite(x)||!std::isfinite(y)||
       !std::isfinite(previousMinusCurrentJitterX)||!std::isfinite(previousMinusCurrentJitterY))return {NAN,NAN};
    // Render/display reference extent is independent of the NR proxy's 640x360.
    return {(x*f.motionScaleX+previousMinusCurrentJitterX)/f.motionReferenceWidth,
            (y*f.motionScaleY+previousMinusCurrentJitterY)/f.motionReferenceHeight};
}
}
