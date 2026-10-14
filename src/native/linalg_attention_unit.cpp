#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
int main(int argc,char** argv){try{
    if(argc!=6)throw std::runtime_error("Usage: attention_unit assets output block<number>|global wave B");
    std::string assets=argv[1],folder=argv[2],kind=argv[3];UINT waveMode=std::stoul(argv[4]);bool wave=waveMode!=0;UINT lanes=std::stoul(argv[5]);
    if(lanes!=2&&lanes!=4)throw std::runtime_error("Unit B must be2/4");
    std::filesystem::create_directories(folder);BatchGraphProbe probe;auto context=probe.sharedContext();context->tileBatch=lanes;context->reuseScratch=true;context->linAlgAttention=wave;
    context->linAlgAttentionInline=waveMode==2;
    context->linAlgWindowFused=waveMode==3||waveMode==4;
    context->linAlgWindowNormalizeFused=waveMode==4;
    std::unique_ptr<WindowHarness> window;std::unique_ptr<BlockHarness> global;
    Tensor *qkv,*result,*scores,*probabilities,*packed,*attended;
    if(kind=="global"){
        auto input=probe.make(144*1024);global=std::make_unique<BlockHarness>(assets+"/bottleneck/block31",false,context,input);
        qkv=global->attentionUnitInput();result=global->attentionUnitResult();scores=global->attentionUnitScores();probabilities=global->attentionUnitProbabilities();
        packed=global->attentionUnitPacked();attended=global->attentionUnitAttended();
    }else{
        if(kind.size()<=5||kind.substr(0,5)!="block"||kind.find_first_not_of("0123456789",5)!=std::string::npos)throw std::runtime_error("Invalid window unit name");
        UINT shape[9]{};std::ifstream config(nativePath(assets+"/"+kind+"/shape.txt"));
        for(auto& value:shape)if(!(config>>value))throw std::runtime_error("Missing unit shape");
        UINT h=shape[0],w=shape[1],channels=shape[8];auto input=probe.make(UINT64(h)*w*channels);
        printf("UNIT_SHAPE channels=%u window_heads=%u B=%u\n",channels,shape[4]*shape[5]*(channels/32)/64,lanes);
        window=std::make_unique<WindowHarness>(assets+"/"+kind,context,input);
        qkv=window->attentionUnitInput();result=window->attentionUnitResult();scores=window->attentionUnitScores();probabilities=window->attentionUnitProbabilities();
        packed=window->attentionUnitPacked();attended=window->attentionUnitAttended();
    }
    std::vector<float> input(size_t(qkv->elements*lanes));
    for(UINT lane=0;lane<lanes;++lane)for(UINT64 i=0;i<qkv->elements;++i)input[size_t(lane*qkv->elements+i)]=float(int((i*17+(i/32)*31+lane*73)%257)-128)/64;
    probe.uploadLanes(qkv,input);context->profiler=std::make_shared<NativeGpuProfile>(context->device.Get(),128);
    auto record=[&](){NativeProfileRange r(context->profiler.get(),context->cmd.Get(),"attention_unit");if(window)window->recordAttentionUnit();else global->recordAttentionUnit();};
    std::vector<float> first;
    for(UINT run=0;run<8;++run){context->profiler->reset();record();context->profiler->resolve(context->cmd.Get());probe.submit();
        auto times=context->profiler->readAfterFence(context->queue.Get());for(auto& x:times)printf("TIME run=%u name=%s ms=%.9f\n",run,x.first.c_str(),x.second.ms);
        auto output=probe.downloadLanes(result);if(!run)first=output;else if(output!=first)throw std::runtime_error("Attention repeat failed");
    }
    std::vector<std::pair<std::string,Tensor*>> captures{{"output",result}};
    if((!window||!context->linAlgWindowNormalizeFused)&&GetEnvironmentVariableA("NR_UNIT_OUTPUT_ONLY",nullptr,0)==0)captures.push_back({"packed",packed});
    if(!window||!context->linAlgWindowFused){captures.push_back({"scores",scores});captures.push_back({"probabilities",probabilities});captures.push_back({"attended",attended});}
    for(auto entry:captures){auto output=probe.downloadLanes(entry.second);std::ofstream f(folder+"/"+entry.first+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(output.data()),output.size()*4);}
    if(lanes==4){for(size_t i=3*qkv->elements;i<input.size();++i)input[i]*=-0.75f;probe.uploadLanes(qkv,input);context->profiler->reset();record();probe.submit();auto alt=probe.downloadLanes(result);
        if(!std::equal(first.begin(),first.begin()+3*result->elements,alt.begin()))throw std::runtime_error("Attention lane contamination");
        if(std::equal(first.begin()+3*result->elements,first.end(),alt.begin()+3*result->elements))throw std::runtime_error("Attention lane3 insensitive");}
    printf("ATTENTION_UNIT_PASS kind=%s B=%u wave=%u wave_jobs=%llu repeat_exact=1\n",kind.c_str(),lanes,UINT(wave),context->linAlgAttentionJobs);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
