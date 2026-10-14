// Standalone numerical test. Upload/readback belong to this test harness only.
// Production integration will bind resident buffers and record on the game's list.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <directml.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>

using Microsoft::WRL::ComPtr;
static void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char message[200];
        snprintf(message, sizeof(message), "%s failed: 0x%08lx", operation, (unsigned long)hr);
        throw std::runtime_error(message);
    }
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes,
    D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap; props.CreationNodeMask = props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (heap == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state,
        nullptr, IID_PPV_ARGS(&result)), "Create buffer");
    return result;
}
static void uavBarrier(ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &b);
}
static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}
int main(int argc, char** argv) {
    HANDLE event = nullptr;
    try {
        bool fp16 = false, int8 = false, unsignedA=false,ownedB=false;
        bool debug=false;
        bool conv1x1=false;
        UINT m = 2, n = 2, k = 2, runs = 1;
        std::string operandsFolder,outputPath;
        for (int arg = 1; arg < argc; ++arg) {
            std::string option = argv[arg];
            if (option == "--fp16") fp16 = true;
            else if (option == "--int8") int8 = true;
            else if (option == "--uint8-a") {int8=true;unsignedA=true;}
            else if (option == "--owned-b") ownedB=true;
            else if (option == "--debug") debug=true;
            else if (option == "--conv1x1") conv1x1=true;
            else if (option == "--operands" && arg+1<argc) operandsFolder=argv[++arg];
            else if (option == "--save-output" && arg+1<argc) outputPath=argv[++arg];
            else if (option == "--shape" && arg + 3 < argc) {
                m = std::stoul(argv[++arg]); n = std::stoul(argv[++arg]); k = std::stoul(argv[++arg]);
            } else if (option == "--runs" && arg + 1 < argc) runs = std::stoul(argv[++arg]);
            else throw std::runtime_error("Usage: dml_probe.exe [--fp16|--int8] [--shape M N K] [--runs N]");
        }
        if ((fp16 && int8) || !m || !n || !k || !runs || runs > 64 || m > 1048576 || n > 4096 || k > 4096)
            throw std::runtime_error("Invalid precision, dimensions or run count");
        const bool smallCase = m == 2 && n == 2 && k == 2;
        if(debug){ComPtr<ID3D12Debug> layer;check(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)),"Debug interface");layer->EnableDebugLayer();}
        if(int8&&k>UINT(INT_MAX/(127*127)))throw std::runtime_error("Integer accumulator may overflow");
        if(conv1x1&&int8)throw std::runtime_error("Convolution probe currently supports floating point only");
        ComPtr<IDXGIFactory6> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create factory");
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT status = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate));
            if (status == DXGI_ERROR_NOT_FOUND) break;
            check(status, "Enumerate GPU");
            check(candidate->GetDesc1(&desc), "GPU description");
            if (desc.VendorId == 0x8086 && desc.DeviceId == 0xe20b) { adapter = candidate; break; }
        }
        if (!adapter) throw std::runtime_error("Intel Arc B580 adapter not found");
        ComPtr<ID3D12Device> device;
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12 device");
        ComPtr<ID3D12InfoQueue> diagnostics;if(debug)check(device.As(&diagnostics),"Debug diagnostics");
        ComPtr<IDMLDevice> dml;
        check(DMLCreateDevice(device.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml)), "DirectML device");
        bool types[3]{};
        DML_TENSOR_DATA_TYPE dataTypes[] = { DML_TENSOR_DATA_TYPE_FLOAT32, DML_TENSOR_DATA_TYPE_FLOAT16, DML_TENSOR_DATA_TYPE_INT8 };
        for (int i = 0; i < 3; ++i) {
            DML_FEATURE_QUERY_TENSOR_DATA_TYPE_SUPPORT query{ dataTypes[i] };
            DML_FEATURE_DATA_TENSOR_DATA_TYPE_SUPPORT supported{};
            check(dml->CheckFeatureSupport(DML_FEATURE_TENSOR_DATA_TYPE_SUPPORT, sizeof(query), &query,
                sizeof(supported), &supported), "Tensor capability");
            types[i] = supported.IsSupported != FALSE;
        }
        UINT aSizes[] = { 1, 1, m, k }, bSizes[] = { 1, 1, k, n }, outSizes[] = { 1, 1, m, n };
        const UINT64 inputElementBytes = int8 ? 1 : (fp16 ? 2 : 4);
        const UINT64 outputElementBytes = fp16 ? 2 : 4;
        const UINT64 aBytes = (UINT64(m) * k * inputElementBytes + 3) & ~UINT64(3);
        const UINT64 bBytes = (UINT64(k) * n * inputElementBytes + 3) & ~UINT64(3);
        const UINT64 bOffset = (aBytes + 15) & ~UINT64(15);
        const UINT64 zeroOffset=(bOffset+bBytes+15)&~UINT64(15);
        const UINT64 inputCapacity = unsignedA?zeroOffset+4:bOffset+bBytes;
        const UINT64 outputBytes = (UINT64(m) * n * outputElementBytes + 3) & ~UINT64(3);
        if (inputCapacity + outputBytes > 256ull * 1024 * 1024) throw std::runtime_error("Probe memory limit exceeded");
        DML_BUFFER_TENSOR_DESC tensorBuffer{ int8 ? DML_TENSOR_DATA_TYPE_INT8 :
            (fp16 ? DML_TENSOR_DATA_TYPE_FLOAT16 : DML_TENSOR_DATA_TYPE_FLOAT32),
            DML_TENSOR_FLAG_NONE, 4, aSizes, nullptr, aBytes, 0 };
        if(unsignedA)tensorBuffer.DataType=DML_TENSOR_DATA_TYPE_UINT8;
        DML_TENSOR_DESC tensor{ DML_TENSOR_TYPE_BUFFER, &tensorBuffer };
        DML_BUFFER_TENSOR_DESC bTensorBuffer = tensorBuffer;
        bTensorBuffer.Sizes = bSizes; bTensorBuffer.TotalTensorSizeInBytes = bBytes;
        if(int8)bTensorBuffer.DataType=DML_TENSOR_DATA_TYPE_INT8;
        if(ownedB)bTensorBuffer.Flags=DML_TENSOR_FLAG_OWNED_BY_DML;
        DML_TENSOR_DESC bTensor{ DML_TENSOR_TYPE_BUFFER, &bTensorBuffer };
        DML_BUFFER_TENSOR_DESC outputTensorBuffer = tensorBuffer;
        outputTensorBuffer.DataType = int8 ? DML_TENSOR_DATA_TYPE_INT32 : tensorBuffer.DataType;
        outputTensorBuffer.Sizes = outSizes;
        outputTensorBuffer.TotalTensorSizeInBytes = outputBytes;
        DML_TENSOR_DESC outputTensor{ DML_TENSOR_TYPE_BUFFER, &outputTensorBuffer };
        DML_GEMM_OPERATOR_DESC gemm{};
        gemm.ATensor = &tensor; gemm.BTensor = &bTensor; gemm.OutputTensor = &outputTensor;
        gemm.TransA = gemm.TransB = DML_MATRIX_TRANSFORM_NONE;
        gemm.Alpha = 1; gemm.Beta = 0;
        DML_OPERATOR_DESC opDesc{ DML_OPERATOR_GEMM, &gemm };
        DML_MATRIX_MULTIPLY_INTEGER_OPERATOR_DESC integer{};
        integer.ATensor = &tensor; integer.BTensor = &bTensor; integer.OutputTensor = &outputTensor;
        UINT zeroSizes[]={1,1,1,1};
        DML_BUFFER_TENSOR_DESC zeroBuffer{DML_TENSOR_DATA_TYPE_UINT8,DML_TENSOR_FLAG_NONE,4,zeroSizes,nullptr,4,0};
        DML_TENSOR_DESC zeroTensor{DML_TENSOR_TYPE_BUFFER,&zeroBuffer};
        if(unsignedA)integer.AZeroPointTensor=&zeroTensor;
        if (int8) opDesc = { DML_OPERATOR_MATRIX_MULTIPLY_INTEGER, &integer };
        // A(M,K)*B(K,N) is a pointwise convolution with NHWC activation/output strides.
        UINT h=static_cast<UINT>(sqrt(double(m)));while(m%h)--h;UINT w=m/h;
        UINT convA[]={1,k,h,w},convB[]={n,k,1,1},convOut[]={1,n,h,w};
        UINT strideA[]={m*k,1,w*k,k},strideB[]={1,n,n,n},strideOut[]={m*n,1,w*n,n};
        UINT spatialStride[]={1,1},dilation[]={1,1},padding[]={0,0};
        DML_CONVOLUTION_OPERATOR_DESC convolution{};
        if(conv1x1){
            tensorBuffer.Sizes=convA;tensorBuffer.Strides=strideA;bTensorBuffer.Sizes=convB;bTensorBuffer.Strides=strideB;
            outputTensorBuffer.Sizes=convOut;outputTensorBuffer.Strides=strideOut;
            convolution.InputTensor=&tensor;convolution.FilterTensor=&bTensor;convolution.OutputTensor=&outputTensor;
            convolution.Mode=DML_CONVOLUTION_MODE_CROSS_CORRELATION;convolution.Direction=DML_CONVOLUTION_DIRECTION_FORWARD;
            convolution.DimensionCount=2;convolution.Strides=spatialStride;convolution.Dilations=dilation;
            convolution.StartPadding=padding;convolution.EndPadding=padding;convolution.OutputPadding=padding;convolution.GroupCount=1;
            opDesc={DML_OPERATOR_CONVOLUTION,&convolution};
        }
        ComPtr<IDMLOperator> op;
        check(dml->CreateOperator(&opDesc, IID_PPV_ARGS(&op)), "Create GEMM");
        ComPtr<IDMLCompiledOperator> compiled;
        check(dml->CompileOperator(op.Get(), DML_EXECUTION_FLAG_NONE, IID_PPV_ARGS(&compiled)), "Compile GEMM");
        IDMLCompiledOperator* operators[] = { compiled.Get() };
        ComPtr<IDMLOperatorInitializer> initializer;
        check(dml->CreateOperatorInitializer(1, operators, IID_PPV_ARGS(&initializer)), "Create initializer");
        auto ip = initializer->GetBindingProperties();
        auto ep = compiled->GetBindingProperties();
        auto input = buffer(device.Get(), inputCapacity, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        auto output = buffer(device.Get(), outputBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto upload = buffer(device.Get(), inputCapacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = buffer(device.Get(), outputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        UINT64 temporarySize = std::max(ip.TemporaryResourceSize, ep.TemporaryResourceSize);
        ComPtr<ID3D12Resource> temporary, persistent;
        if (temporarySize) temporary = buffer(device.Get(), temporarySize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (ep.PersistentResourceSize) persistent = buffer(device.Get(), ep.PersistentResourceSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float operands[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        auto readFile=[&](const char* name,UINT64 bytes){
            std::ifstream file(operandsFolder+"/"+name,std::ios::binary|std::ios::ate);
            if(!file||file.tellg()!=std::streamoff(bytes))throw std::runtime_error("Invalid external operand size");
            std::vector<unsigned char> result(static_cast<size_t>(bytes));file.seekg(0);
            if(!file.read(reinterpret_cast<char*>(result.data()),std::streamsize(bytes)))throw std::runtime_error("Cannot read external operand");
            return result;
        };
        std::vector<unsigned char> rawA,rawB;
        if(!operandsFolder.empty()){rawA=readFile("a.bin",UINT64(m)*k*inputElementBytes);rawB=readFile("b.bin",UINT64(k)*n*inputElementBytes);}
        auto rawValue=[&](const std::vector<unsigned char>& raw,UINT64 index,bool activation)->float{
            if(int8&&unsignedA&&activation)return float(raw[static_cast<size_t>(index)])-128;
            if(int8){signed char value;memcpy(&value,raw.data()+index,1);return float(value);}
            if(fp16){DirectX::PackedVector::HALF value;memcpy(&value,raw.data()+index*2,2);return DirectX::PackedVector::XMConvertHalfToFloat(value);}
            float value;memcpy(&value,raw.data()+index*4,4);if(!std::isfinite(value))throw std::runtime_error("Non-finite operand");return value;
        };
        void* mapped = nullptr;
        D3D12_RANGE empty{ 0, 0 };
        check(upload->Map(0, &empty, &mapped), "Map test upload");
        auto aValue = [&](UINT row, UINT col) -> float {
            if(!rawA.empty())return rawValue(rawA,UINT64(row)*k+col,true);
            if (smallCase) return int8 && row == 0 && col == 1 ? -2.f : operands[row * 2 + col];
            int value = int((UINT64(row) * 13 + col * 7) % 7) - 3;
            return int8 ? float(value) : float(value) / 16.f;
        };
        auto bValue = [&](UINT row, UINT col) -> float {
            if(!rawB.empty())return rawValue(rawB,UINT64(row)*n+col,false);
            if (smallCase) return int8 && row == 1 && col == 0 ? -7.f : operands[4 + row * 2 + col];
            int value = int((UINT64(row) * 11 + col * 3) % 9) - 4;
            return int8 ? float(value) : float(value) / 32.f;
        };
        auto writeValue = [&](UINT64 byteOffset, float value,bool activation=false) {
            char* target = static_cast<char*>(mapped) + byteOffset;
            if(int8&&unsignedA&&activation){unsigned char v=static_cast<unsigned char>(int(value)+128);memcpy(target,&v,1);}
            else if (int8) { signed char v = static_cast<signed char>(value); memcpy(target, &v, 1); }
            else if (fp16) { auto v = DirectX::PackedVector::XMConvertFloatToHalf(value); memcpy(target, &v, 2); }
            else memcpy(target, &value, 4);
        };
        memset(mapped, 0, static_cast<size_t>(inputCapacity));
        for (UINT row = 0; row < m; ++row) for (UINT col = 0; col < k; ++col)
            writeValue((UINT64(row) * k + col) * inputElementBytes, aValue(row, col),true);
        for (UINT row = 0; row < k; ++row) for (UINT col = 0; col < n; ++col)
            writeValue(bOffset + (UINT64(row) * n + col) * inputElementBytes, bValue(row, col));
        if(unsignedA)static_cast<unsigned char*>(mapped)[zeroOffset]=128;
        upload->Unmap(0, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const UINT initCount = std::max(1u, ip.RequiredDescriptorCount);
        const UINT execCount = std::max(1u, ep.RequiredDescriptorCount);
        heapDesc.NumDescriptors = initCount + execCount;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Descriptor heap");
        DML_BINDING_TABLE_DESC tableDesc{ initializer.Get(), heap->GetCPUDescriptorHandleForHeapStart(),
            heap->GetGPUDescriptorHandleForHeapStart(), initCount };
        ComPtr<IDMLBindingTable> initTable, execTable;
        check(dml->CreateBindingTable(&tableDesc, IID_PPV_ARGS(&initTable)), "Initializer bindings");
        UINT increment = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        tableDesc.Dispatchable = compiled.Get(); tableDesc.SizeInDescriptors = execCount;
        tableDesc.CPUDescriptorHandle.ptr += SIZE_T(initCount) * increment;
        tableDesc.GPUDescriptorHandle.ptr += UINT64(initCount) * increment;
        check(dml->CreateBindingTable(&tableDesc, IID_PPV_ARGS(&execTable)), "Execution bindings");
        DML_BUFFER_BINDING scratchBinding{ temporary.Get(), 0, temporarySize };
        DML_BINDING_DESC scratchDesc{ DML_BINDING_TYPE_BUFFER, &scratchBinding };
        if (ip.TemporaryResourceSize) initTable->BindTemporaryResource(&scratchDesc);
        if (ep.TemporaryResourceSize) execTable->BindTemporaryResource(&scratchDesc);
        DML_BUFFER_BINDING persistentBinding{ persistent.Get(), 0, ep.PersistentResourceSize };
        DML_BINDING_DESC persistentDesc{ persistent ? DML_BINDING_TYPE_BUFFER : DML_BINDING_TYPE_NONE,
            persistent ? &persistentBinding : nullptr };
        initTable->BindOutputs(1, &persistentDesc);
        if (persistent) execTable->BindPersistentResource(&persistentDesc);
        DML_BUFFER_BINDING inputBindings[] = { { input.Get(), 0, aBytes }, { input.Get(), bOffset, bBytes } };
        DML_BINDING_DESC inputs[] = { { DML_BINDING_TYPE_BUFFER, &inputBindings[0] },
            { DML_BINDING_TYPE_BUFFER, &inputBindings[1] }, { DML_BINDING_TYPE_NONE, nullptr } };
        DML_BUFFER_BINDING ownedBuffers[4]{};
        if(ownedB){
            ownedBuffers[int8?2:1]=inputBindings[1];
            DML_BUFFER_ARRAY_BINDING array{int8?4u:3u,ownedBuffers};
            DML_BINDING_DESC initInput{DML_BINDING_TYPE_BUFFER_ARRAY,&array};initTable->BindInputs(1,&initInput);
            inputs[1]={DML_BINDING_TYPE_NONE,nullptr};
        }
        DML_BUFFER_BINDING zeroBinding{input.Get(),zeroOffset,4};
        DML_BINDING_DESC integerInputs[] = { inputs[0], { unsignedA?DML_BINDING_TYPE_BUFFER:DML_BINDING_TYPE_NONE,unsignedA?&zeroBinding:nullptr }, inputs[1], { DML_BINDING_TYPE_NONE, nullptr } };
        execTable->BindInputs(int8 ? 4 : 3, int8 ? integerInputs : inputs);
        DML_BUFFER_BINDING outputBinding{ output.Get(), 0, outputBytes };
        DML_BINDING_DESC outputDesc{ DML_BINDING_TYPE_BUFFER, &outputBinding };
        execTable->BindOutputs(1, &outputDesc);
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Command queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator)), "Command allocator");
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Command list");
        ComPtr<IDMLCommandRecorder> recorder;
        check(dml->CreateCommandRecorder(IID_PPV_ARGS(&recorder)), "Command recorder");
        D3D12_QUERY_HEAP_DESC queryDesc{};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queryDesc.Count = runs * 2;
        ComPtr<ID3D12QueryHeap> queries;
        check(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)), "GPU timestamp queries");
        auto timestamps = buffer(device.Get(), UINT64(runs) * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12DescriptorHeap* heaps[] = { heap.Get() }; list->SetDescriptorHeaps(1, heaps);
        list->CopyBufferRegion(input.Get(), 0, upload.Get(), 0, inputCapacity);
        transition(list.Get(), input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        recorder->RecordDispatch(list.Get(), initializer.Get(), initTable.Get());
        uavBarrier(list.Get());
        for (UINT warm = 0; warm < 64; ++warm) {
            recorder->RecordDispatch(list.Get(), compiled.Get(), execTable.Get()); uavBarrier(list.Get());
        }
        for (UINT run = 0; run < runs; ++run) {
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, run * 2);
            recorder->RecordDispatch(list.Get(), compiled.Get(), execTable.Get());
            uavBarrier(list.Get());
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, run * 2 + 1);
        }
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, runs * 2, timestamps.Get(), 0);
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outputBytes);
        check(list->Close(), "Close list");
        ComPtr<ID3D12Fence> fence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Cannot create completion event");
        ID3D12CommandList* lists[] = { list.Get() }; queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), 1), "Signal completion");
        check(fence->SetEventOnCompletion(1, event), "Completion event");
        if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) {
            // Submitted objects must stay alive while GPU work may still reference them.
            fprintf(stderr, "GPU completion timed out; exiting without destroying submitted resources\n");
            ExitProcess(2);
        }
        check(device->GetDeviceRemovedReason(), "Device status");
        if(diagnostics){for(UINT64 i=0;i<diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
            SIZE_T bytes=0;check(diagnostics->GetMessage(i,nullptr,&bytes),"Diagnostic size");std::vector<unsigned char> data(bytes);
            auto message=reinterpret_cast<D3D12_MESSAGE*>(data.data());check(diagnostics->GetMessage(i,message,&bytes),"Diagnostic message");
            if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)throw std::runtime_error(message->pDescription);
        }}
        D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(outputBytes) };
        check(readback->Map(0, &readRange, &mapped), "Map numerical result");
        auto readValue = [&](UINT64 index) -> double {
            if (int8) return double(static_cast<const int*>(mapped)[index]);
            if (fp16) return DirectX::PackedVector::XMConvertHalfToFloat(static_cast<const DirectX::PackedVector::HALF*>(mapped)[index]);
            return static_cast<const float*>(mapped)[index];
        };
        float values[4];
        for (UINT i = 0; i < 4; ++i) values[i] = float(readValue(std::min(UINT64(i), UINT64(m) * n - 1)));
        bool valid = true;
        const UINT64 elements = UINT64(m) * n;
        // Validate all small outputs, distributed samples of large benchmark matrices.
        const UINT sampleCount = static_cast<UINT>(std::min(UINT64(17), elements));
        float maxError = 0;
        for (UINT sample = 0; sample < sampleCount; ++sample) {
            UINT64 index = sampleCount == 1 ? 0 : UINT64(sample) * (elements - 1) / (sampleCount - 1);
            UINT row = static_cast<UINT>(index / n), col = static_cast<UINT>(index % n);
            double expected = 0;
            for (UINT inner = 0; inner < k; ++inner) expected += double(aValue(row, inner)) * bValue(inner, col);
            double actual = readValue(index);
            maxError = std::max(maxError, float(std::fabs(actual - expected)));
            const double tolerance=int8?0.0:(operandsFolder.empty()?(fp16?.01:1e-5):std::max(1e-5,std::fabs(expected)*(fp16?1e-3:1e-4)));
            valid = valid && std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
        }
        if(!outputPath.empty()){
            std::ofstream file(outputPath,std::ios::binary);file.write(static_cast<const char*>(mapped),std::streamsize(outputBytes));
            if(!file)throw std::runtime_error("Cannot save numerical output");
        }
        readback->Unmap(0, &empty);
        if (!valid) throw std::runtime_error("GEMM numerical verification failed");
        UINT64 frequency = 0;
        check(queue->GetTimestampFrequency(&frequency), "GPU timestamp frequency");
        D3D12_RANGE timestampRange{ 0, static_cast<SIZE_T>(runs) * 16 };
        check(timestamps->Map(0, &timestampRange, &mapped), "Read GPU timings");
        auto* ticks = static_cast<const UINT64*>(mapped);
        double gpuMs = 0;
        for (UINT run = 0; run < runs; ++run) gpuMs += double(ticks[run * 2 + 1] - ticks[run * 2]) * 1000 / frequency;
        timestamps->Unmap(0, &empty); gpuMs /= runs;
        printf("{\"device\":\"Intel Arc B580\",\"fp32_supported\":%s,\"fp16_supported\":%s,\"int8_supported\":%s,"
            "\"gemm_precision\":\"%s\",\"gemm_verified\":true,\"shape_mnk\":[%u,%u,%u],\"runs\":%u,\"gpu_mean_ms\":%.6f,"
            "\"validated_samples\":%u,\"max_abs_error\":%g,\"owned_b\":%s,\"conv1x1\":%s,\"persistent_bytes\":%llu,\"first_values\":[%g,%g,%g,%g],\"scope\":\"standalone DirectML test; repeated operands; no full model or XMX utilization claim\"}\n",
            types[0] ? "true" : "false", types[1] ? "true" : "false", types[2] ? "true" : "false",
            int8 ? (unsignedA?"uint8_int8_int32":"int8_int32") : (fp16 ? "fp16" : "fp32"), m, n, k, runs, gpuMs, sampleCount, maxError,
            ownedB?"true":"false",conv1x1?"true":"false",static_cast<unsigned long long>(ep.PersistentResourceSize),values[0], values[1], values[2], values[3]);
        CloseHandle(event); return 0;
    } catch (const std::exception& e) {
        if (event) CloseHandle(event);
        fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
