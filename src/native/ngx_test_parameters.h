#pragma once
#include "../../integration/optiscaler/NativeNrHook.h"
class FixtureNgxParameters : public NVSDK_NGX_Parameter {
public:
    ID3D12Resource* output=nullptr;
    bool voidOnly=false;
#define NR_TEST_PARAMETER(T) void Set(const char*,T)override{} NVSDK_NGX_Result Get(const char*,T*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NR_TEST_PARAMETER(unsigned long long)
    NR_TEST_PARAMETER(float)
    NR_TEST_PARAMETER(double)
    NR_TEST_PARAMETER(unsigned int)
    NR_TEST_PARAMETER(int)
    NR_TEST_PARAMETER(ID3D11Resource*)
#undef NR_TEST_PARAMETER
    void Set(const char*,ID3D12Resource*)override{}
    void Set(const char*,void*)override{}
    NVSDK_NGX_Result Get(const char* name,ID3D12Resource** value)const override{
        if(voidOnly||strcmp(name,NVSDK_NGX_Parameter_Output))return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *value=output;return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* name,void** value)const override{
        if(strcmp(name,NVSDK_NGX_Parameter_Output))return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *value=output;return NVSDK_NGX_Result_Success;
    }
    void Reset()override{}
};
