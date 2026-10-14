#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
class Int8Unit : public BatchGraphProbe {
public:
    void run(const std::string& folder,UINT products){
        if(products!=12&&products!=4096)throw std::runtime_error("Product count must be12 or4096");
        D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT caps{};
        caps.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
        caps.WaveMatrixMultiply.Inputs={16,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32};
        check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&caps,sizeof(caps)),"INT8 Wave support");
        if(!(UINT(caps.WaveMatrixMultiply.SupportFlags)&1)||(UINT(caps.WaveMatrixMultiply.SupportFlags)&6)||!caps.WaveMatrixMultiply.NumShapes||caps.WaveMatrixMultiply.NumShapes>256)throw std::runtime_error("Native INT8 Wave unavailable");
        std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(caps.WaveMatrixMultiply.NumShapes);caps.WaveMatrixMultiply.Shapes=shapes.data();
        check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&caps,sizeof(caps)),"INT8 shapes");
        if(!std::any_of(shapes.begin(),shapes.end(),[](auto s){return s.M==8&&s.K==32&&s.N==16;}))throw std::runtime_error("INT8 8x32x16 unavailable");
        linAlgCheckWaveFP16(device.Get());
        auto floats=make(UINT64(products)*256,false),a=make(UINT64(products)*64,false),b=make(UINT64(products)*128,false);
        auto integer=make(UINT64(products)*128,false),decoded=make(integer->elements,false);
        auto halfA=make(UINT64(products)*128,false),halfB=make(UINT64(products)*256,false),halfOut=make(UINT64(products)*64,false);
        std::vector<float> af(products*256);std::vector<int8_t> aq(products*256),bq(products*512);
        std::vector<uint16_t> ah(products*256),bh(products*512);
        float tieValues[]={-130.f,-127.5f,-126.5f,-.5f,.5f,126.5f,127.5f,130.f};
        for(UINT p=0;p<products;++p){
            for(UINT row=0;row<8;++row)for(UINT k=0;k<32;++k){UINT i=p*256+row*32+k;
                float v=(p==0&&row==0&&k<8)?tieValues[k]:float(int((i*3+p*7)%255)-127);
                af[i]=v/32;aq[i]=int8_t(std::nearbyint(std::clamp(v,-127.f,127.f)));
                ah[p*256+(k/16)*128+row*16+k%16]=DirectX::PackedVector::XMConvertFloatToHalf(float(aq[i])/32);}
            for(UINT k=0;k<32;++k)for(UINT col=0;col<16;++col){UINT i=p*512+k*16+col;bq[i]=int8_t(int((i*5+p*11)%255)-127);
                bh[p*512+(k/16)*256+(k%16)*16+col]=DirectX::PackedVector::XMConvertFloatToHalf(float(bq[i])/64);}
        }
        uploadLanes(floats,af);
        auto uploadBytes=[&](Tensor* target,const void* data,UINT64 bytes){if(bytes!=target->elements*4)throw std::runtime_error("Raw upload size");std::vector<float> raw(size_t(bytes/4));memcpy(raw.data(),data,size_t(bytes));uploadLanes(target,raw);};
        uploadBytes(b,bq.data(),bq.size());uploadBytes(halfA,ah.data(),ah.size()*2);uploadBytes(halfB,bh.data(),bh.size()*2);
        auto root=linAlgRoot(device.Get(),false);std::map<std::string,ComPtr<ID3D12PipelineState>> psos;
        for(const char* n:{"quantize","integer","half","dequantize"})psos[n]=linAlgPso(device.Get(),root.Get(),(std::string("int8-")+n+".dxil").c_str());
        auto bind=[&](const char* n,Tensor* x,Tensor* y,Tensor* z,UINT dispatch){
            x->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);y->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);z->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(psos.at(n).Get());
            cmd->SetComputeRootShaderResourceView(0,x->resource->GetGPUVirtualAddress());cmd->SetComputeRootShaderResourceView(1,y->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(2,z->resource->GetGPUVirtualAddress());
            float inv=32,scale=1.f/2048;UINT constants[]={products,products*64,0,0};memcpy(constants+2,&inv,4);memcpy(constants+3,&scale,4);
            cmd->SetComputeRoot32BitConstants(3,4,constants,0);cmd->Dispatch(dispatch,1,1);uavBarrier(cmd.Get());};
        auto profiler=std::make_shared<NativeGpuProfile>(device.Get(),32);std::vector<float> firstInteger,firstDecoded,firstHalf,firstPacked;
        for(UINT run=0;run<8;++run){profiler->reset();
            {NativeProfileRange total(profiler.get(),cmd.Get(),"int8_pipeline");
                {NativeProfileRange stage(profiler.get(),cmd.Get(),"quantize_a");bind("quantize",floats,b,a,products);}
                {NativeProfileRange stage(profiler.get(),cmd.Get(),"integer_product");bind("integer",a,b,integer,products);}
                {NativeProfileRange stage(profiler.get(),cmd.Get(),"dequantize");bind("dequantize",integer,b,decoded,(products*128+63)/64);}}
            {NativeProfileRange total(profiler.get(),cmd.Get(),"fp16_product");bind("half",halfA,halfB,halfOut,products);}
            profiler->resolve(cmd.Get());submit();auto times=profiler->readAfterFence(queue.Get());
            for(auto& t:times)printf("TIME run=%u name=%s ms=%.9f\n",run,t.first.c_str(),t.second.ms);
            auto ir=downloadLanes(integer),dr=downloadLanes(decoded),hr=downloadLanes(halfOut),qr=downloadLanes(a);
            if(!run){firstInteger=ir;firstDecoded=dr;firstHalf=hr;firstPacked=qr;}
            else if(memcmp(ir.data(),firstInteger.data(),ir.size()*4)||memcmp(dr.data(),firstDecoded.data(),dr.size()*4)||memcmp(hr.data(),firstHalf.data(),hr.size()*4)||memcmp(qr.data(),firstPacked.data(),qr.size()*4))throw std::runtime_error("INT8 repeat mismatch");
        }
        auto save=[&](const char* name,const void* data,size_t bytes){std::ofstream f(nativePath(folder+"/"+name+".bin"),std::ios::binary);f.write(reinterpret_cast<const char*>(data),bytes);if(!f)throw std::runtime_error("INT8 artifact write");};
        save("a_float",af.data(),af.size()*4);save("a_expected",aq.data(),aq.size());save("b",bq.data(),bq.size());save("a_gpu",firstPacked.data(),firstPacked.size()*4);
        save("integer",firstInteger.data(),firstInteger.size()*4);save("decoded",firstDecoded.data(),firstDecoded.size()*4);save("half",firstHalf.data(),firstHalf.size()*4);
        printf("INT8_WAVE_DISPATCH_PASS products=%u M=8 K=32 N=16 wave=16 repeat_exact=1\n",products);
    }
};
int main(int argc,char** argv){try{if(argc!=3)throw std::runtime_error("Usage: int8_unit output products");std::filesystem::create_directories(nativePath(argv[1]));Int8Unit p;p.run(argv[1],std::stoul(argv[2]));return 0;}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
