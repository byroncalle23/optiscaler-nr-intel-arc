#include "../../integration/optiscaler/NativeNrLifecycle.h"
#include <stdexcept>
#include <cstdio>
static void require(bool value){if(!value)throw std::runtime_error("Lifecycle contract failed");}
int main(){try{
    UINT forwarded=0,resets=0,retired=0,shutdowns=0;unsigned lastHandle=0;
    auto forward=[](void* user){++*static_cast<UINT*>(user);};
    require(NativeNrLifecycle::Execute(nullptr,0,nullptr,forward,&forwarded)&&forwarded==1);
    struct Counts {UINT* resets;UINT* retired;UINT* shutdowns;unsigned* handle;} counts{&resets,&retired,&shutdowns,&lastHandle};
    auto provider=std::make_shared<NativeNrLifecycle::Provider>();provider->user=&counts;
    provider->reset=[](void* user,ID3D12GraphicsCommandList*){++*static_cast<Counts*>(user)->resets;};
    provider->retire=[](void* user,unsigned handle){auto& c=*static_cast<Counts*>(user);++*c.retired;*c.handle=handle;};
    provider->shutdown=[](void* user){++*static_cast<Counts*>(user)->shutdowns;};
    NativeNrLifecycle::Register(provider);
    require(NativeNrLifecycle::Reset(nullptr,E_FAIL)&&resets==0);
    require(NativeNrLifecycle::Reset(nullptr,S_OK)&&resets==1);
    require(NativeNrLifecycle::Retire(17)&&retired==1&&lastHandle==17);
    require(NativeNrLifecycle::Shutdown()&&shutdowns==1);
    require(NativeNrLifecycle::Recorded(17,nullptr));
    auto recordingFailure=std::make_shared<NativeNrLifecycle::Provider>(*provider);
    recordingFailure->recorded=[](void*,unsigned,ID3D12GraphicsCommandList*)->bool{throw std::runtime_error("recording observer");};
    NativeNrLifecycle::Register(recordingFailure);require(!NativeNrLifecycle::Recorded(17,nullptr));
    // Publish new immutable providers; don't mutate a registered snapshot.
    auto publish=[&](auto execute){auto value=std::make_shared<NativeNrLifecycle::Provider>(*provider);value->execute=execute;NativeNrLifecycle::Register(value);};
    publish(+[](void*,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,NativeNrLifecycle::Forward,void*)->bool{throw std::runtime_error("before");});
    require(!NativeNrLifecycle::Execute(nullptr,0,nullptr,forward,&forwarded)&&forwarded==2);
    publish(+[](void*,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,NativeNrLifecycle::Forward f,void* u)->bool{f(u);throw std::runtime_error("after");});
    require(!NativeNrLifecycle::Execute(nullptr,0,nullptr,forward,&forwarded)&&forwarded==3);
    publish(+[](void*,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,NativeNrLifecycle::Forward f,void* u){f(u);f(u);return true;});
    require(!NativeNrLifecycle::Execute(nullptr,0,nullptr,forward,&forwarded)&&forwarded==4);
    publish(+[](void*,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*,NativeNrLifecycle::Forward,void*){return true;});
    require(NativeNrLifecycle::Execute(nullptr,0,nullptr,forward,&forwarded)&&forwarded==5);
    auto retained=NativeNrLifecycle::Snapshot();std::weak_ptr<const NativeNrLifecycle::Provider> weak=retained;
    NativeNrLifecycle::Register(nullptr);require(!weak.expired());retained.reset();require(weak.expired());
    require(!NativeNrLifecycle::Execute(nullptr,0,nullptr,nullptr,nullptr));
    printf("{\"passed\":true,\"original_once\":true,\"before_after_failure\":true,\"duplicate_forward_guard\":true,\"failed_reset_retained\":true,\"provider_snapshot_retained\":true,\"retire_shutdown_verified\":true}\n");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
