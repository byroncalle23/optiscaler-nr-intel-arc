// Isolate official Intel sample device/size-query contract. No GPU dispatch.
#define main identityCompatibilityProbe
#include "coop_vector_probe.cpp"
#undef main
#include <d3d12sdklayers.h>
#include <string>
int main(int argc,char** argv){try{
    const bool debugEnabled=argc>1&&std::string(argv[1])=="debug";
    UUID features[]={D3D12ExperimentalShaderModels,D3D12CooperativeVectorExperiment};
    ck(D3D12EnableExperimentalFeatures(2,features,nullptr,nullptr),"EnableExperimentalFeatures");
    if(debugEnabled){ComPtr<ID3D12Debug> debug;ck(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"Debug");debug->EnableDebugLayer();}
    ComPtr<IDXGIFactory6> factory;ck(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"Factory");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x8086&&d.DeviceId==0xe20b){adapter=a;break;}}
    if(!adapter)throw std::runtime_error("B580 missing");
    ComPtr<ID3D12Device> device;ck(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&device)),"Device");
    D3D12_FEATURE_DATA_D3D12_OPTIONS_EXPERIMENTAL options{};
    ck(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL,&options,sizeof(options)),"Options");
    printf("TIER %u\n",UINT(options.CooperativeVectorTier));
    ComPtr<ID3D12DevicePreview> preview;ck(device.As(&preview),"DevicePreview");
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(GetModuleHandleW(L"D3D12Core.dll"),module,MAX_PATH);wprintf(L"RUNTIME %s\n",module);
    D3D12_FEATURE_DATA_COOPERATIVE_VECTOR properties{};
    ck(device->CheckFeatureSupport(D3D12_FEATURE_COOPERATIVE_VECTOR,&properties,sizeof(properties)),"PropertyCounts");
    printf("PROPERTY_COUNTS %u %u %u\n",properties.MatrixVectorMulAddPropCount,properties.OuterProductAccumulatePropCount,properties.VectorAccumulatePropCount);
    if(properties.MatrixVectorMulAddPropCount>1024||properties.OuterProductAccumulatePropCount>1024||properties.VectorAccumulatePropCount>1024)throw std::runtime_error("Property count budget");
    std::vector<D3D12_COOPERATIVE_VECTOR_PROPERTIES_MUL> mul(properties.MatrixVectorMulAddPropCount);
    std::vector<D3D12_COOPERATIVE_VECTOR_PROPERTIES_ACCUMULATE> outer(properties.OuterProductAccumulatePropCount),acc(properties.VectorAccumulatePropCount);
    properties.pMatrixVectorMulAddProperties=mul.data();properties.pOuterProductAccumulateProperties=outer.data();properties.pVectorAccumulateProperties=acc.data();
    ck(device->CheckFeatureSupport(D3D12_FEATURE_COOPERATIVE_VECTOR,&properties,sizeof(properties)),"Properties");
    for(const auto& p:mul)printf("MUL_PROPERTY %u %u %u %u %u %u\n",UINT(p.InputType),UINT(p.InputInterpretation),UINT(p.MatrixInterpretation),UINT(p.BiasInterpretation),UINT(p.OutputType),UINT(p.TransposeSupported));
    const D3D12_LINEAR_ALGEBRA_DATATYPE types[]={D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT_E4M3,D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT_E5M2,D3D12_LINEAR_ALGEBRA_DATATYPE_SINT8,D3D12_LINEAR_ALGEBRA_DATATYPE_UINT8};
    for(auto type:types)for(UINT layout=0;layout<4;++layout)for(UINT size:{16u,64u,128u})for(UINT initial:{0u,0x12345670u}){
        D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_DEST_INFO d{};
        d.DestSize=initial;d.DestLayout=D3D12_LINEAR_ALGEBRA_MATRIX_LAYOUT(layout);d.DestStride=layout<2?size*(type==D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32?4:type==D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16?2:1):0;
        d.NumRows=d.NumColumns=size;d.DestDataType=type;
        preview->GetLinearAlgebraMatrixConversionDestinationInfo(&d);
        printf("QUERY type=%u layout=%u rows=%u cols=%u initial=%u result=%u stride=%u\n",UINT(type),layout,size,size,initial,d.DestSize,d.DestStride);
    }
    ComPtr<ID3D12InfoQueue> info;if(debugEnabled){ck(device.As(&info),"InfoQueue");
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);
            std::vector<char> storage(bytes);auto p=reinterpret_cast<D3D12_MESSAGE*>(storage.data());ck(info->GetMessage(i,p,&bytes),"Message");printf("DEBUG severity=%u %s\n",UINT(p->Severity),p->pDescription);}}
    ck(device->GetDeviceRemovedReason(),"DeviceState");return 0;
}catch(const std::exception& e){fprintf(stderr,"LAYOUT_QUERY_FAILED %s\n",e.what());return 1;}}
