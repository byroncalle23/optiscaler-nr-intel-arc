#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
#ifndef NR_PROBE_SDK_VERSION
#define NR_PROBE_SDK_VERSION 721
#endif
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion=NR_PROBE_SDK_VERSION; __declspec(dllexport) extern const char* D3D12SDKPath=".\\D3D12\\"; }
void ck(HRESULT hr,const char* label){printf("%s HRESULT 0x%08X\n",label,UINT(hr));if(FAILED(hr))throw std::runtime_error(label);}
int main(int argc,char** argv){try{
    const bool debugEnabled=argc>1&&std::string(argv[1])=="debug";
    if(argc==3&&std::string(argv[1])=="select"){
        ComPtr<ID3D12SDKConfiguration> configuration;
        ck(D3D12GetInterface(CLSID_D3D12SDKConfiguration,IID_PPV_ARGS(&configuration)),"SDKConfiguration");
        ck(configuration->SetSDKVersion(721,argv[2]),"EarlySetSDK721");
    }
    // Current Microsoft LinAlg preview contract: only ExperimentalShaderModels.
    UUID features[]={D3D12ExperimentalShaderModels};
    if(argc>1&&std::string(argv[1])=="host")printf("EXPERIMENTAL_FEATURES_NOT_ENABLED host-runtime reproduction only\n");
    else ck(D3D12EnableExperimentalFeatures(1,features,nullptr,nullptr),"EnableExperimentalFeatures");
    if(debugEnabled){ComPtr<ID3D12Debug> debug;ck(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"Debug");debug->EnableDebugLayer();}
    ComPtr<IDXGIFactory6> factory;ck(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Factory");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x8086&&d.DeviceId==0xe20b){adapter=a;break;}}
    if(!adapter)throw std::runtime_error("B580 missing");printf("ADAPTER B580 PCI8086:E20B\n");
    ComPtr<ID3D12Device> device;ck(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&device)),"Device");
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(GetModuleHandleW(L"D3D12Core.dll"),module,MAX_PATH);wprintf(L"RUNTIME %s\n",module);
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_10};
    auto smhr=device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm));printf("SHADER_MODEL_HR 0x%08X MAX 0x%X\n",UINT(smhr),UINT(sm.HighestShaderModel));
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT support{};
    auto hr=device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT,&support,sizeof(support));
    printf("LINALG_HR 0x%08X TIER %u\n",UINT(hr),UINT(support.LinearAlgebraTier));
    if(SUCCEEDED(hr)&&support.LinearAlgebraTier>=D3D12_LINEAR_ALGEBRA_TIER_1_0){
        D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT operation{};
        operation.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY;
        operation.ThreadVectorMatrixMultiply.VectorInputType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
        operation.ThreadVectorMatrixMultiply.MatrixInputType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
        operation.ThreadVectorMatrixMultiply.BiasInputType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
        operation.ThreadVectorMatrixMultiply.VectorResultType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
        auto oph=device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&operation,sizeof(operation));
        printf("THREAD_FP16_HR 0x%08X FLAGS %u\n",UINT(oph),UINT(operation.ThreadVectorMatrixMultiply.SupportFlags));
        // Integer and mixed configurations must be queried; hardware INT8 TOPS
        // do not establish that this driver exposes the corresponding DX path.
        struct TypedCase {D3D12_LINEAR_ALGEBRA_DATATYPE a,b,acc;};
        for(auto types:std::vector<TypedCase>{
            {D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32},
            {D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32},
            {D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32},
            {D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT32},
            {D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT32},
            {D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16},
            {D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16},
            {D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32}}){
            for(auto bias:{types.acc}){
                D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT thread{};thread.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY;
                auto& t=thread.ThreadVectorMatrixMultiply;t.VectorInputType=types.a;t.MatrixInputType=types.b;t.BiasInputType=bias;t.VectorResultType=types.acc;
                auto th=device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&thread,sizeof(thread));
                printf("TYPED_THREAD a=%u b=%u bias=%u result=%u HRESULT=0x%08X flags=%u\n",UINT(types.a),UINT(types.b),UINT(bias),UINT(types.acc),UINT(th),UINT(t.SupportFlags));
            }
            for(UINT ws:{8u,16u,32u,64u}){
                D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT wave{};wave.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
                wave.WaveMatrixMultiply.Inputs={ws,types.a,types.b,types.acc};
                auto wh=device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&wave,sizeof(wave));
                printf("TYPED_WAVE a=%u b=%u accumulator=%u wave=%u HRESULT=0x%08X flags=%u shapes=%u\n",UINT(types.a),UINT(types.b),UINT(types.acc),ws,UINT(wh),UINT(wave.WaveMatrixMultiply.SupportFlags),wave.WaveMatrixMultiply.NumShapes);
                if(SUCCEEDED(wh)&&wave.WaveMatrixMultiply.NumShapes&&wave.WaveMatrixMultiply.NumShapes<=256){
                    std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(wave.WaveMatrixMultiply.NumShapes);wave.WaveMatrixMultiply.Shapes=shapes.data();
                    ck(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&wave,sizeof(wave)),"TypedWaveShapes");
                    for(auto s:shapes){printf("TYPED_SHAPE a=%u b=%u accumulator=%u wave=%u M=%u K=%u N=%u\n",UINT(types.a),UINT(types.b),UINT(types.acc),ws,s.M,s.K,s.N);
                        D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT group{};group.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREADGROUP_MATRIX_MULTIPLY;
                        group.ThreadGroupMatrixMultiply.WaveInputs=wave.WaveMatrixMultiply.Inputs;group.ThreadGroupMatrixMultiply.Shape=s;
                        auto gh=device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&group,sizeof(group));
                        printf("TYPED_GROUP a=%u b=%u accumulator=%u wave=%u M=%u K=%u N=%u HRESULT=0x%08X flags=%u\n",UINT(types.a),UINT(types.b),UINT(types.acc),ws,s.M,s.K,s.N,UINT(gh),UINT(group.ThreadGroupMatrixMultiply.SupportFlags));}
                }
            }
        }
        for(auto accumulator:{D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32})for(UINT waveSize:{8u,16u,32u,64u}){
            D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT wave{};
            wave.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_WAVE_MATRIX_MULTIPLY;
            wave.WaveMatrixMultiply.Inputs={waveSize,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,accumulator};
            auto wh=device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&wave,sizeof(wave));
            printf("WAVE_FP16 accumulator=%u size=%u HRESULT=0x%08X flags=%u shapes=%u\n",UINT(accumulator),waveSize,UINT(wh),UINT(wave.WaveMatrixMultiply.SupportFlags),wave.WaveMatrixMultiply.NumShapes);
            if(SUCCEEDED(wh)&&wave.WaveMatrixMultiply.NumShapes&&wave.WaveMatrixMultiply.NumShapes<=256){
                std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE> shapes(wave.WaveMatrixMultiply.NumShapes);
                wave.WaveMatrixMultiply.Shapes=shapes.data();
                ck(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&wave,sizeof(wave)),"WaveShapes");
                for(auto s:shapes)printf("WAVE_SHAPE accumulator=%u wavesize=%u M=%u K=%u N=%u\n",UINT(accumulator),waveSize,s.M,s.K,s.N);
            }
            for(auto shape:std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_MULTIPLY_SHAPE>{{16,16,16},{32,32,32},{64,32,64},{64,64,32},{64,64,64},{128,32,128},{144,32,192},{144,192,32},{256,32,256},{256,256,32}}){
                D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT group{};
                group.OperationType=D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREADGROUP_MATRIX_MULTIPLY;
                group.ThreadGroupMatrixMultiply.WaveInputs=wave.WaveMatrixMultiply.Inputs;
                group.ThreadGroupMatrixMultiply.Shape=shape;
                auto gh=device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT,&group,sizeof(group));
                auto& g=group.ThreadGroupMatrixMultiply;
                printf("THREADGROUP_FP16 accumulator=%u wave=%u M=%u K=%u N=%u HRESULT=0x%08X flags=%u min=%u max=%u preferred=%u\n",UINT(accumulator),waveSize,shape.M,shape.K,shape.N,UINT(gh),UINT(g.SupportFlags),g.MinThreadGroupSize,g.MaxThreadGroupSize,g.PreferredThreadGroupSize);
            }
        }
        ComPtr<ID3D12DevicePreview> preview;ck(device.As(&preview),"DevicePreview");
        for(UINT layout=0;layout<3;++layout)for(UINT size:{16u,32u,64u,128u})for(UINT initial:{0u,0x12345670u}){
            D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO d{};d.DestSize=initial;
            d.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT(layout);d.DestStride=layout<2?size*2:0;
            d.NumRows=d.NumColumns=size;d.DestDataType=D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
            preview->GetLinearAlgebraMatrixConversionDestinationInfo(&d);
            printf("QUERY layout=%u rows=%u initial=%u result=%u\n",layout,size,initial,d.DestSize);
        }
    }else printf("NO_LINALG_SUPPORT_SKIP_CONVERSION_QUERY\n");
    if(debugEnabled){ComPtr<ID3D12InfoQueue> info;ck(device.As(&info),"InfoQueue");
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);
            std::vector<char> storage(bytes);auto msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data());ck(info->GetMessage(i,msg,&bytes),"Message");printf("DEBUG severity=%u %s\n",UINT(msg->Severity),msg->pDescription);}}
    ck(device->GetDeviceRemovedReason(),"DeviceState");return 0;
}catch(const std::exception& e){fprintf(stderr,"LINALG_PROBE_FAILED %s\n",e.what());return 1;}}
