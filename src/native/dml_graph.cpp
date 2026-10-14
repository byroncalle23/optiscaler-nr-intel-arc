#include "dml_graph_pipeline.h"
#include "dml_endpoints.h"
#include "dml_window.h"
#include "dml_transition.h"
#include "dml_block.h"
#include "dml_frame.h"
#include "dml_texture.h"
#include "ngx_test_parameters.h"
#include <functional>
#include <map>

static void saveGraph(const std::string& path,const std::vector<float>& data){
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*4));
    if(!file)throw std::runtime_error("Cannot write graph result");
}
int main(int argc,char** argv){try{
    if(argc<2)throw std::runtime_error("Usage: dml_graph.exe fixture-directory [--private-scratch|--reuse-scratch] [--rgb-frame|--texture-frame] [--texture-format rgba32f|rgba16f|rgba8]");
    bool reuse=true,rgb=false,textures=false,external=false,opti=false;
    DXGI_FORMAT textureFormat=DXGI_FORMAT_R32G32B32A32_FLOAT;
    for(int i=2;i<argc;++i){std::string option(argv[i]);if(option=="--private-scratch")reuse=false;else if(option=="--reuse-scratch")reuse=true;else if(option=="--rgb-frame")rgb=true;else if(option=="--texture-frame"){rgb=true;textures=true;}
        else if(option=="--external-list"){external=true;rgb=true;textures=true;}
        else if(option=="--opti-post"){opti=true;external=true;rgb=true;textures=true;}
        else if(option=="--texture-format"&&i+1<argc){std::string name=argv[++i];textureFormat=name=="rgba32f"?DXGI_FORMAT_R32G32B32A32_FLOAT:name=="rgba16f"?DXGI_FORMAT_R16G16B16A16_FLOAT:name=="rgba8"?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_UNKNOWN;if(textureFormat==DXGI_FORMAT_UNKNOWN)throw std::runtime_error("Unsupported graph texture format");}
        else throw std::runtime_error("Unknown graph option");}
    if(textureFormat!=DXGI_FORMAT_R32G32B32A32_FLOAT&&!textures)throw std::runtime_error("Texture format requires --texture-frame");
    std::string folder(argv[1]);
    std::shared_ptr<NativeContext> context;
    Tensor* source=nullptr;
    std::unique_ptr<FrameHarness> frameInput,frameOutput;
    std::unique_ptr<TextureHarness> textureFrame;
    std::unique_ptr<TextureHarness> caller;
    if(textures){UINT h,w,nh,nw;std::ifstream config(folder+"/frame_input/shape.txt");
        if(!(config>>h>>w>>nh>>nw))throw std::runtime_error("Texture input geometry missing");
        textureFrame=std::make_unique<TextureHarness>(w,h,nullptr,textureFormat);context=textureFrame->sharedContext();
        if(textureFormat==DXGI_FORMAT_R32G32B32A32_FLOAT)textureFrame->upload(folder+"/frame_input/colour.bin");else textureFrame->uploadPacked(folder+"/frame_input/colour_texture.bin");
        if(external){caller=std::move(textureFrame);context=NativeContext::onDevice(caller->sharedContext()->device.Get());
            textureFrame=std::make_unique<TextureHarness>(w,h,context,textureFormat,caller->inputTexture(),caller->outputTexture());}}
    if(rgb){frameInput=std::make_unique<FrameHarness>(folder+"/frame_input",context,nullptr,nullptr,textures?textureFrame->colourTensor():nullptr);context=frameInput->sharedContext();
        frameInput->constrainToGpuBudget();context->reuseScratch=reuse;source=frameInput->resultTensor();}
    GraphPipeline graph(folder,context,source,reuse);context=graph.sharedContext();source=graph.resultTensor();auto& nodes=graph.publicNodes();
    if(rgb)frameOutput=std::make_unique<FrameHarness>(folder+"/frame_output",context,source,frameInput->colourTensor());
    std::vector<float> first,last;
    bool submitGuard=false;
    UINT postCalls=0;
    for(int run=0;run<2;++run){
        if(opti){if(textureFormat==DXGI_FORMAT_R32G32B32A32_FLOAT)caller->upload(folder+"/frame_input/colour.bin");else caller->uploadPacked(folder+"/frame_input/colour_texture.bin");}
        auto record=[&](){if(external)textureFrame->recordExternalInput(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST);else if(textures)textureFrame->recordInput();
            if(rgb)frameInput->record();for(auto& node:nodes)node->record();if(rgb)frameOutput->record();
            if(external)textureFrame->recordExternalOutput(frameOutput->resultTensor(),run==0?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            else if(textures)textureFrame->recordOutput(frameOutput->resultTensor());};
        if(external){{ExternalCommandRecording borrowed(context,caller->sharedContext()->cmd.Get());
                try{textureFrame->submit();}catch(const std::runtime_error& e){submitGuard=std::string(e.what())=="Submission is forbidden while recording on caller list";}
                if(!submitGuard)throw std::runtime_error("Caller list submission guard failed");
                if(opti){std::function<bool()> callback=[&](){record();textureFrame->recordCopyBackToInput(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST);++postCalls;return true;};
                    auto resource=caller->inputTexture();auto desc=resource->GetDesc();
                    NativeNrHook::Binding binding{17,UINT(desc.Width),desc.Height,desc.Format,resource,&callback,
                        [](void* user,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES){return (*static_cast<std::function<bool()>*>(user))();}};
                    FixtureNgxParameters params;params.output=resource;params.voidOnly=run!=0;NativeNrHook::Register(&binding);
                    auto list=caller->sharedContext()->cmd.Get();
                    if(NativeNrHook::Evaluate(18,list,&params,false,true,D3D12_RESOURCE_STATE_COPY_DEST)!=NativeNrHook::Result::Skipped||
                       NativeNrHook::Evaluate(17,list,&params,true,true,D3D12_RESOURCE_STATE_COPY_DEST)!=NativeNrHook::Result::Skipped||
                       NativeNrHook::Evaluate(17,list,&params,false,false,D3D12_RESOURCE_STATE_COPY_DEST)!=NativeNrHook::Result::Skipped)throw std::runtime_error("NGX preflight gate failed");
                    auto result=NativeNrHook::Evaluate(17,list,&params,false,true,D3D12_RESOURCE_STATE_COPY_DEST);NativeNrHook::Register(nullptr);
                    if(result!=NativeNrHook::Result::Recorded)throw std::runtime_error("NGX post-process recording failed");
                }else record();}
            caller->submit(); // Test caller owns close/execute/fence. Full inference is already recorded.
        }else record();
        last=textures?textureFrame->downloadTexture(folder+"/native_texture_packed.bin",opti):nodes.back()->runtime()->download(rgb?frameOutput->resultTensor():source);
        if(run==0)first=last;else if(memcmp(first.data(),last.data(),last.size()*4))throw std::runtime_error("Graph output is not repeatable");}
    for(float value:last)if(!std::isfinite(value))throw std::runtime_error("Non-finite graph output");
    if(opti&&postCalls!=2)throw std::runtime_error("Unexpected NGX callback count");
    if(textures){saveGraph(folder+"/native_texture.bin",last);last=frameOutput->download(frameOutput->resultTensor());}
    if(rgb){saveGraph(folder+"/native_rgb.bin",last);saveGraph(folder+"/native_features.bin",frameInput->download(frameInput->resultTensor()));
        saveGraph(folder+"/native.bin",nodes.back()->runtime()->download(source));}
    else saveGraph(folder+"/native.bin",last);
    UINT skips=0;
    // All diagnostic downloads follow the complete graph; no inference-stage reads.
    for(auto& node:nodes){saveGraph(folder+"/native_"+node->name+".bin",node->runtime()->download(node->output()));
        if(node->window && node->window->skipTensor()){
            ++skips;saveGraph(folder+"/native_skip_"+node->name+".bin",node->runtime()->download(node->window->skipTensor()));}}
    printf("{\"finite\":true,\"repeat_byte_identical\":true,\"runs\":2,\"nodes\":%zu,\"retained_skips\":%u,\"scratch_reused\":%s,\"tensor_scratch_reuses\":%llu,\"dml_scratch_reuses\":%llu,\"owned_default_buffer_bytes\":%llu,\"buffer_budget_bytes\":%llu,\"shared_device_and_list\":true,\"cpu_intermediate_reads\":0,\"rgb_pipeline\":%s,\"texture_pipeline\":%s,\"external_command_list\":%s,\"external_submit_guard\":%s,\"opti_postprocess\":%s,\"post_callbacks\":%u}\n",nodes.size(),skips,reuse ? "true" : "false",context->tensorScratchReuses,context->dmlScratchReuses,context->defaultBufferBytes,context->defaultBufferLimit,rgb?"true":"false",textures?"true":"false",external?"true":"false",submitGuard?"true":"false",opti?"true":"false",postCalls);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
