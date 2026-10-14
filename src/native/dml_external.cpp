#include "dml_texture.h"
int main(){try{
    TextureHarness caller(23,17);auto owner=caller.sharedContext();
    auto context=NativeContext::onDevice(owner->device.Get());NativeRuntime runtime(context);
    auto saved=context->cmd;UINT rejects=0;
    auto reject=[&](auto action,const char* reason){bool caught=false;
        try{action();}catch(const std::runtime_error& e){caught=std::string(e.what()).find(reason)!=std::string::npos;}
        if(!caught)throw std::runtime_error("External contract rejection failed");++rejects;};
    reject([&](){NativeContext::onDevice(nullptr);},"External device missing");
    reject([&](){ExternalCommandRecording scope(context,nullptr);},"Invalid or nested");
    reject([&](){TextureHarness bad(23,17,context,DXGI_FORMAT_R32G32B32A32_FLOAT,caller.inputTexture(),nullptr);},"Borrow distinct");
    reject([&](){TextureHarness bad(23,17,context,DXGI_FORMAT_R32G32B32A32_FLOAT,caller.inputTexture(),caller.inputTexture());},"Borrow distinct");
    reject([&](){TextureHarness bad(24,17,context,DXGI_FORMAT_R32G32B32A32_FLOAT,caller.inputTexture(),caller.outputTexture());},"geometry/format mismatch");
    TextureHarness adapter(23,17,context,DXGI_FORMAT_R32G32B32A32_FLOAT,caller.inputTexture(),caller.outputTexture());
    reject([&](){adapter.recordExternalInput(D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON);},"requires caller recording scope");
    ComPtr<ID3D12CommandAllocator> copyAllocator;ComPtr<ID3D12GraphicsCommandList> copy;
    check(owner->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&copyAllocator)),"Test copy allocator");
    check(owner->device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,copyAllocator.Get(),nullptr,IID_PPV_ARGS(&copy)),"Test copy list");
    reject([&](){ExternalCommandRecording scope(context,copy.Get());},"must support compute");
    {ExternalCommandRecording scope(context,owner->cmd.Get());
        if(context->cmd.Get()!=owner->cmd.Get())throw std::runtime_error("Caller list not adopted");
        reject([&](){runtime.submit();},"Submission is forbidden");
        reject([&](){runtime.download(adapter.colourTensor());},"Readback is forbidden");
        reject([&](){adapter.downloadTexture();},"Readback is forbidden");
        reject([&](){adapter.uploadPacked("not-read.bin");},"Fixture upload is forbidden");
        reject([&](){NativeRuntime bad(context);},"Initialization is forbidden");
        reject([&](){ExternalCommandRecording nested(context,owner->cmd.Get());},"Invalid or nested");}
    if(context->cmd.Get()!=saved.Get()||context->externalRecording||context->serial||owner->serial)throw std::runtime_error("Recording scope changed caller submission or failed restoration");
    printf("{\"passed\":true,\"rejections_verified\":%u,\"list_restored\":true,\"caller_submissions\":0}\n",rejects);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
