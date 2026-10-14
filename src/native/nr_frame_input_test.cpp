#include "../../integration/optiscaler/NativeNrHook.h"
#include "../../integration/optiscaler/NativeNrFfxState.h"
#include <cstdio>
#include <map>
#include <string>
#include <variant>
#include <stdexcept>
static void require(bool v){if(!v)throw std::runtime_error("Frame input contract failed");}
class Params : public NVSDK_NGX_Parameter {
    using Value=std::variant<unsigned,float,void*>;std::map<std::string,Value> values;
public:
    void Set(const char* k,unsigned v) override{values[k]=v;}
    void Set(const char* k,float v) override{values[k]=v;}
    void Set(const char* k,void* v) override{values[k]=v;}
    void Set(const char* k,int v) override{Set(k,unsigned(v));}
    void Set(const char* k,unsigned long long v) override{Set(k,unsigned(v));}
    void Set(const char* k,double v) override{Set(k,float(v));}
    void Set(const char* k,ID3D11Resource* v) override{Set(k,static_cast<void*>(v));}
    void Set(const char* k,ID3D12Resource* v) override{Set(k,static_cast<void*>(v));}
    template<class T> NVSDK_NGX_Result read(const char* k,T* v) const{
        auto it=values.find(k);if(it==values.end())return NVSDK_NGX_Result_FAIL_InvalidParameter;
        if(auto p=std::get_if<T>(&it->second)){*v=*p;return NVSDK_NGX_Result_Success;}return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char* k,unsigned* v) const override{return read(k,v);}
    NVSDK_NGX_Result Get(const char* k,float* v) const override{return read(k,v);}
    NVSDK_NGX_Result Get(const char* k,void** v) const override{return read(k,v);}
    NVSDK_NGX_Result Get(const char*,int*) const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,unsigned long long*) const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,double*) const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,ID3D11Resource**) const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,ID3D12Resource**) const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    void Reset() override{values.clear();}
};
int main(){try {
    Params params;params.Set(NVSDK_NGX_Parameter_Depth,reinterpret_cast<void*>(1));params.Set(NVSDK_NGX_Parameter_MotionVectors,reinterpret_cast<void*>(2));
    params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,1280u);params.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,720u);
    params.Set("FSR.upscaleSize.width",1920u);params.Set("FSR.upscaleSize.height",1080u);
    params.Set(NVSDK_NGX_Parameter_MV_Scale_X,1280.f);params.Set(NVSDK_NGX_Parameter_MV_Scale_Y,-720.f);
    params.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,.25f);params.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,-.25f);
    params.Set("NR.MotionStateKnown",1u);params.Set("NR.DepthStateKnown",1u);
    params.Set("NR.MotionState",unsigned(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));params.Set("NR.DepthState",unsigned(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    auto f=NativeNrHook::ReadFrameInputs(17,99,&params,NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
    require(f.handle==17&&f.featureFrame==99&&f.lowResolutionMotion&&f.invertedDepth&&NativeNrTemporal::CanSnapshot(f));
    require(!NativeNrTemporal::CanReproject(f)); // No sign/reference assumption.
    f.motionConventionVerified=true;f.motionReferenceWidth=1280;f.motionReferenceHeight=720;
    auto motion=NativeNrTemporal::Normalize(f,.125f,.25f);require(std::abs(motion.u-.125f)<1e-7f&&std::abs(motion.v+.25f)<1e-7f);
    auto jitter=NativeNrTemporal::Normalize(f,0,0,.5f,-.5f);require(std::abs(jitter.u-.5f/1280)<1e-7f&&std::abs(jitter.v+.5f/720)<1e-7f);
    params.Set("NR.MotionStateKnown",0u);require(!NativeNrTemporal::CanSnapshot(NativeNrHook::ReadFrameInputs(17,100,&params,0)));
    D3D12_RESOURCE_STATES state{};require(NativeNrFfxState(4,state)&&state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    require(NativeNrFfxState(512,state)&&state==D3D12_RESOURCE_STATE_DEPTH_WRITE);require(!NativeNrFfxState(0,state)&&!NativeNrFfxState(0xffff,state));
    require(!NativeNrLifecycle::FrameInputs(nullptr,f));
    printf("FRAME_INPUT_CONTRACT_PASS\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
