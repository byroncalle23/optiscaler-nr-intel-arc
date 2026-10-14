#include "dml_endpoints.h"
#include "dml_window.h"
#include "dml_transition.h"

struct EncoderNode {
    std::string name;
    std::unique_ptr<EndpointHarness> endpoint;
    std::unique_ptr<WindowHarness> window;
    std::unique_ptr<TransitionHarness> transition;
    NativeRuntime* runtime(){if(endpoint)return endpoint.get();if(window)return window.get();return transition.get();}
    Tensor* output(){if(endpoint)return endpoint->resultTensor();if(window)return window->resultTensor();return transition->resultTensor();}
    void record(){if(endpoint)endpoint->record();else if(window)window->record();else transition->record();}
};
static void saveEncoder(const std::string& path,const std::vector<float>& data){
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*4));
    if(!file)throw std::runtime_error("Cannot write encoder result");
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Usage: dml_encoder.exe fixture-directory");
    std::string folder(argv[1]);std::ifstream manifest(folder+"/graph.txt");if(!manifest)throw std::runtime_error("Encoder manifest missing");
    std::vector<std::unique_ptr<EncoderNode>> nodes;
    std::shared_ptr<NativeContext> context;
    Tensor* source=nullptr;std::string type,name;UINT retain;
    while(manifest>>type>>name>>retain){
        if(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos || retain>1)throw std::runtime_error("Invalid encoder node name or retention flag");
        auto node=std::make_unique<EncoderNode>();node->name=name;
        if(type=="endpoint")node->endpoint=std::make_unique<EndpointHarness>(folder+"/"+name,context,source);
        else if(type=="window"){node->window=std::make_unique<WindowHarness>(folder+"/"+name,context,source,retain!=0);
            if(node->window->diagnosticIsolation())throw std::runtime_error("Disable stage isolation for the encoder");}
        else if(type=="transition")node->transition=std::make_unique<TransitionHarness>(folder+"/"+name,context,source);
        else throw std::runtime_error("Unknown encoder node type");
        if(!context)context=node->runtime()->sharedContext();
        if(node->runtime()->sharedContext()!=context)throw std::runtime_error("Encoder nodes must share their context");
        source=node->output();nodes.push_back(std::move(node));
    }
    if(!manifest.eof() || nodes.size()!=38 || nodes.front()->name!="adapter" || nodes.back()->name!="down30")throw std::runtime_error("Incomplete encoder manifest");
    std::vector<float> first,last;
    for(int run=0;run<2;++run){for(auto& node:nodes)node->record();last=nodes.back()->runtime()->download(source);
        if(run==0)first=last;else if(memcmp(first.data(),last.data(),last.size()*4))throw std::runtime_error("Encoder output is not repeatable");}
    for(float value:last)if(!std::isfinite(value))throw std::runtime_error("Non-finite encoder output");
    saveEncoder(folder+"/native.bin",last);
    UINT skips=0;
    // All diagnostic downloads follow the complete encoder; no inference-stage reads.
    for(auto& node:nodes){saveEncoder(folder+"/native_"+node->name+".bin",node->runtime()->download(node->output()));
        if(node->window && node->window->skipTensor()){
            ++skips;saveEncoder(folder+"/native_skip_"+node->name+".bin",node->runtime()->download(node->window->skipTensor()));}}
    printf("{\"finite\":true,\"repeat_byte_identical\":true,\"runs\":2,\"nodes\":%zu,\"retained_skips\":%u,\"shared_device_and_list\":true,\"cpu_intermediate_reads\":0}\n",nodes.size(),skips);return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
