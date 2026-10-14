#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
class DenseWaveProbe : public BatchGraphProbe {
public:
    void recordLast(){recordGemm(*jobs.back());}
    UINT stride()const{return jobs.back()->native->stride;}
};
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: dense_tile_unit output");
    std::string folder=argv[1];std::filesystem::create_directories(nativePath(folder));
    {
        DenseWaveProbe probe;auto ctx=probe.sharedContext();
        D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT support{};
        support.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY;
        auto& op=support.ThreadVectorMatrixMultiply;
        op.VectorInputType=op.MatrixInputType=op.BiasInputType=op.VectorResultType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
        // Query the supported FP16 family, as in the existing grouped path.
        check(ctx->device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&support,sizeof(support)),"Thread FP16 query");
        printf("DENSE_THREAD_FP16_FLAGS %u\n",UINT(op.SupportFlags));
        if(!(op.SupportFlags&D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_SUPPORTED))throw std::runtime_error("Thread FP16 multiply unsupported");
    }
    UINT cases=0;
    struct DenseShape{UINT n,k,m;};
    for(auto shape:std::vector<DenseShape>{{64,64,33},{192,128,33},{256,256,33},{768,256,33},{1024,512,33},{4096,1024,33},{1024,4096,33},{64,64,254017}}){
        UINT n=shape.n,k=shape.k,m=shape.m;
        std::vector<float> reference;
        for(UINT tile:{32u,64u}){
            DenseWaveProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=2;ctx->linAlg=true;ctx->denseTileN=32;ctx->denseWavePolicy=tile==64?"wave64":"baseline";
            auto a=probe.make(UINT64(m)*k),b=probe.make(UINT64(k)*n,false),out=probe.make(UINT64(m)*n);b->immutableWeight=true;
            std::vector<float> input(size_t(m)*k*2),weights(size_t(k)*n);
            // Dyadic values keep each K64 partial exactly representable in FP16.
            // Independent double oracle checks transpose, row/lane strides and sum.
            for(size_t i=0;i<input.size();++i)input[i]=float(int((i*17+i/k*13+i/a->elements*7)%9)-4)/16;
            for(size_t i=0;i<weights.size();++i)weights[i]=float(int((i*11+i/n*5)%9)-4)/16;
            probe.uploadLanes(a,input);probe.uploadLanes(b,weights);probe.addGemm(a,b,out,m,n,k);probe.submit();
            auto run=[&](){probe.recordLast();probe.submit();return probe.downloadLanes(out);};
            std::vector<float> first;
            auto verify=[&](const std::vector<float>& actual,const std::vector<float>& source){
                for(UINT row=0;row<m*2;++row)for(UINT c=0;c<n;++c){double sum=0;
                    for(UINT inner=0;inner<k;++inner)sum+=double(source[size_t(row)*k+inner])*weights[size_t(inner)*n+c];
                    float v=actual[size_t(row)*n+c];if(!std::isfinite(v)||v!=float(sum))throw std::runtime_error("Dense tile double oracle mismatch");}
            };
            for(UINT repeat=0;repeat<3;++repeat){auto actual=run();if(!repeat)verify(actual,input);
                if(!repeat)first=actual;else if(memcmp(first.data(),actual.data(),actual.size()*4))throw std::runtime_error("Dense tile repeat mismatch");}
            if(tile==32)reference=first;else if(memcmp(first.data(),reference.data(),first.size()*4))throw std::runtime_error("Dense tile policy mismatch");
            auto changed=input;for(UINT i=0;i<k;++i)changed[size_t(m*2-1)*k+i]*=-1;
            probe.uploadLanes(a,changed);auto alt=run();verify(alt,changed);bool affected=false;
            for(size_t i=0;i<first.size();++i){if(i<size_t(m*2-1)*n&&alt[i]!=first[i])throw std::runtime_error("Dense tile row/lane contamination");if(alt[i]!=first[i])affected=true;}
            if(!affected)throw std::runtime_error("Dense tile input insensitivity");
            std::string name=folder+"/N"+std::to_string(n)+"-K"+std::to_string(k)+"-M"+std::to_string(m)+"-T"+std::to_string(tile)+".bin";
            std::ofstream file(nativePath(name),std::ios::binary);file.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!file)throw std::runtime_error("Dense unit output");
            printf("DENSE_WAVE_UNIT_PASS N=%u K=%u M=%u tile=%u stride=%u oracle_exact=1 repeat_exact=1 isolation=1\n",n,k,m,tile,probe.stride());++cases;
        }
    }
    printf("DENSE_WAVE_UNITS_PASS cases=%u\n",cases);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
