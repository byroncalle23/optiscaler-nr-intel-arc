#define main previousFrameContractMain
#include "nr_frame_input_test.cpp"
#undef main
#include "../../integration/optiscaler/NativeNrFfxBridge.h"
int main(){try{
    require(previousFrameContractMain()==0);
    Params p;auto resource=reinterpret_cast<ID3D12Resource*>(uintptr_t(1));
    p.Set(NVSDK_NGX_Parameter_Depth,resource);p.Set(NVSDK_NGX_Parameter_MotionVectors,resource);
    p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,1280u);p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,720u);
    p.Set("FSR.upscaleSize.width",1920u);p.Set("FSR.upscaleSize.height",1080u);
    p.Set(NVSDK_NGX_Parameter_MV_Scale_X,1280.f);p.Set(NVSDK_NGX_Parameter_MV_Scale_Y,720.f);
    p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,0.f);p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f);
    ffxDispatchDescUpscale d{};d.motionVectors.state=4;d.depth.state=12;
    d.renderSize.width=1280;d.renderSize.height=720;d.reactive.resource=resource;
    d.reactive.state=4;d.reactive.description.width=1280;d.reactive.description.height=720;
    NativeNrPublishFfxMetadata(&p,d);
    auto f=NativeNrHook::ReadFrameInputs(1,1,&p,NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
    require(NativeNrTemporal::CanReproject(f)&&f.motionReferenceWidth==1280&&f.motionReferenceHeight==720);
    unsigned flag=0;p.Get("NR.ReactiveStateKnown",&flag);require(flag==1);
    d.motionVectors.state=0;d.depth.state=0xffff;d.reactive.description.width=1279;
    NativeNrPublishFfxMetadata(&p,d);
    f=NativeNrHook::ReadFrameInputs(1,2,&p,0);
    require(!f.motion.stateKnown&&!f.depth.stateKnown&&!NativeNrTemporal::CanReproject(f));
    p.Get("NR.ReactiveStateKnown",&flag);require(flag==0);
    d.motionVectors.state=4;d.depth.state=12;d.reactive.resource=nullptr;
    NativeNrPublishFfxMetadata(&p,d);f=NativeNrHook::ReadFrameInputs(1,3,&p,0);
    require(NativeNrTemporal::CanReproject(f)&&f.motionReferenceWidth==1920&&f.motionReferenceHeight==1080);
    puts("FFX_BRIDGE_VALID_UNKNOWN_REFRESH_AND_RENDER_DISPLAY_REFERENCE_PASS");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
