#include "nr_session.h"
#include <chrono>
#include <iomanip>
int main(int argc,char** argv){try{
    if(argc!=5&&argc!=6)throw std::runtime_error("Usage: dml_profile.exe assets-folder fixture-folder processing-width processing-height [--coarse-only]");
    const bool coarseOnly=argc==6&&std::string(argv[5])=="--coarse-only";
    if(argc==6&&!coarseOnly)throw std::runtime_error("Unknown profiling option");
    std::string folder=argv[2];UINT width=UINT(std::stoul(argv[3])),height=UINT(std::stoul(argv[4]));
    TextureHarness caller(2560,1440,nullptr,DXGI_FORMAT_R10G10B10A2_UNORM,nullptr,nullptr,320,320);
    auto ctx=caller.sharedContext();
    auto session=std::make_shared<NativeNrSession>(caller.inputTexture(),argv[1],true,true,true);
    auto profile=std::make_shared<NativeGpuProfile>(ctx->device.Get());
    session->setProcessingExtent(width,height);
    fprintf(stderr,"FP16 GEMM operators: %llu\n",static_cast<unsigned long long>(session->captureGraph().sharedContext()->tensorHalfGemmJobs));
    fprintf(stderr,"Prepacked FP16 weights: %llu\n",static_cast<unsigned long long>(session->captureGraph().sharedContext()->prepackedHalfWeights));
    std::ofstream csv(nativePath(folder+"/profile_ranges.csv"));csv<<"run,path,count,gpu_ms\n"<<std::setprecision(10);
    std::vector<float> baseline;
    double gpu[4]{},cpu[4]{},wall[4]{};
    for(UINT run=0;run<(coarseOnly?3u:4u);++run){
        caller.uploadPacked(folder+"/colour_texture.bin");profile->reset();
        session->captureGraph().sharedContext()->profiler=run==3?profile:nullptr;
        auto start=std::chrono::steady_clock::now();
        {
            NativeProfileRange frame(profile.get(),ctx->cmd.Get(),"frame");
            transition(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
            if(!session->record(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_PRESENT)){session->cancelUnsubmitted();throw std::runtime_error("Profile record failed");}
            transition(ctx->cmd.Get(),caller.inputTexture(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        }
        cpu[run]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        profile->resolve(ctx->cmd.Get());session->notifySubmitted(ctx->fence.Get(),ctx->serial+1);
        caller.submit();wall[run]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(!session->ready())throw std::runtime_error("Profile completion missing");
        auto rows=profile->readAfterFence(ctx->queue.Get());gpu[run]=rows.at("frame").ms;
        for(const auto& row:rows)csv<<run<<','<<row.first<<','<<row.second.count<<','<<row.second.ms<<'\n';
        auto output=caller.downloadTexture(folder+"/profile_frame"+std::to_string(run)+".bin",true);
        if(run==1)baseline=output;
        if(run>1&&output!=baseline)throw std::runtime_error("Profiling changed the output image");
        fprintf(stderr,"profile run%u GPU %.3fms CPU record %.3fms wall %.3fms\n",run,gpu[run],cpu[run],wall[run]);
    }
    session->captureGraph().sharedContext()->profiler.reset();session->requestRetire();session.reset();
    printf("{\"passed\":true,\"processing_width\":%u,\"processing_height\":%u,\"tile_passes\":%u,\"warm_gpu_ms\":[%.6f,%.6f],\"warm_cpu_record_ms\":[%.6f,%.6f],\"warm_record_submit_wait_ms\":[%.6f,%.6f],\"detailed_gpu_ms\":%.6f,\"detailed_cpu_record_ms\":%.6f,\"profiling_output_byte_identical\":true,\"timings_exclude_initialization_upload_and_image_readback\":true,\"game_test\":false}\n",width,height,((width+223)/224)*((height+223)/224),gpu[1],gpu[2],cpu[1],cpu[2],wall[1],wall[2],gpu[3],cpu[3]);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
