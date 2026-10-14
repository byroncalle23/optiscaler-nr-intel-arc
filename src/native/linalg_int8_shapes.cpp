#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
class Int8Shapes:public BatchGraphProbe{
public:
    void run(const std::string& folder,UINT m,UINT n,UINT k,const std::string& dataFolder=""){
        if(m%8||n%16||k%32||m/8>65535||n/16>65535||UINT64(k)*127*127>INT_MAX)
            throw std::runtime_error("Unsupported aligned shape / integer overflow bound");
        D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT caps{};
        caps.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
        caps.WaveMatrixMultiply.Inputs={16,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32};
        check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&caps,sizeof(caps)),"INT8 capability");
        if(!(UINT(caps.WaveMatrixMultiply.SupportFlags)&1)||(UINT(caps.WaveMatrixMultiply.SupportFlags)&6)||!caps.WaveMatrixMultiply.NumShapes||caps.WaveMatrixMultiply.NumShapes>256)throw std::runtime_error("Native INT8 unavailable");
        std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(caps.WaveMatrixMultiply.NumShapes);caps.WaveMatrixMultiply.Shapes=shapes.data();
        check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&caps,sizeof(caps)),"INT8 shapes");
        if(!std::any_of(shapes.begin(),shapes.end(),[](auto s){return s.M==8&&s.K==32&&s.N==16;}))throw std::runtime_error("INT8 shape missing");
        auto a=make(UINT64(m)*k,false),b=make(UINT64(k)*n,false),baseline=make(UINT64(m)*n,false);
        auto packedA=make(UINT64(m)*k/4,false),packedB=make(UINT64(k)*n/4+n,false);
        auto scratch=make(UINT64(m)*n,false),fused=make(UINT64(m)*n,false),staged=make(UINT64(m)*n,false);
        std::vector<float> av(size_t(a->elements)),bv(size_t(b->elements));std::vector<int8_t> bp(size_t(k)*n+n*4);
        std::vector<float> weightScales(n,1.f/64);float activationScale=1.f/32;
        for(UINT64 i=0;i<a->elements;++i)av[size_t(i)]=float(int((i*3+7)%17)-8)/32;
        for(UINT col=0;col<n;++col)for(UINT inner=0;inner<k;++inner){
            UINT64 logical=UINT64(inner)*n+col;int q=int((logical*5+11)%17)-8;bv[size_t(logical)]=float(q)/64;
            UINT64 offset=(UINT64(col/16)*(k/32)+inner/32)*512+(inner%32)*16+col%16;bp[size_t(offset)]=int8_t(q);
        }
        if(!dataFolder.empty()){
            auto read=[&](const char* file,void* data,size_t bytes){std::ifstream f(nativePath(dataFolder+"/"+file),std::ios::binary|std::ios::ate);
                if(!f||f.tellg()!=std::streamoff(bytes))throw std::runtime_error(std::string("Real layer size: ")+file);f.seekg(0);f.read(static_cast<char*>(data),bytes);if(!f)throw std::runtime_error("Real layer read");};
            read("a_float.bin",av.data(),av.size()*4);read("w_float.bin",bv.data(),bv.size()*4);
            read("packed_b.bin",bp.data(),bp.size());read("activation_scale.bin",&activationScale,4);
            if(!std::isfinite(activationScale)||activationScale<=0)throw std::runtime_error("Invalid activation scale");
            memcpy(weightScales.data(),bp.data()+size_t(k)*n,n*4);
            for(float scale:weightScales)if(!std::isfinite(scale)||scale<=0)throw std::runtime_error("Invalid weight scale");
            if(!std::all_of(av.begin(),av.end(),[](float v){return std::isfinite(v);})||!std::all_of(bv.begin(),bv.end(),[](float v){return std::isfinite(v);}))throw std::runtime_error("Nonfinite real operands");
        }else memcpy(bp.data()+size_t(k)*n,weightScales.data(),n*4);
        uploadLanes(a,av);uploadLanes(b,bv);
        std::vector<float> raw(size_t(packedB->elements));memcpy(raw.data(),bp.data(),bp.size());uploadLanes(packedB,raw);
        // Match the current global graph's operator selection on these exact dimensions.
        context->linAlg=true;b->immutableWeight=true;addGemm(a,b,baseline,m,n,k);submit();
        printf("BASELINE name=%s\n",jobs.back()->profileName.c_str());
        D3D12_ROOT_PARAMETER p[5]{};
        for(UINT i=0;i<4;++i){p[i].ParameterType=i<2?D3D12_ROOT_PARAMETER_TYPE_SRV:D3D12_ROOT_PARAMETER_TYPE_UAV;p[i].Descriptor.ShaderRegister=i%2;}
        p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[4].Constants.Num32BitValues=6;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=5;rd.pParameters=p;ComPtr<ID3DBlob> blob,error;
        check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"INT8 shape root serialize");
        ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"INT8 shape root");
        std::map<std::string,ComPtr<ID3D12PipelineState>> psos;
        for(auto name:{"Quantize","Product","Publish"})psos[name]=linAlgPso(device.Get(),root.Get(),(std::string("shape-")+name+".dxil").c_str());
        auto bind=[&](const char* name,Tensor* input,Tensor* temp,Tensor* output,bool merge,UINT x,UINT y){
            input->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);packedB->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            temp->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);output->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(psos.at(name).Get());
            cmd->SetComputeRootShaderResourceView(0,input->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,packedB->resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(2,temp->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(3,output->resource->GetGPUVirtualAddress());
            UINT constants[]={m,n,k,UINT(merge),0,0};float inv=1.f/activationScale;memcpy(constants+4,&inv,4);memcpy(constants+5,&activationScale,4);
            cmd->SetComputeRoot32BitConstants(4,6,constants,0);cmd->Dispatch(x,y,1);uavBarrier(cmd.Get());
        };
        auto profile=std::make_shared<NativeGpuProfile>(device.Get(),64);
        auto baselinePass=[&](){NativeProfileRange t(profile.get(),cmd.Get(),"baseline");recordGemm(*jobs.back());};
        auto intPass=[&](bool merge){NativeProfileRange total(profile.get(),cmd.Get(),merge?"int8_fused":"int8_staged");
            {NativeProfileRange t(profile.get(),cmd.Get(),"quantize");UINT groups=UINT((UINT64(m)*k/4+63)/64);bind("Quantize",a,packedA,merge?fused:staged,merge,std::min(groups,4096u),(groups+4095)/4096);}
            {NativeProfileRange t(profile.get(),cmd.Get(),"product");bind("Product",packedA,scratch,merge?fused:staged,merge,m/8,n/16);}
            if(!merge){NativeProfileRange t(profile.get(),cmd.Get(),"publish");bind("Publish",packedA,scratch,staged,false,m/8,n/16);}
        };
        std::vector<float> first;
        for(UINT r=0;r<10;++r){profile->reset();
            if(r%2){intPass(false);if(dataFolder.empty())intPass(true);baselinePass();}else{baselinePass();if(dataFolder.empty())intPass(true);intPass(false);}
            profile->resolve(cmd.Get());submit();for(auto& t:profile->readAfterFence(queue.Get()))printf("TIME run=%u name=%s ms=%.9f\n",r,t.first.c_str(),t.second.ms);
            auto result=downloadLanes(dataFolder.empty()?fused:staged);
            if(dataFolder.empty()){auto other=downloadLanes(staged);if(memcmp(result.data(),other.data(),result.size()*4))throw std::runtime_error("Fused/staged output mismatch");}
            if(r&&memcmp(first.data(),result.data(),result.size()*4))throw std::runtime_error("INT8 repetition mismatch");if(!r)first=std::move(result);
        }
        auto save=[&](const char* name,Tensor* tensor){auto values=downloadLanes(tensor);std::ofstream f(nativePath(folder+"/"+name+".bin"),std::ios::binary);f.write(reinterpret_cast<const char*>(values.data()),values.size()*4);if(!f)throw std::runtime_error("Shape output write");};
        save("int8",dataFolder.empty()?fused:staged);save("integer",scratch);save("baseline",baseline);save("packed_a",packedA);
        printf("INT8_SHAPE_PASS M=%u N=%u K=%u repeat_exact=1 fused_staged_exact=%u\n",m,n,k,dataFolder.empty()?1u:0u);
    }
};
int main(int argc,char** argv){try{if(argc!=5&&argc!=6)throw std::runtime_error("Usage: int8_shapes output M N K [realDataFolder]");std::filesystem::create_directories(nativePath(argv[1]));Int8Shapes p;p.run(argv[1],std::stoul(argv[2]),std::stoul(argv[3]),std::stoul(argv[4]),argc==6?argv[5]:"");return 0;}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
