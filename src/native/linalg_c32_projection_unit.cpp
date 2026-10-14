#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
class C32ProjectionProbe : public BatchGraphProbe {
public:
    void recordLast(){recordGemm(*jobs.back());}
    bool isNative()const{return jobs.back()->native!=nullptr;}
    UINT stride()const{return jobs.back()->native?jobs.back()->native->stride:0;}
};
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: c32_projection_unit output");
    std::string folder=argv[1];std::filesystem::create_directories(nativePath(folder));
    {
        C32ProjectionProbe probe;auto ctx=probe.sharedContext();
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
    for(auto shape:std::vector<std::pair<UINT,UINT>>{{32,33},{96,33},{32,254016},{96,254016}}){
        UINT n=shape.first,k=32,m=shape.second;
        std::vector<float> reference;
        for(UINT policy:{0u,1u}){
            C32ProjectionProbe probe;auto ctx=probe.sharedContext();ctx->tileBatch=2;ctx->linAlg=true;ctx->c32ProjectionPolicy=policy?"both":"baseline";
            auto a=probe.make(UINT64(m)*k),b=probe.make(UINT64(k)*n,false),out=probe.make(UINT64(m)*n);b->immutableWeight=true;b->c32ProjectionRole=n==96?1:2;
            std::vector<float> input(size_t(m)*k*2),weights(size_t(k)*n);
            // Four signed dyadic nonzeros per column: all K32 partials exact in F16.
            for(size_t i=0;i<input.size();++i)input[i]=float(int((i*17+i/k*13+i/a->elements*7)%9)-4)/16;
            for(UINT c=0;c<n;++c)for(UINT slot=0;slot<4;++slot)weights[size_t((c*7+slot*5)%32)*n+c]=((c+slot)%3?-1.0f:1.0f)/16;
            std::vector<std::vector<std::pair<UINT,float>>> oracle(n);
            for(UINT c=0;c<n;++c)for(UINT inner=0;inner<k;++inner)if(weights[size_t(inner)*n+c]!=0)oracle[c].push_back({inner,weights[size_t(inner)*n+c]});
            probe.uploadLanes(a,input);probe.uploadLanes(b,weights);probe.addGemm(a,b,out,m,n,k);probe.submit();if(probe.isNative()!=(policy==1))throw std::runtime_error("C32 selected path mismatch");
            auto run=[&](){probe.recordLast();probe.submit();return probe.downloadLanes(out);};
            std::vector<float> first;
            auto verify=[&](const std::vector<float>& actual,const std::vector<float>& source){
                for(UINT row=0;row<m*2;++row)for(UINT c=0;c<n;++c){double sum=0;
                    for(auto item:oracle[c])sum+=double(source[size_t(row)*k+item.first])*item.second;
                    float v=actual[size_t(row)*n+c];if(!std::isfinite(v)||v!=float(sum))throw std::runtime_error("Dense tile double oracle mismatch");}
            };
            for(UINT repeat=0;repeat<3;++repeat){auto actual=run();if(!repeat)verify(actual,input);
                if(!repeat)first=actual;else if(memcmp(first.data(),actual.data(),actual.size()*4))throw std::runtime_error("Dense tile repeat mismatch");}
            if(policy==0)reference=first;else if(memcmp(first.data(),reference.data(),first.size()*4))throw std::runtime_error("Dense tile policy mismatch");
            auto changed=input;for(UINT i=0;i<k;++i)changed[size_t(m*2-1)*k+i]*=-1;
            probe.uploadLanes(a,changed);auto alt=run();verify(alt,changed);bool affected=false;
            for(size_t i=0;i<first.size();++i){if(i<size_t(m*2-1)*n&&alt[i]!=first[i])throw std::runtime_error("Dense tile row/lane contamination");if(alt[i]!=first[i])affected=true;}
            if(!affected)throw std::runtime_error("Dense tile input insensitivity");
            std::string name=folder+"/N"+std::to_string(n)+"-M"+std::to_string(m)+"-P"+std::to_string(policy)+".bin";
            std::ofstream file(nativePath(name),std::ios::binary);file.write(reinterpret_cast<const char*>(first.data()),first.size()*4);if(!file)throw std::runtime_error("Dense unit output");
            printf("C32_PROJECTION_UNIT_PASS N=%u M=%u policy=%u stride=%u oracle_exact=1 repeat_exact=1 isolation=1\n",n,m,policy,probe.stride());++cases;
        }
    }
    printf("C32_PROJECTION_UNITS_PASS cases=%u\n",cases);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
