#pragma once
#include "dml_common.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <bcrypt.h>

class NativeNrAssets {
    std::filesystem::path root;
    UINT geometry[4]{};
    static std::string hash(const std::filesystem::path& path){
        struct Hash {BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE value=nullptr;
            ~Hash(){if(value)BCryptDestroyHash(value);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}} hash;
        check(BCryptOpenAlgorithmProvider(&hash.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0),"Asset hash algorithm");
        check(BCryptCreateHash(hash.algorithm,&hash.value,nullptr,0,nullptr,0,0),"Asset hash create");
        std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("Asset file missing");
        std::vector<unsigned char> chunk(1024*1024);
        while(stream){stream.read(reinterpret_cast<char*>(chunk.data()),std::streamsize(chunk.size()));auto size=stream.gcount();
            if(size)check(BCryptHashData(hash.value,chunk.data(),ULONG(size),0),"Asset hash data");}
        if(!stream.eof())throw std::runtime_error("Asset read failed");
        unsigned char digest[32];check(BCryptFinishHash(hash.value,digest,32,0),"Asset hash finish");
        const char* hex="0123456789abcdef";std::string result;for(auto byte:digest){result+=hex[byte>>4];result+=hex[byte&15];}return result;
    }
public:
    explicit NativeNrAssets(std::filesystem::path directory):root(std::filesystem::canonical(directory)){
        std::ifstream manifest(root/"assets.txt");std::string word;UINT version,count;
        if(!(manifest>>word>>version)||word!="NR_NATIVE_ASSETS"||version!=1)throw std::runtime_error("Unsupported native asset package");
        if(!(manifest>>word)||word!="geometry")throw std::runtime_error("Package geometry missing");
        for(auto& extent:geometry)if(!(manifest>>extent)||!extent||extent>1920)throw std::runtime_error("Invalid package geometry");
        if(!(manifest>>word>>count)||word!="files"||!count||count>2048)throw std::runtime_error("Invalid package file count");
        std::set<std::string> paths;
        for(UINT i=0;i<count;++i){UINT64 size;std::string digest,name;
            if(!(manifest>>size>>digest>>name)||!size||size>256ull*1024*1024||digest.size()!=64||digest.find_first_not_of("0123456789abcdef")!=std::string::npos||
               name.empty()||name.front()=='/'||name.find("..")!=std::string::npos||name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/-.")!=std::string::npos||!paths.insert(name).second)
                throw std::runtime_error("Invalid package asset entry");
            auto path=std::filesystem::canonical(root/nativePath(name));
            auto relative=path.lexically_relative(root);if(relative.empty()||*relative.begin()=="..")throw std::runtime_error("Asset resolves outside package");
            if(std::filesystem::file_size(path)!=size||hash(path)!=digest)throw std::runtime_error("Asset integrity mismatch");
        }
        if(manifest>>word)throw std::runtime_error("Trailing asset manifest data");
        if(!manifest.eof()||!paths.count("graph.txt")||!paths.count("frame_input/shape.txt")||!paths.count("frame_input/noise.bin")||!paths.count("frame_output/shape.txt"))throw std::runtime_error("Incomplete asset package");
        std::ifstream shape(root/"frame_input/shape.txt");for(auto extent:geometry){UINT actual;if(!(shape>>actual)||actual!=extent)throw std::runtime_error("Package geometry mismatch");}
    }
    // Pass the owning DLL's HMODULE in production. nullptr selects the test EXE.
    static std::filesystem::path moduleDirectory(HMODULE module){
        std::vector<wchar_t> path(32768);DWORD count=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
        if(!count||count>=path.size())throw std::runtime_error("Native module path unavailable");
        auto folder=std::filesystem::path(std::wstring(path.data(),count)).parent_path();
        // Requiem shadows executable modules in this specific child directory.
        // Assets/config remain in the installation root and are still hash checked.
        if(folder.filename()==L"_storage_")folder=folder.parent_path();
        return folder;
    }
    static NativeNrAssets fromModule(HMODULE module){
        auto folder=moduleDirectory(module);
        bool global=GetPrivateProfileIntW(L"NativeNR",L"GlobalB1",0,(folder/L"native_nr.ini").c_str())==1;
#ifdef NR_GAME_MODES
        if(!global)throw std::runtime_error("NR modes require GlobalB1=1");
        return NativeNrAssets(folder/"native_nr_modes"/"assets-180");
#elif defined(NR_GAME_COMPACT320)
        if(!global)throw std::runtime_error("Compact320 test needs GlobalB1=1");
        return NativeNrAssets(folder/"native_nr_compact320");
#elif defined(NR_GAME_COMPACT512)
        if(!global)throw std::runtime_error("Compact512 test needs GlobalB1=1");
        return NativeNrAssets(folder/"native_nr_compact512");
#else
        return NativeNrAssets(folder/(global?"native_nr_global360":"native_nr_assets"));
#endif
    }
    std::string folder()const{auto utf8=root.u8string();return {reinterpret_cast<const char*>(utf8.data()),utf8.size()};}
    UINT width()const{return geometry[1];}
    UINT height()const{return geometry[0];}
};
