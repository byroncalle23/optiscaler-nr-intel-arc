#pragma once
#include "dml_runtime.h"
#include <array>

// Standalone diagnostics only. Readback is mapped after Present and GPU completion.
class NativeNrPresentBench : public NativeRuntime {
    ComPtr<IDXGISwapChain3> swapchain;
    std::array<ComPtr<ID3D12Resource>,2> backbuffers;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readbackBytes=0;
    UINT width=0,height=0;
public:
    std::vector<UINT> indices;
    std::vector<UINT> results;
    NativeNrPresentBench(std::shared_ptr<NativeContext> context,IDXGISwapChain1* chain):NativeRuntime(context){
        check(chain->QueryInterface(IID_PPV_ARGS(&swapchain)),"Presentation swapchain3");
        for(UINT i=0;i<2;++i)check(chain->GetBuffer(i,IID_PPV_ARGS(&backbuffers[i])),"Presentation backbuffer");
        auto desc=backbuffers[0]->GetDesc();width=UINT(desc.Width);height=desc.Height;
        if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM)throw std::runtime_error("Presentation bank requires RGBA8 or RGB10A2");
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&readbackBytes);
        readback=buffer(device.Get(),readbackBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void submitAndPresent(ID3D12Resource* output,const std::string& capture,bool captureAfterPresent=false){
        auto desc=output->GetDesc();
        if(desc.Format!=backbuffers[0]->GetDesc().Format||desc.Width!=width||desc.Height!=height)throw std::runtime_error("Presentation output incompatible");
        UINT index=swapchain->GetCurrentBackBufferIndex();if(index>=backbuffers.size())throw std::runtime_error("Presentation buffer index invalid");
        auto target=backbuffers[index].Get();indices.push_back(index);
        // Append to the SAME command list after NR has recorded its output.
        transition(cmd.Get(),output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd.Get(),target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=target;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=output;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        transition(cmd.Get(),output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        src.pResource=target;dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        transition(cmd.Get(),target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        check(cmd->Close(),"Presentation list close");ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
        // No explicit CPU fence wait or pixel read occurs before this real call.
        auto status=swapchain->Present(0,0);results.push_back(UINT(status));
        ComPtr<ID3D12CommandAllocator> afterAllocator;ComPtr<ID3D12GraphicsCommandList> afterList;
        if(captureAfterPresent){
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&afterAllocator)),"After-Present diagnostic allocator");
            check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,afterAllocator.Get(),nullptr,IID_PPV_ARGS(&afterList)),"After-Present diagnostic list");
            transition(afterList.Get(),target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
            afterList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            transition(afterList.Get(),target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
            check(afterList->Close(),"After-Present diagnostic close");ID3D12CommandList* after[]={afterList.Get()};queue->ExecuteCommandLists(1,after);
        }
        // Present can enqueue GPU work of its own. Include it in the fence that
        // protects backbuffer release/reacquisition during ResizeBuffers.
        check(queue->Signal(fence.Get(),++serial),"Post-Present completion signal");
        check(fence->SetEventOnCompletion(serial,event),"Presentation completion event");
        if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)ExitProcess(2);
        check(device->GetDeviceRemovedReason(),"Presentation device state");
        // Retain backbuffer owners through completion even when Present fails.
        if(status!=S_OK&&status!=DXGI_STATUS_OCCLUDED)throw std::runtime_error("Unexpected presentation result");
        if(diagnostics){
            auto count=diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
            for(UINT64 i=0;i<count;++i){SIZE_T bytes=0;check(diagnostics->GetMessage(i,nullptr,&bytes),"Presentation diagnostic size");
                std::vector<unsigned char> storage(bytes);auto message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                check(diagnostics->GetMessage(i,message,&bytes),"Presentation diagnostic message");
                if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)
                    throw std::runtime_error(std::string("Presentation D3D12 validation: ")+message->pDescription);
            }diagnostics->ClearStoredMessages();
        }
        check(allocator->Reset(),"Presentation allocator reset");check(cmd->Reset(allocator.Get(),nullptr),"Presentation list reset");
        void* mapped=nullptr;D3D12_RANGE range{0,SIZE_T(readbackBytes)};check(readback->Map(0,&range,&mapped),"Presentation diagnostic map");
        std::ofstream file(nativePath(capture),std::ios::binary);
        for(UINT y=0;y<height;++y)file.write(static_cast<const char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch,std::streamsize(width)*4);
        D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);if(!file)throw std::runtime_error("Presentation diagnostic write failed");
    }
};
