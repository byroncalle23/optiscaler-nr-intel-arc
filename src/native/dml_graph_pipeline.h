#pragma once
#include "dml_endpoints.h"
#include "dml_window.h"
#include "dml_transition.h"
#include "dml_block.h"
#include <map>
#include <functional>
struct GraphNode {
    std::string name;
    std::unique_ptr<EndpointHarness> endpoint;
    std::unique_ptr<WindowHarness> window;
    std::unique_ptr<TransitionHarness> transition;
    std::unique_ptr<BlockHarness> bottleneck;
    NativeRuntime* runtime(){if(endpoint)return endpoint.get();if(window)return window.get();if(bottleneck)return bottleneck.get();return transition.get();}
    Tensor* output(){if(endpoint)return endpoint->resultTensor();if(window)return window->resultTensor();if(bottleneck)return bottleneck->resultTensor();return transition->resultTensor();}
    void record(){if(endpoint)endpoint->record();else if(window)window->record();else if(bottleneck)bottleneck->recordResident();else transition->record();}
};

class GraphPipeline {
    std::vector<std::unique_ptr<GraphNode>> nodes;
    std::shared_ptr<NativeContext> context;
    std::map<std::string,Tensor*> retained;
    Tensor* source;
#ifdef NR_COMPACT_GEOMETRY
    std::function<void(GraphNode&)> studyObserver;
#endif
public:
#ifdef NR_COMPACT_GEOMETRY
    void setStudyObserver(std::function<void(GraphNode&)> observer){studyObserver=std::move(observer);}
#endif
    GraphPipeline(const std::string& folder,std::shared_ptr<NativeContext> existing=nullptr,Tensor* input=nullptr,bool reuse=true):context(existing),source(input){
        std::ifstream manifest(nativePath(folder+"/graph.txt"));if(!manifest)throw std::runtime_error("Graph manifest missing");
        std::string type,name,skipName;UINT retain;
    while(manifest>>type>>name>>retain>>skipName){
        Tensor* skip=nullptr;if(skipName!="-"){auto entry=retained.find(skipName);if(entry==retained.end())throw std::runtime_error("Missing retained skip");skip=entry->second;}
        if(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos || retain>1)throw std::runtime_error("Invalid graph node name or retention flag");
        auto node=std::make_unique<GraphNode>();node->name=name;
        if(type=="endpoint")node->endpoint=std::make_unique<EndpointHarness>(folder+"/"+name,context,source,skip);
        else if(type=="window"){node->window=std::make_unique<WindowHarness>(folder+"/"+name,context,source,retain!=0);
            if(node->window->diagnosticIsolation())throw std::runtime_error("Disable stage isolation for the graph");}
        else if(type=="bottleneck")node->bottleneck=std::make_unique<BlockHarness>(folder+"/"+name,true,context,source);
        else if(type=="transition")node->transition=std::make_unique<TransitionHarness>(folder+"/"+name,context,source,skip);
        else throw std::runtime_error("Unknown graph node type");
        if(!context){context=node->runtime()->sharedContext();node->runtime()->constrainToGpuBudget();context->reuseScratch=reuse;}
        if(node->runtime()->sharedContext()!=context)throw std::runtime_error("Graph nodes must share their context");
        if(node->window && node->window->skipTensor())retained["skip_"+name]=node->window->skipTensor();
        source=node->output();nodes.push_back(std::move(node));
    }
    if(!manifest.eof() || nodes.size()!=77 || nodes.front()->name!="adapter" || nodes.back()->name!="head")throw std::runtime_error("Incomplete graph manifest");

    }
    std::shared_ptr<NativeContext> sharedContext()const{return context;}
    Tensor* resultTensor()const{return source;}
    auto& publicNodes(){return nodes;}
    void record(){for(auto& node:nodes){NativeProfileRange range(context->profiler.get(),context->cmd.Get(),node->name.c_str());node->record();
#ifdef NR_COMPACT_GEOMETRY
        if(studyObserver)studyObserver(*node);
#endif
    }}
};
