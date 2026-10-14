#pragma once
// Compiled only into the isolated SDK721 standalone experiment.
struct LinAlgProjection {
    ComPtr<ID3D12Resource> packed, optimal;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    UINT rows=0, columns=0, inner=0, stride=0;
    UINT groups=1, lanes=1, tile=64;
#ifdef NR_CLEAN_PROFILE
    bool inputShadow=false,reuseRows16=false;
    ComPtr<ID3D12RootSignature> studyPackRoot;
    ComPtr<ID3D12PipelineState> studyPackPipeline;
#endif
#ifdef NR_DENSE_WAVE
    bool waveDense=false;
#endif
};
struct Tensor;
struct LinAlgWaveGemm {
    Tensor *packedA=nullptr,*packedB=nullptr;
    ComPtr<ID3D12RootSignature> root,packRoot;
    ComPtr<ID3D12PipelineState> pipeline,packPipeline;
    UINT m=0,n=0,k=0,mp=0,np=0,kp=0,groups=0,lanes=0;
    UINT64 aOffset=0,bOffset=0;
    bool transposeB=false;
    bool inlinePreparation=false;
};
// DLL integration supplies its verified package path on the preparation worker.
// Standalone experiments retain their executable-relative lookup.
inline thread_local std::filesystem::path nrLinAlgShaderDirectory;
inline std::vector<char> linAlgShader(const char* file) {
    char directory[32768]{};
    if(!GetModuleFileNameA(nullptr,directory,sizeof(directory)))throw std::runtime_error("LinAlg executable path");
    auto folder=nrLinAlgShaderDirectory.empty()?std::filesystem::path(directory).parent_path():nrLinAlgShaderDirectory;
#ifdef NR_CLEAN_PROFILE
    if(std::string(file)=="branch-segment-pair.dxil"){
        wchar_t study[32768]{};if(!GetEnvironmentVariableW(L"NR_STUDY_BRANCH_SEGMENT_DIR",study,32768))throw std::runtime_error("Branch segment study directory missing");
        folder=study;printf("NR_BRANCH_SEGMENT_SHADER file=%s\n",file);
    }
    if(GetEnvironmentVariableA("NR_STUDY_FP16_DENSE_DIR",nullptr,0)){
        if(std::string(file)=="attention-softmax-wave8.dxil")file="fp16dense-attention.dxil";
        if(std::string(file)=="branch-ProjectSum.dxil")file="fp16dense-branch.dxil";
    }
    if(std::string(file).rfind("fp16dense-",0)==0){
        wchar_t study[32768]{};if(!GetEnvironmentVariableW(L"NR_STUDY_FP16_DENSE_DIR",study,32768))throw std::runtime_error("FP16 dense directory missing");
        folder=study;printf("NR_FP16_DENSE_SHADER file=%s\n",file);
    }
    if(std::string(file).rfind("c32i8-",0)==0){
        wchar_t overrideFolder[32768]{};
        if(!GetEnvironmentVariableW(L"NR_STUDY_C32_INT8_DIR",overrideFolder,32768))throw std::runtime_error("C32 INT8 study directory missing");
        folder=overrideFolder;
        printf("NR_STUDY_C32_INT8_LOADED file=%s\n",file);
    }
    // Restrict the isolated study to the measured whole-window attention block.
    if(std::string(file)=="attention-wg256.dxil"||std::string(file)=="attention-softmax-wave8.dxil"){
        wchar_t overrideFolder[32768]{};
        if(GetEnvironmentVariableW(L"NR_STUDY_SHADER_DIR",overrideFolder,32768)>0){
            auto candidate=std::filesystem::path(overrideFolder)/file;
            if(!std::filesystem::is_regular_file(candidate))throw std::runtime_error("Study attention shader missing");
            folder=overrideFolder;
            printf("NR_STUDY_ATTENTION_LOADED file=%s path=%s\n",file,candidate.string().c_str());
        }
    }
    if(std::string(file)=="c32-FullFFN.dxil"){
        wchar_t overrideFolder[32768]{};
        if(!GetEnvironmentVariableW(L"NR_STUDY_FFN_DIR",overrideFolder,32768))throw std::runtime_error("Study FFN shader directory missing");
        folder=overrideFolder;
    }
    if(std::string(file)=="dense-register32.dxil"||std::string(file)=="dense-wave-pack.dxil"){
        wchar_t overrideFolder[32768]{};
        if(GetEnvironmentVariableW(L"NR_STUDY_DENSE_DIR",overrideFolder,32768)>0)folder=overrideFolder;
    }
#endif
    std::ifstream f(folder/file,std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error("LinAlg shader missing");
    auto size=f.tellg();if(size<=0)throw std::runtime_error("LinAlg shader empty");
    std::vector<char> data(static_cast<size_t>(size));f.seekg(0);if(!f.read(data.data(),data.size()))throw std::runtime_error("LinAlg shader read");return data;
}
inline ComPtr<ID3D12RootSignature> linAlgRoot(ID3D12Device* device,bool pack,bool grouped=false,UINT constants=0) {
    D3D12_ROOT_PARAMETER p[4]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[0].Descriptor.ShaderRegister=0;
    p[1].ParameterType=pack?D3D12_ROOT_PARAMETER_TYPE_UAV:D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[1].Descriptor.ShaderRegister=pack?0:1;
    p[2].ParameterType=pack?D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:D3D12_ROOT_PARAMETER_TYPE_UAV;
    if(pack)p[2].Constants.Num32BitValues=constants?constants:(grouped?4:2);
    p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[3].Constants.Num32BitValues=constants?constants:(grouped?6:4);
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=pack?3:4;desc.pParameters=p;
    ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"LinAlg root serialize");
    ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"LinAlg root");return root;
}
inline ComPtr<ID3D12PipelineState> linAlgPso(ID3D12Device* device,ID3D12RootSignature* root,const char* file) {
    auto shader=linAlgShader(file);D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root;desc.CS={shader.data(),shader.size()};
    ComPtr<ID3D12PipelineState> pso;check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pso)),"LinAlg graph PSO");return pso;
}
inline void linAlgCheckWaveFP16(ID3D12Device* device){
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT support{};
    support.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
    support.WaveMatrixMultiply.Inputs={16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16};
    check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&support,sizeof(support)),"Wave FP16 support");
    if(!(support.WaveMatrixMultiply.SupportFlags&D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_SUPPORTED)||!support.WaveMatrixMultiply.NumShapes||support.WaveMatrixMultiply.NumShapes>256)throw std::runtime_error("Wave FP16 unsupported");
    std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(support.WaveMatrixMultiply.NumShapes);support.WaveMatrixMultiply.Shapes=shapes.data();
    check(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&support,sizeof(support)),"Wave FP16 shapes");
    if(!std::any_of(shapes.begin(),shapes.end(),[](auto s){return s.M==8&&s.K==16&&s.N==16;}))throw std::runtime_error("Wave FP16 8x16x16 missing");
}
inline ComPtr<ID3D12RootSignature> linAlgWindowRoot(ID3D12Device* device){
    D3D12_ROOT_PARAMETER p[5]{};
    p[0].ParameterType=p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1].Descriptor.ShaderRegister=2;
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[3].Constants.Num32BitValues=4;
    p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[4].Descriptor.ShaderRegister=1;
    D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=5;desc.pParameters=p;
    ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"Fused window root serialize");
    ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Fused window root");return root;
}
