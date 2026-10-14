#pragma once
#include "dml_runtime.h"
#include <filesystem>
#include <fstream>

// Opt-in one-frame diagnostics. GPU copies share the model list; the writer
// maps immutable readbacks only after the caller's completion fence.
class NativeNrPairCapture {
    ComPtr<ID3D12Resource> before,after,stabilityReadback,motionReadback;
    UINT64 stabilityBytes=0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT width,height,lowWidth=640,lowHeight=360;
    DXGI_FORMAT format;
    UINT64 bytes=0;
    void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* target,ID3D12Resource* readback){
        transition(list,target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        transition(list,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
    }
    void write(ID3D12Resource* resource,const std::filesystem::path& path){
        void* data=nullptr;D3D12_RANGE range{0,SIZE_T(bytes)};check(resource->Map(0,&range,&data),"Diagnostic pair map");
        std::ofstream file(path,std::ios::binary);
        for(UINT y=0;y<height;++y)file.write(static_cast<char*>(data)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch,std::streamsize(width)*4);
        D3D12_RANGE empty{0,0};resource->Unmap(0,&empty);if(!file)throw std::runtime_error("Diagnostic pair write");
    }
public:
    NativeNrPairCapture(ID3D12Device* device,ID3D12Resource* target,UINT lowW=640,UINT lowH=360):lowWidth(lowW),lowHeight(lowH){
        if(!lowW||!lowH||lowW>1920||lowH>1152)throw std::runtime_error("Invalid diagnostic low geometry");
        auto desc=target->GetDesc();width=UINT(desc.Width);height=desc.Height;format=desc.Format;
        if(format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_R10G10B10A2_UNORM)throw std::runtime_error("Unsupported pair capture format");
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        before=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        after=buffer(device,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void recordBefore(ID3D12GraphicsCommandList* list,ID3D12Resource* target){copy(list,target,before.Get());}
    void recordAfter(ID3D12GraphicsCommandList* list,ID3D12Resource* target,Tensor* trace=nullptr,Tensor* motion=nullptr,Tensor* confidence=nullptr){
        copy(list,target,after.Get());
        if(trace){if(trace->elements!=UINT64(lowWidth)*lowHeight*4)throw std::runtime_error("Invalid stability diagnostic tensor");
            ComPtr<ID3D12Device> device;check(target->GetDevice(IID_PPV_ARGS(&device)),"Diagnostic device");stabilityBytes=trace->elements*4;
            stabilityReadback=buffer(device.Get(),stabilityBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            trace->to(list,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(stabilityReadback.Get(),0,trace->resource.Get(),0,stabilityBytes);
        }
        if(motion&&confidence){
            if(motion->elements!=UINT64(lowWidth)*lowHeight*2||confidence->elements!=UINT64(lowWidth)*lowHeight)throw std::runtime_error("Invalid motion diagnostic tensors");
            ComPtr<ID3D12Device> device;check(target->GetDevice(IID_PPV_ARGS(&device)),"Motion diagnostic device");
            motionReadback=buffer(device.Get(),UINT64(lowWidth)*lowHeight*3*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            motion->to(list,D3D12_RESOURCE_STATE_COPY_SOURCE);confidence->to(list,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(motionReadback.Get(),0,motion->resource.Get(),0,motion->elements*4);
            list->CopyBufferRegion(motionReadback.Get(),motion->elements*4,confidence->resource.Get(),0,confidence->elements*4);
        }
    }
    void save(const std::filesystem::path& folder,UINT graphPasses,UINT64 presentFrame=0,UINT64 producerFrame=0,bool temporal=false){
        std::filesystem::create_directories(folder);write(before.Get(),folder/L"before.bin");write(after.Get(),folder/L"after.bin");
        std::ofstream meta(folder/L"pair.json");meta<<"{\"same_frame\":true,\"width\":"<<width<<",\"height\":"<<height<<",\"format_id\":"<<UINT(format)<<",\"graph_passes\":"<<graphPasses<<",\"present_frame\":"<<presentFrame<<",\"producer_frame\":"<<producerFrame<<",\"temporal_applied\":"<<(temporal?"true":"false")<<",\"diagnostic_readback_after_fence\":true}";
        if(stabilityReadback){
            void* data;D3D12_RANGE range{0,SIZE_T(stabilityBytes)};check(stabilityReadback->Map(0,&range,&data),"Stability map");
            std::ofstream traceFile(folder/L"stability.bin",std::ios::binary);traceFile.write(static_cast<char*>(data),std::streamsize(stabilityBytes));D3D12_RANGE empty{0,0};stabilityReadback->Unmap(0,&empty);if(!traceFile)throw std::runtime_error("Stability write");
            std::ofstream traceMeta(folder/L"stability.json");traceMeta<<"{\"width\":"<<lowWidth<<",\"height\":"<<lowHeight<<",\"channels\":[\"current_weight\",\"legacy_weight\",\"residual_delta\",\"base_delta\"],\"same_frame\":true}";if(!traceMeta)throw std::runtime_error("Stability metadata");
        }
        if(motionReadback){
            const SIZE_T size=UINT64(lowWidth)*lowHeight*3*4;void* data;D3D12_RANGE range{0,size};check(motionReadback->Map(0,&range,&data),"Motion diagnostic map");
            std::ofstream file(folder/L"motion-confidence.bin",std::ios::binary);file.write(static_cast<char*>(data),std::streamsize(size));D3D12_RANGE empty{0,0};motionReadback->Unmap(0,&empty);if(!file)throw std::runtime_error("Motion diagnostic write");
            std::ofstream layout(folder/L"motion-confidence.json");layout<<"{\"width\":"<<lowWidth<<",\"height\":"<<lowHeight<<",\"layout\":\"motion_xy_interleaved_then_confidence\",\"motion_convention\":\"current_to_previous_uv\",\"same_frame\":true}";if(!layout)throw std::runtime_error("Motion diagnostic metadata");
        }
        if(!meta)throw std::runtime_error("Pair metadata write");
    }
};
