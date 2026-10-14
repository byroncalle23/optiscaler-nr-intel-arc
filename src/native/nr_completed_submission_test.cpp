#define NOMINMAX
#include "nr_submission.h"
#include <cstdio>
struct ProbeSession {
    ComPtr<ID3D12Fence> fence;UINT64 value=0;
    void notifySubmitted(ID3D12Fence* f,UINT64 v){fence=f;value=v;}
    void cancelUnsubmitted(){}
};
// COM alias fixture for the integration's owned-reference native resolver.
class NativeAlias : public IUnknown {
    ULONG refs=1;
public:
    ComPtr<IUnknown> base;
    explicit NativeAlias(IUnknown* value):base(value){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto count=--refs;if(!count)delete this;return count;}
};
static NativeAlias* aliasFixture=nullptr;
class CommandAlias : public ID3D12CommandList {
    ULONG refs=1;
public:
    ComPtr<ID3D12CommandList> base;
    explicit CommandAlias(ID3D12CommandList* value):base(value){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;
        if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(ID3D12Object)&&iid!=__uuidof(ID3D12DeviceChild)&&iid!=__uuidof(ID3D12CommandList))return E_NOINTERFACE;
        *out=static_cast<ID3D12CommandList*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto count=--refs;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g,UINT* n,void* p)override{return base->GetPrivateData(g,n,p);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g,UINT n,const void* p)override{return base->SetPrivateData(g,n,p);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,const IUnknown* p)override{return base->SetPrivateDataInterface(g,p);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name)override{return base->SetName(name);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** out)override{return base->GetDevice(iid,out);}
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType()override{return base->GetType();}
};
static CommandAlias* listAliasFixture=nullptr;
static HRESULT resolveAlias(IUnknown* value,IUnknown** out){if(!out)return E_POINTER;*out=nullptr;
    IUnknown* base=value==aliasFixture?aliasFixture->base.Get():value;
    if(value==listAliasFixture)base=listAliasFixture->base.Get();base->AddRef();*out=base;return S_OK;}
int main(int argc,char**){try{
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"Device");
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");
    ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"List");check(list->Close(),"Close");
    NativeNrSubmissions submissions(device.Get());auto session=std::make_shared<ProbeSession>();
    ComPtr<NativeAlias> alias;alias.Attach(new NativeAlias(device.Get()));aliasFixture=alias.Get();nrNativeInterfaceResolver.store(resolveAlias);
    if(nativeObjectIdentity(alias.Get()).Get()!=nativeObjectIdentity(device.Get()).Get())throw std::runtime_error("Proxy native identity mismatch");
    // Legacy D3D12CreateDevice returns a singleton for the same adapter. WARP
    // supplies a genuinely distinct device for the rejection regression.
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Test factory");ComPtr<IDXGIAdapter> warp;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP adapter");
    ComPtr<ID3D12Device> other;check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&other)),"Distinct WARP device");
    if(sameNativeDevice(device.Get(),other.Get()))throw std::runtime_error("Distinct devices incorrectly equivalent");
    ComPtr<ID3D12CommandAllocator> otherAllocator;check(other->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&otherAllocator)),"Other allocator");
    ComPtr<ID3D12GraphicsCommandList> otherList;check(other->CreateCommandList(0,q.Type,otherAllocator.Get(),nullptr,IID_PPV_ARGS(&otherList)),"Other list");check(otherList->Close(),"Other close");
    bool otherRejected=false;try{submissions.track(otherList.Get(),session);}catch(const std::exception& e){otherRejected=std::string(e.what())=="Submission device mismatch";}
    if(!otherRejected||submissions.recordedCount()!=0)throw std::runtime_error("Cross-device recording was not rejected before ownership");
    puts("NATIVE_PROXY_IDENTITY_AND_DISTINCT_DEVICE_REJECTION_PASS");
    ComPtr<ID3D12Fence> gate;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"Gate");
    check(queue->Wait(gate.Get(),1),"Hold actual pending GPU work");
    ComPtr<CommandAlias> listAlias;listAlias.Attach(new CommandAlias(list.Get()));listAliasFixture=listAlias.Get();
    submissions.track(listAlias.Get(),session);ID3D12CommandList* lists[]={list.Get()};
    if(!submissions.hasRecorded(1,lists)||!submissions.contains(list.Get(),session.get()))throw std::runtime_error("Proxy/native submission identity lost");
    submissions.execute(queue.Get(),1,lists,[&]{queue->ExecuteCommandLists(1,lists);});
    bool pendingRejected=false;try{submissions.track(list.Get(),session);}catch(const std::exception&){pendingRejected=true;}
    check(gate->Signal(1),"Release held work");
    if(!pendingRejected)throw std::runtime_error("Unfinished GPU owner incorrectly reusable");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Event");
    check(session->fence->SetEventOnCompletion(session->value,event),"Fence");auto waited=WaitForSingleObject(event,10000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("GPU wait");
    // GPU finished, but the caller intentionally has NOT called collect().
    if(submissions.inFlightCount()!=1)throw std::runtime_error("Missing stale registry fixture");
    bool blocked=false;try{submissions.track(list.Get(),session);}catch(const std::exception&){blocked=true;}
    if(argc>1){submissions.collect();if(!blocked)throw std::runtime_error("Old defect not reproduced");puts("STALE_COMPLETED_SUBMISSION_REPRODUCED");return 0;}
    if(blocked){submissions.collect();throw std::runtime_error("Completed GPU work incorrectly blocked next capture");}
    if(submissions.inFlightCount()!=0||submissions.recordedCount()!=1)throw std::runtime_error("Completed work not collected at recording boundary");
    submissions.discard(listAlias.Get());nrNativeInterfaceResolver.store(nullptr);aliasFixture=nullptr;listAliasFixture=nullptr;puts("PROXY_RECORD_NATIVE_SUBMIT_PROXY_DISCARD_PASS");puts("COMPLETED_SUBMISSION_REUSE_WITHOUT_CALLER_COLLECT_PASS");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
