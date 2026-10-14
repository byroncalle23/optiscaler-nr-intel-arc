#pragma once
#include <nvsdk_ngx_params.h>
#include "ffx_upscale.h"
#include "NativeNrFfxState.h"

// Both FFX DLL and executable-inline entry points expose the same dispatch ABI.
// Refresh every dispatch; never infer resource states or retain stale known flags.
inline void NativeNrPublishFfxMetadata(NVSDK_NGX_Parameter* params,const ffxDispatchDescUpscale& dispatch) {
    D3D12_RESOURCE_STATES mv{},depth{},reactive{};
    const bool mvKnown=NativeNrFfxState(dispatch.motionVectors.state,mv);
    const bool depthKnown=NativeNrFfxState(dispatch.depth.state,depth);
    const bool reactiveKnown=dispatch.reactive.resource&&
        dispatch.reactive.description.width>=dispatch.renderSize.width&&
        dispatch.reactive.description.height>=dispatch.renderSize.height&&
        NativeNrFfxState(dispatch.reactive.state,reactive);
    params->Set("NR.MotionStateKnown",unsigned(mvKnown));params->Set("NR.DepthStateKnown",unsigned(depthKnown));
    params->Set("NR.MotionState",unsigned(mv));params->Set("NR.DepthState",unsigned(depth));
    params->Set("NR.FfxMotionContract",1u);
    params->Set("NR.ReactiveStateKnown",unsigned(reactiveKnown));params->Set("NR.ReactiveState",unsigned(reactive));
}
