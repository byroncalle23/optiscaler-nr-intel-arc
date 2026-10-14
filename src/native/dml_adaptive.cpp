#include "nr_session.h"
#include <filesystem>
int main(int argc,char** argv){try{
    std::set_terminate([](){fprintf(stderr,"Adaptive benchmark ownership contract terminated\n");std::abort();});
    if(argc!=3)throw std::runtime_error("Usage: dml_adaptive.exe assets-folder 1440p-fixture-folder");
    std::string fixture=argv[2];
    TextureHarness caller(2560,1440,nullptr,DXGI_FORMAT_R10G10B10A2_UNORM,nullptr,nullptr,320,320);
    auto context=caller.sharedContext();
    auto session=std::make_shared<NativeNrSession>(caller.inputTexture(),argv[1],true,true,true);
    fprintf(stderr,"Adaptive model initialized\n");
    auto bytes=session->allocatedBytes();std::vector<float> first;
    const UINT widths[]={1920,2560,1920},heights[]={1080,1440,1080},passes[]={45,84,45};
    for(UINT i=0;i<3;++i){
        fprintf(stderr,"Adaptive step %u upload\n",i);
        caller.uploadPacked(fixture+"/colour_texture.bin");
        session->setProcessingExtent(widths[i],heights[i]);
        fprintf(stderr,"Adaptive step %u record\n",i);
        if(session->passesPerFrame()!=passes[i])throw std::runtime_error("Adaptive tile count mismatch");
        // Match production Present. The fixture adapter retains COPY_DEST state,
        // so restore it after recording before its next upload/readback.
        transition(context->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        if(!session->record(context->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_PRESENT)){
            session->cancelUnsubmitted();throw std::runtime_error("Adaptive record failed");}
        transition(context->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        fprintf(stderr,"Adaptive step %u recorded\n",i);
        bool rejected=false;try{session->setProcessingExtent(1280,720);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Changed resolution before caller completion");
        fprintf(stderr,"Adaptive step %u notify\n",i);
        session->notifySubmitted(context->fence.Get(),context->serial+1);
        fprintf(stderr,"Adaptive step %u submit\n",i);
        try{caller.submit();}catch(const std::exception& e){fprintf(stderr,"Adaptive submit diagnostic: %s\n",e.what());
            if(context->diagnostics)for(UINT64 n=0;n<context->diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();++n){SIZE_T size=0;context->diagnostics->GetMessage(n,nullptr,&size);std::vector<unsigned char> data(size);auto message=reinterpret_cast<D3D12_MESSAGE*>(data.data());context->diagnostics->GetMessage(n,message,&size);fprintf(stderr,"D3D12: %s\n",message->pDescription);}
            ExitProcess(1);}
        fprintf(stderr,"Adaptive step %u completed\n",i);
        if(!session->ready())throw std::runtime_error("Adaptive session did not become ready");
        auto actual=caller.downloadTexture(fixture+"/adaptive"+std::to_string(i)+".bin",true);
        for(float value:actual)if(!std::isfinite(value))throw std::runtime_error("Nonfinite adaptive output");
        if(i==0)first=actual;
        if(i==2&&actual!=first)throw std::runtime_error("Returning to 1080p did not repeat output");
        if(session->allocatedBytes()!=bytes)throw std::runtime_error("Resolution change allocated model buffers");
    }
    if(session->graphPasses()!=174||session->recordedFrames()!=3)throw std::runtime_error("Actual graph dispatch count differs from adaptive schedule");
    session->requestRetire();if(!session->retired())throw std::runtime_error("Adaptive retirement pending");session.reset();
    puts("{\"passed\":true,\"physical_extent\":\"2560x1440\",\"processing_extents\":[\"1920x1080\",\"2560x1440\",\"1920x1080\"],\"tile_passes\":[45,84,45],\"model_preparations\":1,\"resolution_change_before_completion_rejected\":true,\"return_to_1080p_byte_identical\":true,\"hot_model_allocations\":0,\"game_test\":false}");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
