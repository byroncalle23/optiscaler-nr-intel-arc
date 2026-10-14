#pragma once
#include "dml_common.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifdef NR_PIX_MARKERS
#include <pix3.h>
#endif

// Optional diagnostics. Timestamps record on the inference list; map only after
// the caller's completion fence. No tensor/image readback or GPU CPU round-trip.
class NativeGpuProfile {
    struct Event {std::string path;UINT begin,end;};
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    std::vector<Event> events;
    std::vector<UINT> stack;
    UINT used=0,capacity;
public:
    explicit NativeGpuProfile(ID3D12Device* device,UINT maximum=262144):capacity(maximum){
        D3D12_QUERY_HEAP_DESC desc{};desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;desc.Count=capacity;
        check(device->CreateQueryHeap(&desc,IID_PPV_ARGS(&heap)),"NR profiling queries");
        readback=buffer(device,UINT64(capacity)*8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        events.reserve(capacity/2);stack.reserve(16);
    }
    void reset(){if(!stack.empty())throw std::runtime_error("Unclosed GPU profile range");events.clear();used=0;}
    UINT begin(ID3D12GraphicsCommandList* list,const char* name){
        if(used+2>=capacity)throw std::runtime_error("GPU profile query capacity exceeded");
        auto path=stack.empty()?std::string(name):events[stack.back()].path+"/"+name;
        UINT id=UINT(events.size());events.push_back({std::move(path),used++,~UINT(0)});stack.push_back(id);
        list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,events[id].begin);return id;
    }
    void end(ID3D12GraphicsCommandList* list,UINT id){
        if(stack.empty()||stack.back()!=id||used>=capacity)throw std::runtime_error("Invalid GPU profile nesting");
        events[id].end=used++;list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,events[id].end);stack.pop_back();
    }
    void resolve(ID3D12GraphicsCommandList* list){if(!stack.empty()||!used)throw std::runtime_error("Incomplete GPU profile");
        list->ResolveQueryData(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,used,readback.Get(),0);}
    struct Total {double ms=0;UINT count=0;};
    std::map<std::string,Total> readAfterFence(ID3D12CommandQueue* queue){
        UINT64 frequency;check(queue->GetTimestampFrequency(&frequency),"NR GPU timestamp frequency");
        void* mapped;D3D12_RANGE range{0,SIZE_T(used)*8};check(readback->Map(0,&range,&mapped),"NR profiling readback");
        auto times=static_cast<UINT64*>(mapped);std::map<std::string,Total> result;
        for(const auto& event:events){if(event.end==~UINT(0)||times[event.end]<times[event.begin])throw std::runtime_error("Invalid GPU profile timestamps");
            auto& row=result[event.path];row.ms+=double(times[event.end]-times[event.begin])*1000/double(frequency);++row.count;}
        D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);return result;
    }
};
class NativeProfileRange {
    NativeGpuProfile* profile;ID3D12GraphicsCommandList* list;UINT id=0;
#ifdef NR_PIX_MARKERS
    bool marked=false;
#endif
public:
    NativeProfileRange(NativeGpuProfile* profiler,ID3D12GraphicsCommandList* commandList,const char* name):profile(profiler),list(commandList){if(profile)id=profile->begin(list,name);
#ifdef NR_PIX_MARKERS
        static const bool capture=GetEnvironmentVariableA("NR_PIX_CAPTURE",nullptr,0)!=0;
        if(capture){PIXBeginEvent(list,PIX_COLOR_DEFAULT,"%s",name);marked=true;}
#endif
    }
    ~NativeProfileRange(){if(profile)profile->end(list,id);
#ifdef NR_PIX_MARKERS
        if(marked)PIXEndEvent(list);
#endif
    }
};
