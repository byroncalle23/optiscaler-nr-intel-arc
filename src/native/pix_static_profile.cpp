#define NOMINMAX
#include <windows.h>
#include <wrl.h>
#include <experimental/PixApiExperimental.h>
#include <cstdio>
#include <fstream>
#include <string>
using Microsoft::WRL::ComPtr;
class PixStudyLog:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IPixLogging>{public:void STDMETHODCALLTYPE ReportLog(PIX_LOGGING_MESSAGE log)override{printf("PIX_LOG %ls\n",log.Message);}};
void ck(HRESULT hr,const char* step){if(FAILED(hr)){printf("FAIL %s HRESULT=0x%08X\n",step,unsigned(hr));throw hr;}}
std::wstring read(const wchar_t* path){std::ifstream f(path,std::ios::binary);if(!f)throw E_INVALIDARG;std::string s((std::istreambuf_iterator<char>(f)),{});int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),out.data(),n);return out;}
int wmain(int argc,wchar_t** argv){try{CoInitializeEx(nullptr,COINIT_MULTITHREADED);
if(GetEnvironmentVariableA("NR_PIX_EXPERIMENTAL",nullptr,0)){
    ComPtr<ID3D12SDKConfiguration1> sdk;ck(D3D12GetInterface(CLSID_D3D12SDKConfiguration,IID_PPV_ARGS(&sdk)),"Experimental SDK configuration");
    ComPtr<ID3D12DeviceFactory> localFactory;
    ck(sdk->CreateDeviceFactory(721,"D:\\Dlss nr en intel arc b580 Antigravity\\work\\b580\\pix-profile\\runner\\native_nr_runtime\\",IID_PPV_ARGS(&localFactory)),"SDK721.3 factory");
    UUID feature=D3D12ExperimentalShaderModels;auto hr=localFactory->EnableExperimentalFeatures(1,&feature,nullptr,nullptr);
    printf("FACTORY_EXPERIMENTAL_SHADER_MODELS HRESULT=0x%08X\n",unsigned(hr));ck(hr,"Experimental models");
    ck(localFactory->ApplyToGlobalState(),"Offline-only global factory application");

}
wchar_t pixRoot[32768]{};if(!GetEnvironmentVariableW(L"NR_PIX_ROOT",pixRoot,32768))wcscpy_s(pixRoot,L"C:\\Program Files\\Microsoft PIX Preview\\2606.18-preview");SetDllDirectoryW(pixRoot);auto apiPath=std::wstring(pixRoot)+L"\\pixapi.dll";auto module=LoadLibraryW(apiPath.c_str());if(!module)throw HRESULT_FROM_WIN32(GetLastError());auto create=reinterpret_cast<decltype(&PixCreateFactory)>(GetProcAddress(module,"PixCreateFactory"));ComPtr<IPixFactory> factory;ck(create(IID_PPV_ARGS(&factory)),"Factory");auto logger=Microsoft::WRL::Make<PixStudyLog>();ck(factory->SetLogger(logger.Get()),"Logger");ComPtr<IPixFactoryExperimental> experimental;ck(factory.As(&experimental),"Experimental factory");ComPtr<IPixShaderProfilingDocument> doc;ck(experimental->OpenShaderProfilingDocument(IID_PPV_ARGS(&doc)),"Shader document");ComPtr<IPixCollection> adapters;ck(doc->GetAdapters(IID_PPV_ARGS(&adapters)),"Adapters");ComPtr<IPixShaderProfilingAdapter> selected;
for(UINT64 i=0;i<adapters->GetCount();++i){ComPtr<IPixShaderProfilingAdapter> a;ck(adapters->Get(i,IID_PPV_ARGS(&a)),"Adapter");printf("ADAPTER %llu %ls family=%ls vendor=%ls\n",i,a->GetName(),a->GetFamilyName(),a->GetVendorName());std::wstring family=a->GetFamilyName()?a->GetFamilyName():L"";std::wstring name=a->GetName();if(name.find(L"B580")!=name.npos||family.find(L"Xe2")!=family.npos||family.find(L"Battlemage")!=family.npos)selected=a;}
if(argc==1)return 0;if(!selected)throw E_FAIL;
auto contents=read(argv[1]);LPCWSTR paths[]={argv[1]},sources[]={contents.c_str()};PIX_SHADER_PROFILING_SOURCE_SHADER shader{};shader.Stage=PIX_SHADER_STAGE_COMPUTE;shader.SourceCount=1;shader.SourcePaths=paths;shader.SourceContents=sources;shader.Target=argc>4?argv[4]:L"cs_6_10";shader.Entry=argv[2];shader.Flags=argv[3];shader.Defines=L"";
D3D12_ROOT_PARAMETER params[9]{};
for(UINT i=0;i<4;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[i].Descriptor.ShaderRegister=i;}
for(UINT i=0;i<3;++i){params[i+4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[i+4].Descriptor.ShaderRegister=i;}
params[7].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[7].Constants={0,0,16};params[8].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[8].Constants={1,0,8};
D3D12_ROOT_SIGNATURE_DESC rootDesc{9,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> rootBlob,errorBlob;ck(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,&errorBlob),"Root");D3D12_GLOBAL_SERIALIZED_ROOT_SIGNATURE root{{rootBlob->GetBufferPointer(),rootBlob->GetBufferSize()}};LPCWSTR exports[]={argv[2]};D3D12_GENERIC_PROGRAM_DESC program{L"Study",1,exports,0,nullptr};D3D12_STATE_SUBOBJECT subs[]={{D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_SERIALIZED_ROOT_SIGNATURE,&root},{D3D12_STATE_SUBOBJECT_TYPE_GENERIC_PROGRAM,&program}};PIX_PIPELINE_STATE_DESC pipeline{2,subs};ComPtr<IPixShaderProfilingCompilation> compilation;ck(doc->Compile(selected.Get(),&pipeline,1,&shader,nullptr,nullptr,nullptr,IID_PPV_ARGS(&compilation)),"Compile");for(UINT i=0;i<compilation->GetCompilerOutputCount(shader.Stage);++i){LPCWSTR out;ck(compilation->GetCompilerOutput(shader.Stage,i,&out),"Compiler output");printf("COMPILER %ls\n",out);}if(!compilation->Succeeded())throw E_FAIL;
ComPtr<IPixShaderProfilingStaticResult> result;ck(compilation->GetResult(IID_PPV_ARGS(&result)),"Result");printf("TARGET %ls family=%ls\n",result->GetAdapterName(),result->GetAdapterFamily());
for(UINT s=0;s<result->GetShaderCount();++s){ComPtr<IPixShaderProfilingStaticShader> sh;ck(result->GetShader(s,IID_PPV_ARGS(&sh)),"Shader");for(UINT i=0;i<sh->GetInfoCount();++i){PIX_SHADER_PROFILING_SHADER_INFO info;ck(sh->GetInfo(i,&info),"Info");printf("INFO %ls=%ls\n",info.Name,info.Value);}for(UINT i=0;i<result->GetRegisterTypeCount();++i){PIX_SHADER_PROFILING_REGISTER_TYPE type;ck(result->GetRegisterType(i,&type),"Register type");printf("REGISTERS %ls allocated=%u\n",type.Name,sh->GetRegisterAllocatedCount(type.Id));}for(UINT b=0;b<sh->GetBlockCount();++b){ComPtr<IPixShaderProfilingStaticBlock> block;ck(sh->GetBlock(b,IID_PPV_ARGS(&block)),"Block");for(UINT i=0;i<block->GetInstructionCount();++i){ComPtr<IPixShaderProfilingStaticInstruction> instruction;ck(block->GetInstruction(i,IID_PPV_ARGS(&instruction)),"Instruction");printf("ISA %ls\n",instruction->GetText());}}}return 0;
}catch(HRESULT hr){printf("PIX_STATIC_FAILED 0x%08X\n",unsigned(hr));return 1;}}


