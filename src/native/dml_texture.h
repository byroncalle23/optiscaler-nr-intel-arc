#pragma once
#include "dml_runtime.h"
#include <d3dcompiler.h>

// Standalone typed texture adapter. record methods only enqueue GPU work.
// Resources/descriptors stay alive until the shared context's fence completes.
class TextureHarness : public NativeRuntime {
    UINT shape[12];
    Tensor* colour;
    Tensor* accumulation=nullptr;
    ComPtr<ID3D12Resource> input,output;
    D3D12_RESOURCE_STATES inputState=D3D12_RESOURCE_STATE_COMMON,outputState=D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12PipelineState> readShader,writeShader;
    ComPtr<ID3D12PipelineState> clearShader,accumulateShader;
    UINT increment;
    DXGI_FORMAT format;
    bool globalMode=false;
    UINT pixelBytes()const{return format==DXGI_FORMAT_R32G32B32A32_FLOAT?16:format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;}
    ComPtr<ID3D12Resource> texture(){
        D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;props.CreationNodeMask=props.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=shape[0];desc.Height=shape[1];
        desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result;check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result)),"Frame texture");return result;
    }
    void to(ID3D12Resource* resource,D3D12_RESOURCE_STATES& state,D3D12_RESOURCE_STATES next){
        if(state!=next){transition(cmd.Get(),resource,state,next);state=next;}
    }
    void bind(ID3D12PipelineState* shader,Tensor* rgb){
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(shader);
        ID3D12DescriptorHeap* heaps[]={heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
        cmd->SetComputeRootShaderResourceView(0,rgb->resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(1,colour->resource->GetGPUVirtualAddress());
        auto handle=heap->GetGPUDescriptorHandleForHeapStart();cmd->SetComputeRootDescriptorTable(2,handle);
        handle.ptr+=increment;cmd->SetComputeRootDescriptorTable(3,handle);
        cmd->SetComputeRoot32BitConstants(4,12,shape,0);
        cmd->SetComputeRootUnorderedAccessView(5,(accumulation?accumulation:colour)->resource->GetGPUVirtualAddress());
        UINT width=shader==readShader.Get()?shape[2]:shader==accumulateShader.Get()?shape[6]:shader==clearShader.Get()?shape[10]:shape[0],height=shader==readShader.Get()?shape[3]:shader==accumulateShader.Get()?shape[6]:shader==clearShader.Get()?shape[11]:shape[1];
        cmd->Dispatch((width+7)/8,(height+7)/8,1);uavBarrier(cmd.Get());
    }
public:
    TextureHarness(UINT width,UINT height,std::shared_ptr<NativeContext> existing=nullptr,DXGI_FORMAT textureFormat=DXGI_FORMAT_R32G32B32A32_FLOAT,ID3D12Resource* borrowedInput=nullptr,ID3D12Resource* borrowedOutput=nullptr,UINT modelWidth=0,UINT modelHeight=0,bool nativeTiles=false,bool globalResidual=false):NativeRuntime(existing),shape{width,height,modelWidth?modelWidth:width,modelHeight?modelHeight:height,0,0,256,32,nativeTiles?1u:0u,224,width,height},format(textureFormat),globalMode(globalResidual){
        if(nativeTiles&&(shape[2]!=320||shape[3]!=320))throw std::runtime_error("Native tiles require the 320 model package");
        if(!width||!height||width>3840||height>2160||!shape[2]||!shape[3]||shape[2]>1920||shape[3]>1152)throw std::runtime_error("Invalid texture extent");
        if(format!=DXGI_FORMAT_R32G32B32A32_FLOAT&&format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_R10G10B10A2_UNORM)throw std::runtime_error("Unsupported texture format");
        if(bool(borrowedInput)!=bool(borrowedOutput)||borrowedInput&&borrowedInput==borrowedOutput)throw std::runtime_error("Borrow distinct input/output textures together");
        auto validate=[&](ID3D12Resource* resource,bool writable){
            auto desc=resource->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=width||desc.Height!=height||desc.Format!=format||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality!=0)throw std::runtime_error("Borrowed texture geometry/format mismatch");
            if(writable?!(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):bool(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))throw std::runtime_error("Borrowed texture view unsupported");
            ComPtr<ID3D12Device> owner;check(resource->GetDevice(IID_PPV_ARGS(&owner)),"Borrowed texture device");
            ComPtr<IUnknown> a,b;check(owner.As(&a),"Texture device identity");check(device.As(&b),"Adapter device identity");if(a.Get()!=b.Get())throw std::runtime_error("Borrowed texture device mismatch");
        };
        if(borrowedInput){validate(borrowedInput,false);validate(borrowedOutput,true);input=borrowedInput;output=borrowedOutput;}
        else {input=texture();output=texture();}
        colour=make(UINT64(shape[2])*shape[3]*3);
        // Fixed capacity allows logical resolution changes after the caller fence,
        // without reallocating weights, bindings or any GPU buffer during a frame.
        if(nativeTiles)accumulation=make(UINT64(3840)*2160*4);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)),"Texture format support");
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))throw std::runtime_error("Texture load/store unsupported");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"Texture descriptors");increment=device->GetDescriptorHandleIncrementSize(hd.Type);
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format=format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(input.Get(),&srv,handle);handle.ptr+=increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=srv.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,handle);
        D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,1,0,0};
        D3D12_ROOT_PARAMETER params[6]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[0].Descriptor.ShaderRegister=1;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        for(UINT i=0;i<2;++i){params[i+2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i+2].DescriptorTable={1,&ranges[i]};}
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[4].Constants.Num32BitValues=12;
        params[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[5].Descriptor.ShaderRegister=2;
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=6;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Texture root serialization");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Texture root");
        auto compile=[&](const char* entry,ComPtr<ID3D12PipelineState>& shader){
            D3D_SHADER_MACRO defines[]={{"GLOBAL_RESIDUAL",globalResidual?"1":"0"},{"OUTPUT_HALF",format==DXGI_FORMAT_R16G16B16A16_FLOAT?"1":"0"},{"OUTPUT_UNORM",format==DXGI_FORMAT_R8G8B8A8_UNORM?"1":"0"},{"OUTPUT_UNORM10",format==DXGI_FORMAT_R10G10B10A2_UNORM?"1":"0"},{nullptr,nullptr}};
            blob=compileNativeShader("texture_ops.hlsl",entry,defines);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();pipeline.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&shader)),"Texture pipeline");
        };compile("TextureToRGB",readShader);compile("RGBToTexture",writeShader);
        if(nativeTiles){compile("ClearAccumulation",clearShader);compile("AccumulateTile",accumulateShader);}
    }
    Tensor* colourTensor()const{return colour;}
    void edgeGuidance(bool enabled){if(!globalMode||accumulation)throw std::runtime_error("Edge guidance requires non-tiled global residual");shape[4]=enabled?1:0;}
    ID3D12Resource* inputTexture()const{return input.Get();}
    ID3D12Resource* outputTexture()const{return output.Get();}
    UINT tileStride()const{return shape[9];}
    UINT workWidth()const{return shape[10];}
    void appendTextureStates(std::vector<NativeTrackedState>& states){appendTrackedStates(states);states.push_back({input.Get(),&inputState});states.push_back({output.Get(),&outputState});}
    UINT workHeight()const{return shape[11];}
    void setProcessingExtent(UINT width,UINT height){if(!accumulation||!width||!height||width>3840||height>2160)throw std::runtime_error("Invalid NR processing extent");shape[10]=width;shape[11]=height;}
    UINT tileCount()const{return ((shape[10]+shape[9]-1)/shape[9])*((shape[11]+shape[9]-1)/shape[9]);}
    void selectTile(UINT x,UINT y){if(!accumulation||x>=shape[10]||y>=shape[11])throw std::runtime_error("Invalid tile origin");shape[4]=x;shape[5]=y;}
    void clearTiles(){if(!accumulation)throw std::runtime_error("Not a tile adapter");accumulation->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);colour->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bind(clearShader.Get(),colour);}
    void accumulateTile(Tensor* rgb){if(!accumulation||!rgb||rgb==colour||rgb->elements!=colour->elements)throw std::runtime_error("Invalid tile result");rgb->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);colour->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);accumulation->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bind(accumulateShader.Get(),rgb);}
    void copyColourForTest(Tensor* rgb){colour->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);rgb->to(cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);cmd->CopyBufferRegion(rgb->resource.Get(),0,colour->resource.Get(),0,colour->elements*4);}
    // Caller must wait for the session fence before updating a live descriptor.
    bool compatibleInput(ID3D12Resource* resource,bool allowDeniedSrv=false)const{
        if(!resource)return false;auto desc=resource->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=shape[0]||desc.Height!=shape[1]||desc.Format!=format||desc.MipLevels!=1||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality||(!allowDeniedSrv&&bool(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))return false;
        ComPtr<ID3D12Device> owner;ComPtr<IUnknown> a,b;
        if(FAILED(resource->GetDevice(IID_PPV_ARGS(&owner)))||FAILED(owner.As(&a))||FAILED(device.As(&b)))return false;return a.Get()==b.Get();
    }
    void rebindInput(ID3D12Resource* resource){
        if(!compatibleInput(resource))throw std::runtime_error("Incompatible rotated NR output");
        if(input.Get()==resource)return;input=resource;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(input.Get(),&srv,heap->GetCPUDescriptorHandleForHeapStart());
    }
    void recordExternalInput(D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        if(!context->externalRecording)throw std::runtime_error("External texture pass requires caller recording scope");
        inputState=before;recordInput();to(input.Get(),inputState,after);
    }
    void recordExternalOutput(Tensor* rgb,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        if(!context->externalRecording)throw std::runtime_error("External texture pass requires caller recording scope");
        auto savedInputState=inputState;outputState=before;recordOutput(rgb);to(output.Get(),outputState,after);to(input.Get(),inputState,savedInputState);
    }
    void recordCopyBackToInput(D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        if(!context->externalRecording)throw std::runtime_error("Copy-back requires caller recording scope");
        inputState=before;to(input.Get(),inputState,D3D12_RESOURCE_STATE_COPY_DEST);
        to(output.Get(),outputState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(input.Get(),output.Get());to(input.Get(),inputState,after);
    }
    void upload(const std::string& file){
        if(context->externalRecording)throw std::runtime_error("Fixture upload is forbidden during external recording");
        if(format!=DXGI_FORMAT_R32G32B32A32_FLOAT)throw std::runtime_error("Use packed fixture upload for smaller formats");
        std::ifstream stream(nativePath(file),std::ios::binary|std::ios::ate);UINT64 count=UINT64(shape[0])*shape[1]*3;
        if(!stream||stream.tellg()!=std::streamoff(count*4))throw std::runtime_error("Invalid texture fixture");
        std::vector<float> rgb(static_cast<size_t>(count));stream.seekg(0);if(!stream.read(reinterpret_cast<char*>(rgb.data()),std::streamsize(count*4)))throw std::runtime_error("Texture fixture read failed");
        auto desc=input->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        auto staging=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped;D3D12_RANGE empty{0,0};check(staging->Map(0,&empty,&mapped),"Texture upload map");memset(mapped,0,static_cast<size_t>(bytes));
        for(UINT y=0;y<shape[1];++y){auto row=reinterpret_cast<float*>(static_cast<unsigned char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch);
            for(UINT x=0;x<shape[0];++x){for(UINT c=0;c<3;++c)row[x*4+c]=rgb[(UINT64(y)*shape[0]+x)*3+c];row[x*4+3]=1;}}
        staging->Unmap(0,&empty);to(input.Get(),inputState,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=input.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
    }
    void uploadPacked(const std::string& file){
        if(context->externalRecording)throw std::runtime_error("Fixture upload is forbidden during external recording");
        UINT64 rowBytes=UINT64(shape[0])*pixelBytes(),count=rowBytes*shape[1];
        std::ifstream stream(nativePath(file),std::ios::binary|std::ios::ate);
        if(!stream||stream.tellg()!=std::streamoff(count))throw std::runtime_error("Invalid packed texture fixture");
        std::vector<unsigned char> packed(static_cast<size_t>(count));stream.seekg(0);
        if(!stream.read(reinterpret_cast<char*>(packed.data()),std::streamsize(count)))throw std::runtime_error("Packed fixture read failed");
        auto desc=input->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        auto staging=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped;D3D12_RANGE empty{0,0};check(staging->Map(0,&empty,&mapped),"Packed texture upload map");memset(mapped,0,static_cast<size_t>(bytes));
        for(UINT y=0;y<shape[1];++y)memcpy(static_cast<unsigned char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch,packed.data()+UINT64(y)*rowBytes,static_cast<size_t>(rowBytes));
        staging->Unmap(0,&empty);to(input.Get(),inputState,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=input.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
    }
    Tensor* loadRgbForTest(const std::string& folder){auto tensor=load(folder,"colour",colour->elements);submit();uploads.clear();return tensor;}
    void recordInput(){to(input.Get(),inputState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        colour->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bind(readShader.Get(),colour);}
    void recordOutput(Tensor* rgb){if(!rgb||rgb->elements!=colour->elements)throw std::runtime_error("Texture output extent mismatch");
        if(shape[8]||shape[0]!=shape[2]||shape[1]!=shape[3]){to(input.Get(),inputState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);colour->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
        rgb->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);to(output.Get(),outputState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bind(writeShader.Get(),rgb);}
    std::vector<float> downloadTexture(const std::string& packedPath="",bool captureInput=false){
        if(context->externalRecording)throw std::runtime_error("Readback is forbidden during external recording");
        auto resource=captureInput?input.Get():output.Get();auto& state=captureInput?inputState:outputState;auto previous=state;
        auto desc=resource->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        auto rb=buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);to(resource,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=rb.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=resource;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);if(captureInput)to(resource,state,previous);submit();void* mapped;
        D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};check(rb->Map(0,&range,&mapped),"Texture readback map");
        std::vector<unsigned char> packed(size_t(shape[0])*shape[1]*pixelBytes());
        for(UINT y=0;y<shape[1];++y)memcpy(packed.data()+size_t(y)*shape[0]*pixelBytes(),static_cast<unsigned char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch,size_t(shape[0])*pixelBytes());
        if(!packedPath.empty()){std::ofstream file(packedPath,std::ios::binary);file.write(reinterpret_cast<const char*>(packed.data()),std::streamsize(packed.size()));if(!file)throw std::runtime_error("Packed texture write failed");}
        std::vector<float> rgba(size_t(shape[0])*shape[1]*4);
        for(size_t i=0;i<rgba.size();++i){
            if(pixelBytes()==16)memcpy(&rgba[i],packed.data()+i*4,4);
            else if(pixelBytes()==8){uint16_t value;memcpy(&value,packed.data()+i*2,2);rgba[i]=DirectX::PackedVector::XMConvertHalfToFloat(value);}
            else if(format==DXGI_FORMAT_R10G10B10A2_UNORM){uint32_t value;memcpy(&value,packed.data()+(i/4)*4,4);UINT channel=UINT(i%4);rgba[i]=channel==3?float(value>>30)/3.0f:float((value>>(channel*10))&1023)/1023.0f;}
            else rgba[i]=float(packed[i])/255.0f;
        }
        D3D12_RANGE empty{0,0};rb->Unmap(0,&empty);return rgba;
    }
};
