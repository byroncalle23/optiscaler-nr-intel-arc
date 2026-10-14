#pragma once
#include "nr_temporal.h"
#include "../../integration/optiscaler/NativeNrFrameInput.h"

struct NativeMotionGeometry {D3D12_RESOURCE_DESC motion{},depth{},reactive{};};
// Owned GPU snapshots outlive the borrowed engine buffers. Construction happens
// on the preparation worker; record() enqueues copies/normalization only.
class NativeMotionBridge : public NativeRuntime {
    NativeMotionGeometry geometry;
    ComPtr<ID3D12Resource> motionSnapshot,depthSnapshot,reactiveSnapshot;
    D3D12_RESOURCE_STATES motionState=D3D12_RESOURCE_STATE_COMMON,depthState=D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES reactiveState=D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> shader;
    ComPtr<ID3D12PipelineState> clearShader,projectShader,rejectShader;
    Tensor *currentDepth,*reconstructedDepth;
    std::shared_ptr<NativeTemporalInputs> outputs;
    ComPtr<ID3D12Resource> retainedMotion,retainedDepth,retainedReactive;
    static bool planarDepth(DXGI_FORMAT f){return f==DXGI_FORMAT_R32G8X24_TYPELESS||f==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;}
    static DXGI_FORMAT depthSrv(DXGI_FORMAT f){if(planarDepth(f))return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;if(f==DXGI_FORMAT_R32_FLOAT||f==DXGI_FORMAT_R32_TYPELESS||f==DXGI_FORMAT_D32_FLOAT)return DXGI_FORMAT_R32_FLOAT;throw std::runtime_error("Unsupported NR depth format");}
    ComPtr<ID3D12Resource> texture(D3D12_RESOURCE_DESC d){
        d.Flags=D3D12_RESOURCE_FLAG_NONE;
        if(d.Format==DXGI_FORMAT_D32_FLOAT)d.Format=DXGI_FORMAT_R32_TYPELESS;
        if(d.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)d.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
        D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_DEFAULT;p.CreationNodeMask=p.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"NR motion snapshot");return r;
    }
    static bool shape(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b){return a.Dimension==b.Dimension&&a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format&&a.MipLevels==1&&a.DepthOrArraySize==1&&a.SampleDesc.Count==1;}
public:
    static bool usableReactive(const D3D12_RESOURCE_DESC& d){return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width&&d.Width<=3840&&d.Height&&d.Height<=2160&&d.MipLevels==1&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1&&(d.Format==DXGI_FORMAT_R8_UNORM||d.Format==DXGI_FORMAT_R16_UNORM||d.Format==DXGI_FORMAT_R16_FLOAT||d.Format==DXGI_FORMAT_R32_FLOAT);}
    bool usesReactive() const{return bool(reactiveSnapshot);}
    NativeMotionBridge(std::shared_ptr<NativeContext> ctx,NativeMotionGeometry g,std::shared_ptr<NativeTemporalInputs> target):NativeRuntime(ctx),geometry(g),outputs(std::move(target)) {
        if(!outputs||outputs->motionTensor()->elements!=UINT64(outputs->lowWidth())*outputs->lowHeight()*2||outputs->confidenceTensor()->elements!=UINT64(outputs->lowWidth())*outputs->lowHeight())throw std::runtime_error("Invalid motion output arena");
        for(auto d:{g.motion,g.depth})if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!d.Width||!d.Height||d.Width>3840||d.Height>2160||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1)throw std::runtime_error("Invalid NR motion geometry");
        if(g.motion.Format!=DXGI_FORMAT_R16G16_FLOAT&&g.motion.Format!=DXGI_FORMAT_R32G32_FLOAT)throw std::runtime_error("Unsupported NR motion format");
        auto df=depthSrv(g.depth.Format);motionSnapshot=texture(g.motion);depthSnapshot=texture(g.depth);
        // Optional mask never prevents the required motion/depth bridge from working.
        if(usableReactive(g.reactive))reactiveSnapshot=texture(g.reactive);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"Motion heap");
        auto h=heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=g.motion.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(motionSnapshot.Get(),&srv,h);h.ptr+=device->GetDescriptorHandleIncrementSize(hd.Type);srv.Format=df;device->CreateShaderResourceView(depthSnapshot.Get(),&srv,h);
        h.ptr+=device->GetDescriptorHandleIncrementSize(hd.Type);srv.Format=reactiveSnapshot?g.reactive.Format:df;device->CreateShaderResourceView(reactiveSnapshot?reactiveSnapshot.Get():depthSnapshot.Get(),&srv,h);
        currentDepth=make(UINT64(outputs->lowWidth())*outputs->lowHeight(),false);reconstructedDepth=make(UINT64(outputs->lowWidth())*outputs->lowHeight(),false);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};D3D12_ROOT_PARAMETER params[6]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
        for(UINT i=1;i<=4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[i].Descriptor.ShaderRegister=i-1;}
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[5].Constants.Num32BitValues=20;
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=6;desc.pParameters=params;ComPtr<ID3DBlob> blob,error;
        check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Motion root serialize");check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Motion root");
        blob=compileNativeShader("motion_ops.hlsl","NormalizeMotion");D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={blob->GetBufferPointer(),blob->GetBufferSize()};check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&shader)),"Motion PSO");
        auto compile=[&](const char* entry,ComPtr<ID3D12PipelineState>& target){auto b=compileNativeShader("motion_ops.hlsl",entry);p.CS={b->GetBufferPointer(),b->GetBufferSize()};check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(&target)),entry);};
        compile("ClearProjectedDepth",clearShader);compile("ProjectCurrentDepth",projectShader);compile("RejectOccludedHistory",rejectShader);
    }
    bool compatible(const NativeNrTemporal::FrameInput& f){return NativeNrTemporal::CanReproject(f)&&shape(f.motion.resource->GetDesc(),geometry.motion)&&shape(f.depth.resource->GetDesc(),geometry.depth)&&
        f.renderWidth<=geometry.depth.Width&&f.renderHeight<=geometry.depth.Height&&
        (f.lowResolutionMotion?f.renderWidth:f.displayWidth)<=geometry.motion.Width&&(f.lowResolutionMotion?f.renderHeight:f.displayHeight)<=geometry.motion.Height&&
        (!reactiveSnapshot||!(f.reactive.resource&&f.reactive.stateKnown)||(shape(f.reactive.resource->GetDesc(),geometry.reactive)&&f.renderWidth<=geometry.reactive.Width&&f.renderHeight<=geometry.reactive.Height));}
    bool sameGeometry(const NativeMotionGeometry& g){return shape(g.motion,geometry.motion)&&shape(g.depth,geometry.depth)&&g.reactive.Width==geometry.reactive.Width&&(!g.reactive.Width||shape(g.reactive,geometry.reactive));}
    void appendMotionStates(std::vector<NativeTrackedState>& s){appendTrackedStates(s);s.push_back({motionSnapshot.Get(),&motionState});s.push_back({depthSnapshot.Get(),&depthState});if(reactiveSnapshot)s.push_back({reactiveSnapshot.Get(),&reactiveState});}
    void record(const NativeNrTemporal::FrameInput& f,float jitterCorrectionX,float jitterCorrectionY){
        if(!compatible(f))throw std::runtime_error("Motion packet incompatible");
        for(auto resource:{f.motion.resource,f.depth.resource,reactiveSnapshot?f.reactive.resource:nullptr}){if(!resource)continue;ComPtr<ID3D12Device> owner;check(resource->GetDevice(IID_PPV_ARGS(&owner)),"Motion owner");if(!sameNativeDevice(owner.Get(),device.Get()))throw std::runtime_error("Motion device mismatch");}
        retainedMotion=f.motion.resource;retainedDepth=f.depth.resource;
        retainedReactive=reactiveSnapshot?f.reactive.resource:nullptr;
        auto copy=[&](ID3D12Resource* src,D3D12_RESOURCE_STATES before,ID3D12Resource* dst,D3D12_RESOURCE_STATES& state,bool depthPlane=false){
            // Engine metadata describes the sampled depth plane, not stencil.
            // Only transition/copy plane zero of combined depth/stencil inputs.
            auto sourceTransition=[&](D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
                if(!depthPlane){transition(cmd.Get(),src,a,b);return;}
                D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={src,0,a,b};cmd->ResourceBarrier(1,&barrier);
            };
            if(before!=D3D12_RESOURCE_STATE_COPY_SOURCE)sourceTransition(before,D3D12_RESOURCE_STATE_COPY_SOURCE);
            if(state!=D3D12_RESOURCE_STATE_COPY_DEST)transition(cmd.Get(),dst,state,D3D12_RESOURCE_STATE_COPY_DEST);
            if(depthPlane){D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=src;to.pResource=dst;from.Type=to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=to.SubresourceIndex=0;cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
            else cmd->CopyResource(dst,src);
            state=D3D12_RESOURCE_STATE_COPY_DEST;
            if(before!=D3D12_RESOURCE_STATE_COPY_SOURCE)sourceTransition(D3D12_RESOURCE_STATE_COPY_SOURCE,before);
            transition(cmd.Get(),dst,state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        };
        copy(f.motion.resource,f.motion.state,motionSnapshot.Get(),motionState);copy(f.depth.resource,f.depth.state,depthSnapshot.Get(),depthState,planarDepth(geometry.depth.Format));
        bool reactive=reactiveSnapshot&&f.reactive.resource&&f.reactive.stateKnown;
        if(reactive)copy(f.reactive.resource,f.reactive.state,reactiveSnapshot.Get(),reactiveState);
        else if(reactiveSnapshot&&reactiveState!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE){transition(cmd.Get(),reactiveSnapshot.Get(),reactiveState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);reactiveState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}
        outputs->motionTensor()->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);outputs->confidenceTensor()->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        currentDepth->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);reconstructedDepth->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        UINT words[20]={outputs->lowWidth(),outputs->lowHeight(),f.lowResolutionMotion?f.renderWidth:f.displayWidth,f.lowResolutionMotion?f.renderHeight:f.displayHeight,f.renderWidth,f.renderHeight,UINT(f.invertedDepth),1};words[12]=f.renderWidth;words[13]=f.renderHeight;words[14]=reactive;
        float depthParams[]={f.cameraNear,f.cameraFar,.05f};memcpy(words+16,depthParams,sizeof(depthParams));words[19]=f.cameraRangeKnown;
        float scales[]={f.motionScaleX/f.motionReferenceWidth,f.motionScaleY/f.motionReferenceHeight,jitterCorrectionX,jitterCorrectionY};memcpy(words+8,scales,sizeof(scales));
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(shader.Get());ID3D12DescriptorHeap* heaps[]={heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());cmd->SetComputeRootUnorderedAccessView(1,outputs->motionTensor()->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(2,outputs->confidenceTensor()->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(3,currentDepth->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(4,reconstructedDepth->resource->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstants(5,20,words,0);
        for(auto ps:{clearShader.Get(),shader.Get(),projectShader.Get(),rejectShader.Get()}){cmd->SetPipelineState(ps);cmd->Dispatch((outputs->lowWidth()+7)/8,(outputs->lowHeight()+7)/8,1);uavBarrier(cmd.Get());}
    }
};
