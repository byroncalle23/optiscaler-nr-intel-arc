#include "dml_texture.h"
int main(int argc,char** argv){try{
    if(argc<2)throw std::runtime_error("Usage: dml_texture.exe fixture-directory [--format rgba32f|rgba16f|rgba8] [--store]");
    std::string formatName="rgba32f";bool store=false,packed=false,tiles=false,global=false,edge=false;UINT modelWidth=0,modelHeight=0,workWidth=0,workHeight=0;
    for(int i=2;i<argc;++i){std::string option=argv[i];if(option=="--format"&&i+1<argc){formatName=argv[++i];packed=true;}else if(option=="--model"&&i+2<argc){modelWidth=UINT(std::stoul(argv[++i]));modelHeight=UINT(std::stoul(argv[++i]));}else if(option=="--processing"&&i+2<argc){workWidth=UINT(std::stoul(argv[++i]));workHeight=UINT(std::stoul(argv[++i]));}else if(option=="--store")store=true;else if(option=="--tiles")tiles=true;else if(option=="--global")global=true;else if(option=="--edge-guided")edge=true;else throw std::runtime_error("Invalid texture option");}
    DXGI_FORMAT format=formatName=="rgba32f"?DXGI_FORMAT_R32G32B32A32_FLOAT:formatName=="rgba16f"?DXGI_FORMAT_R16G16B16A16_FLOAT:formatName=="rgba8"?DXGI_FORMAT_R8G8B8A8_UNORM:formatName=="rgb10a2"?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_UNKNOWN;
    std::string folder=argv[1];UINT width,height;std::ifstream config(folder+"/shape.txt");
    if(!(config>>width>>height))throw std::runtime_error("Invalid texture roundtrip geometry");
    TextureHarness harness(width,height,nullptr,format,nullptr,nullptr,modelWidth,modelHeight,tiles,global);
    if(edge)harness.edgeGuidance(true);
    if(workWidth)harness.setProcessingExtent(workWidth,workHeight);
    if(packed)harness.uploadPacked(folder+"/colour_texture.bin");else harness.upload(folder+"/colour.bin");
    Tensor* storeInput=store||tiles?harness.loadRgbForTest(folder):nullptr;
    std::vector<float> first,last;
    for(int run=0;run<2;++run){
        if(tiles){harness.clearTiles();for(UINT y=0;y<harness.workHeight();y+=harness.tileStride())for(UINT x=0;x<harness.workWidth();x+=harness.tileStride()){
            harness.selectTile(x,y);harness.recordInput();if(!store)harness.copyColourForTest(storeInput);harness.accumulateTile(storeInput);}
        }else if(!store||modelWidth)harness.recordInput();
        harness.recordOutput(storeInput?storeInput:harness.colourTensor());last=harness.downloadTexture(folder+"/native_texture_packed.bin");
        if(run==0)first=last;else if(memcmp(first.data(),last.data(),last.size()*4))throw std::runtime_error("Texture result not repeatable");}
    std::ofstream file(folder+"/native.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(last.data()),std::streamsize(last.size()*4));
    if(!file)throw std::runtime_error("Texture result write failed");
    if(!store){auto loaded=harness.download(harness.colourTensor());std::ofstream colourFile(folder+"/native_colour.bin",std::ios::binary);colourFile.write(reinterpret_cast<const char*>(loaded.data()),std::streamsize(loaded.size()*4));if(!colourFile)throw std::runtime_error("Colour result write failed");}
    auto diagnostic=harness.sharedContext()->diagnostics;
    if(diagnostic){for(UINT64 i=0;i<diagnostic->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes=0;check(diagnostic->GetMessage(i,nullptr,&bytes),"Texture debug size");std::vector<char> storage(bytes);auto m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(diagnostic->GetMessage(i,m,&bytes),"Texture debug message");if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(m->pDescription);}}
    printf("{\"repeat_byte_identical\":true,\"format\":\"%s\",\"store_only\":%s,\"edge_guided\":%s}\n",formatName.c_str(),store?"true":"false",edge?"true":"false");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
