#include "nr_motion.h"
#include <cstdio>
class MotionTestHost : public NativeRuntime {
public:
    ComPtr<ID3D12Resource> input(DXGI_FORMAT format,UINT pitch,const void* data,bool depth=false){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=1280;d.Height=720;d.MipLevels=d.DepthOrArraySize=1;d.SampleDesc.Count=1;d.Format=format;if(depth)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_DEFAULT;p.CreationNodeMask=p.VisibleNodeMask=1;ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"Motion test input");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes,rowBytes;device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,&rowBytes,&bytes);
        auto up=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped;D3D12_RANGE empty{0,0};check(up->Map(0,&empty,&mapped),"Input upload");
        bool planar=format==DXGI_FORMAT_R32G8X24_TYPELESS||format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        if(planar&&(pitch!=1280*4||(rowBytes!=1280*4&&rowBytes!=1280*8)))throw std::runtime_error("Unexpected planar depth upload footprint");
        if(!planar&&rowBytes!=pitch)throw std::runtime_error("Unexpected input row size");
        for(UINT y=0;y<720;++y){auto dst=static_cast<char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch;auto src=static_cast<const char*>(data)+UINT64(y)*pitch;
            if(planar){memset(dst,0,size_t(rowBytes));for(UINT x=0;x<1280;++x)memcpy(dst+UINT64(x)*(rowBytes/1280),src+x*4,4);}else memcpy(dst,src,pitch);
        }up->Unmap(0,&empty);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=r.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=up.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;
        cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);
        if(planar){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={r.Get(),1,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON};cmd->ResourceBarrier(1,&barrier);}
        uploads.push_back(up);return r;
    }
    void debug(){if(!diagnostics)throw std::runtime_error("Debug layer required");for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i){SIZE_T n=0;diagnostics->GetMessage(i,nullptr,&n);std::vector<char> a(n);auto p=reinterpret_cast<D3D12_MESSAGE*>(a.data());diagnostics->GetMessage(i,p,&n);if(p->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(p->pDescription);}}
};
int main(){try{
    SetEnvironmentVariableA("NR_D3D12_DEBUG","1");MotionTestHost host;auto ctx=host.sharedContext();
    std::vector<UINT16> mv(1280*720*2);for(size_t i=0;i<mv.size();i+=2){mv[i]=0x2400;mv[i+1]=0xa000;} // .015625, -.0078125
    std::vector<float> depth(1280*720,.3f);
    auto m=host.input(DXGI_FORMAT_R16G16_FLOAT,1280*4,mv.data()),d=host.input(DXGI_FORMAT_D32_FLOAT,1280*4,depth.data(),true);host.submit();
    auto inputs=std::make_shared<NativeTemporalInputs>(ctx,640,360);NativeMotionBridge bridge(ctx,{m->GetDesc(),d->GetDesc()},inputs);
    NativeNrTemporal::FrameInput f{};f.motion={m.Get(),D3D12_RESOURCE_STATE_COPY_DEST,true};f.depth={d.Get(),D3D12_RESOURCE_STATE_COPY_DEST,true};
    f.renderWidth=1280;f.renderHeight=720;f.displayWidth=1920;f.displayHeight=1080;f.motionScaleX=1280;f.motionScaleY=-720;f.scalesKnown=f.jitterKnown=f.motionConventionVerified=f.lowResolutionMotion=true;f.motionReferenceWidth=1280;f.motionReferenceHeight=720;
    for(int pass=0;pass<2;++pass){f.invertedDepth=pass!=0;bridge.record(f,.001f,-.002f);auto motion=host.download(inputs->motionTensor()),valid=host.download(inputs->confidenceTensor());
        for(size_t i=0;i<valid.size();++i){float expected=((float(i%640)+.5f)/640+.016625f<=1&&(float(i/640)+.5f)/360+.0058125f<=1)?1.f:0.f;
            if(std::abs(motion[i*2]-.016625f)>1e-7f||std::abs(motion[i*2+1]-.0058125f)>1e-7f||valid[i]!=expected)throw std::runtime_error("GPU motion normalization/bounds mismatch");}}
    std::vector<unsigned char> mask(1280*720,255);auto reactive=host.input(DXGI_FORMAT_R8_UNORM,1280,mask.data());host.submit();
    NativeMotionGeometry geometry{m->GetDesc(),d->GetDesc(),reactive->GetDesc()};NativeMotionBridge reactiveBridge(ctx,geometry,inputs);f.reactive={reactive.Get(),D3D12_RESOURCE_STATE_COPY_DEST,true};reactiveBridge.record(f,0,0);
    auto validity=host.download(inputs->confidenceTensor());for(float value:validity)if(value!=0)throw std::runtime_error("Reactive pixels retained history");
    // The game may supply a packed/typeless optional reactive texture. It must
    // not disable required MV/depth or trigger a preparation retry loop.
    auto unsupported=geometry;unsupported.reactive.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
    NativeMotionBridge ignoredBridge(ctx,unsupported,inputs);
    if(ignoredBridge.usesReactive()||!ignoredBridge.compatible(f))throw std::runtime_error("Unsupported optional reactive blocked motion");
    ignoredBridge.record(f,0,0);auto ignored=host.download(inputs->confidenceTensor());
    bool retained=false;for(float v:ignored)retained=retained||v>0;
    if(!retained)throw std::runtime_error("Ignored optional mask rejected all history");
    puts("GPU_UNSUPPORTED_OPTIONAL_REACTIVE_FALLBACK_PASS");
    f.reactive={};f.cameraNear=.1f;f.cameraFar=1000;f.cameraRangeKnown=true;
    for(int inverted=0;inverted<2;++inverted){
        std::vector<UINT16> objectMotion(1280*720*2,0);std::vector<float> objectDepth(1280*720,inverted?.2f:.8f);
        for(UINT y=200;y<280;++y)for(UINT x=680;x<760;++x){objectMotion[(y*1280+x)*2]=0xa800;objectDepth[y*1280+x]=inverted?.9f:.1f;}
        auto mo=host.input(DXGI_FORMAT_R16G16_FLOAT,1280*4,objectMotion.data()),de=host.input(DXGI_FORMAT_D32_FLOAT,1280*4,objectDepth.data(),true);host.submit();
        NativeMotionBridge occlusionBridge(ctx,{mo->GetDesc(),de->GetDesc()},inputs);f.motion={mo.Get(),D3D12_RESOURCE_STATE_COPY_DEST,true};f.depth={de.Get(),D3D12_RESOURCE_STATE_COPY_DEST,true};f.invertedDepth=inverted!=0;
        occlusionBridge.record(f,0,0);auto confidence=host.download(inputs->confidenceTensor());
        if(confidence[120*640+320]!=0||confidence[120*640+350]!=1||confidence[120*640+100]!=1)throw std::runtime_error("Projected depth did not reject newly exposed background");
        for(auto format:{DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_D32_FLOAT_S8X24_UINT}){
            auto planar=host.input(format,1280*4,objectDepth.data(),true);host.submit();
            NativeMotionBridge planarBridge(ctx,{mo->GetDesc(),planar->GetDesc()},inputs);f.depth={planar.Get(),D3D12_RESOURCE_STATE_COPY_DEST,true};
            for(int replay=0;replay<2;++replay){planarBridge.record(f,0,0);auto actual=host.download(inputs->confidenceTensor());
                if(actual!=confidence)throw std::runtime_error("Planar depth differs from D32 disocclusion reference");}
        }
    }
    host.debug();puts("GPU_PLANAR_DEPTH_TYPELESS_TYPED_PARITY_REPEAT_NORMAL_REVERSED_Z_PASS");puts("GPU_ENGINE_MOTION_COPY_NORMALIZATION_DEBUG_PASS");puts("GPU_REACTIVE_HISTORY_REJECTION_PASS");puts("GPU_PROJECTED_DEPTH_DISOCCLUSION_NORMAL_AND_REVERSED_Z_PASS");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
