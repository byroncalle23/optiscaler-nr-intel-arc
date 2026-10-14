// Reuse the standalone upload/readback helpers; never compiled into OptiScaler.
#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
class GroupedProbe : public BatchGraphProbe {
public:
    void run(const std::string& folder,UINT lanes,UINT groups,UINT m,UINT n,UINT k,bool native){
        context->tileBatch=lanes;context->linAlg=native;context->linAlgGrouped=native;
        auto a=make(UINT64(groups)*m*k),w=make(UINT64(groups)*k*n,false),o=make(UINT64(groups)*m*n);
        std::vector<float> input(size_t(a->elements*lanes)),weights(size_t(w->elements));
        for(UINT lane=0;lane<lanes;++lane)for(UINT g=0;g<groups;++g)for(UINT i=0;i<m*k;++i)
            input[(size_t(lane)*groups+g)*m*k+i]=float(int((i*17+lane*31+g*43)%127)-63)/64;
        for(UINT g=0;g<groups;++g)for(UINT i=0;i<k*n;++i)weights[size_t(g)*k*n+i]=float(int((i*13+g*29)%61)-30)/128;
        uploadLanes(a,input);uploadLanes(w,weights);w->immutableWeight=true;
        addGemm(a,w,o,m,n,k,groups);submit();
        std::vector<float> first;
        for(UINT run=0;run<3;++run){recordGemm(*jobs.back());submit();auto result=downloadLanes(o);
            if(!run)first=result;else if(result!=first)throw std::runtime_error("Grouped repeat mismatch");}
        auto job=jobs.back().get();
        if(native){
            auto& p=*job->native;auto rb=buffer(device.Get(),UINT64(groups)*n*k*2,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            transition(cmd.Get(),p.packed.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(rb.Get(),0,p.packed.Get(),0,UINT64(groups)*n*k*2);
            transition(cmd.Get(),p.packed.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);submit();
            void* mapped;D3D12_RANGE range{0,size_t(groups)*n*k*2};check(rb->Map(0,&range,&mapped),"Group pack readback");
            std::ofstream f(folder+"/packed.bin",std::ios::binary);f.write(static_cast<char*>(mapped),range.End);D3D12_RANGE empty{};rb->Unmap(0,&empty);
        }
        for(auto entry:std::vector<std::pair<std::string,const std::vector<float>*>>{{"input",&input},{"weights",&weights},{"output",&first}}){
            std::ofstream f(folder+"/"+entry.first+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(entry.second->data()),entry.second->size()*4);if(!f)throw std::runtime_error("Group output write");}
        // Perturb lane3; every other lane must remain bit-identical.
        if(lanes==4){for(size_t i=3*a->elements;i<input.size();++i)input[i]+=0.125f;uploadLanes(a,input);recordGemm(*job);submit();auto alt=downloadLanes(o);
            if(!std::equal(first.begin(),first.begin()+3*o->elements,alt.begin()))throw std::runtime_error("Group lane contamination");
            if(std::equal(first.begin()+3*o->elements,first.end(),alt.begin()+3*o->elements))throw std::runtime_error("Group lane3 insensitive");}
        printf("GROUPED_PASS B=%u G=%u M=%u N=%u K=%u native=%u jobs=%llu weight_bytes=%llu\n",lanes,groups,m,n,k,UINT(native),context->linAlgGroupedJobs,context->linAlgWeightBytes);
    }
};
int main(int argc,char** argv){try{
    if(argc!=8)throw std::runtime_error("Usage: grouped_probe output B G M N K native");
    std::filesystem::create_directories(argv[1]);GroupedProbe probe;
    probe.run(argv[1],std::stoul(argv[2]),std::stoul(argv[3]),std::stoul(argv[4]),std::stoul(argv[5]),std::stoul(argv[6]),std::stoul(argv[7])!=0);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
