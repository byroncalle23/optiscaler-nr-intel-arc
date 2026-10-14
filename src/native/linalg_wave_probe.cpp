#define main nr_unused_graph_main
#include "linalg_batch_graph.cpp"
#undef main
class WaveProbe : public BatchGraphProbe {
public:
    void run(const std::string& folder){
        D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT support{};
        support.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
        support.WaveMatrixMultiply.Inputs={16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16};
        check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&support,sizeof(support)),"Wave FP16 support");
        if(!(support.WaveMatrixMultiply.SupportFlags&D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_SUPPORTED)||!support.WaveMatrixMultiply.NumShapes||support.WaveMatrixMultiply.NumShapes>256)throw std::runtime_error("Wave FP16 unsupported");
        std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(support.WaveMatrixMultiply.NumShapes);support.WaveMatrixMultiply.Shapes=shapes.data();
        check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&support,sizeof(support)),"Wave FP16 shapes");
        if(!std::any_of(shapes.begin(),shapes.end(),[](auto s){return s.M==8&&s.K==16&&s.N==16;}))throw std::runtime_error("Wave 8x16x16 shape missing");
        constexpr UINT products=12;
        auto a=make(products*64,false),b=make(products*128,false),out=make(products*64,false);
        std::vector<uint16_t> ah(products*128),bh(products*256);
        for(UINT p=0;p<products;++p){for(UINT i=0;i<128;++i)ah[p*128+i]=DirectX::PackedVector::XMConvertFloatToHalf(float(int((i*3+p*7)%31)-15)/16);
            for(UINT i=0;i<256;++i)bh[p*256+i]=DirectX::PackedVector::XMConvertFloatToHalf(float(int((i*5+p*11)%29)-14)/32);}
        auto uploadHalf=[&](Tensor* tensor,const std::vector<uint16_t>& halves){std::vector<float> raw(halves.size()/2);memcpy(raw.data(),halves.data(),halves.size()*2);uploadLanes(tensor,raw);};
        uploadHalf(a,ah);uploadHalf(b,bh);auto root=linAlgRoot(device.Get(),false);auto pso=linAlgPso(device.Get(),root.Get(),"wave-probe.dxil");
        std::vector<float> first;
        for(UINT run=0;run<3;++run){
            a->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);b->to(cmd.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);out->to(cmd.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pso.Get());cmd->SetComputeRootShaderResourceView(0,a->resource->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(1,b->resource->GetGPUVirtualAddress());cmd->SetComputeRootUnorderedAccessView(2,out->resource->GetGPUVirtualAddress());
            UINT zero[4]{};cmd->SetComputeRoot32BitConstants(3,4,zero,0);cmd->Dispatch(products,1,1);uavBarrier(cmd.Get());submit();
            auto raw=downloadLanes(out);if(!run)first=raw;else if(memcmp(raw.data(),first.data(),raw.size()*4))throw std::runtime_error("Wave repeat mismatch");
        }
        for(auto entry:std::vector<std::pair<std::string,const std::vector<uint16_t>*>>{{"a",&ah},{"b",&bh}}){std::ofstream f(folder+"/"+entry.first+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(entry.second->data()),entry.second->size()*2);}
        std::ofstream f(folder+"/output.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(first.data()),first.size()*4);
        puts("WAVE_FP16_DISPATCH_PASS products=12 M=8 K=16 N=16 wave=16 repeat_exact=1");
    }
};
int main(int argc,char** argv){try{if(argc!=2)throw std::runtime_error("Usage: wave_probe output");std::filesystem::create_directories(argv[1]);WaveProbe p;p.run(argv[1]);return 0;}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
