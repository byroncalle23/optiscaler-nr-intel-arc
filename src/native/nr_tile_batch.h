#pragma once
#include "dml_graph_pipeline.h"
#include <vector>
#include <stdexcept>

struct TileBatchPlan {
    struct Tile {unsigned x,y;};
    std::vector<std::vector<Tile>> batches;
    TileBatchPlan(unsigned width,unsigned height,unsigned batch=2,unsigned stride=224){
        if(!width||!height||(batch!=1&&batch!=2&&batch!=4)||!stride)throw std::runtime_error("Invalid experimental tile batch geometry");
        for(unsigned y=0;y<height;y+=stride)for(unsigned x=0;x<width;x+=stride){
            if(batches.empty()||batches.back().size()==batch)batches.emplace_back();
            batches.back().push_back({x,y});
        }
    }
};

// The graph's activations carry a leading tile dimension. Each GEMM broadcasts
// shared parameters or batches independent attention matrices across that dimension.
class BatchedGraph {
public:
    static std::unique_ptr<GraphPipeline> create(const std::string& folder,std::shared_ptr<NativeContext> context,Tensor* input){
        if((context->tileBatch!=2&&context->tileBatch!=4)||input->lanes!=context->tileBatch)throw std::runtime_error("BatchedGraph requires two or four independent lanes");
        return std::make_unique<GraphPipeline>(folder,context,input);
    }
};
