#pragma once
#include "dml_common.h"
#include "embedded_shaders.h"
#include <d3dcompiler.h>
#include <map>
#include <mutex>

// Compile only during setup, from sources embedded in the binary. This cache
// contains device-independent DXBC; PSOs still belong to the supplied device.
inline ComPtr<ID3DBlob> compileNativeShader(const char* name,const char* entry,const D3D_SHADER_MACRO* macros=nullptr){
    if(!name||!entry)throw std::runtime_error("Missing embedded shader name/entry");
    const EmbeddedNativeShader* source=nullptr;
    for(auto& shader:embeddedNativeShaders)if(shader.name==name){source=&shader;break;}
    if(!source)throw std::runtime_error("Unknown embedded shader");
    std::string key=std::string(name)+"\n"+entry;
    if(macros)for(auto p=macros;p->Name;++p){if(!p->Definition)throw std::runtime_error("Shader macro definition missing");key+="\n"+std::string(p->Name)+"="+p->Definition;}
    static std::mutex mutex;static std::map<std::string,ComPtr<ID3DBlob>> cache;
    std::lock_guard<std::mutex> lock(mutex);auto hit=cache.find(key);if(hit!=cache.end())return hit->second;
    ComPtr<ID3DBlob> blob,error;
    auto hr=D3DCompile(source->source.data(),source->source.size(),name,macros,nullptr,entry,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
    if(FAILED(hr)&&error)fprintf(stderr,"%s\n",static_cast<char*>(error->GetBufferPointer()));check(hr,entry);
    cache.emplace(key,blob);return blob;
}
