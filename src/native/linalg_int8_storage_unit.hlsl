#include <hlsl/dx/linalg.h>
StructuredBuffer<float> Source:register(t0);
RWByteAddressBuffer Scratch:register(u0);
cbuffer Shape:register(b0){uint Tokens;uint Channels;uint Count;uint Publish;}
using namespace dx::linalg;
groupshared float16_t SharedA[256],SharedB[512];
[WaveSize(16)] [numthreads(16,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex){
    for(uint i=thread;i<256;i+=16)SharedA[i]=float16_t(Source[group.z*Count+i]);
    for(uint i=thread;i<512;i+=16)SharedB[i]=float16_t(Source[group.z*Count+256+i]);
    GroupMemoryBarrierWithGroupSync();
    using FA=Matrix<ComponentType::F16,8,32,MatrixUse::A,MatrixScope::Wave>;
    using FB=Matrix<ComponentType::F16,32,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::I32,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    FA fa=FA::Load(SharedA,0,32,MatrixLayout::RowMajor);
    FB fb;
    if(Tokens==0)fb=FB::Load(SharedB,0,16,MatrixLayout::RowMajor);
    else fb=FB::Load(SharedB,0,32,MatrixLayout::ColMajor);
    Matrix<ComponentType::I8,8,32,MatrixUse::A,MatrixScope::Wave> a=fa.Cast<ComponentType::I8>();
    Matrix<ComponentType::I8,32,16,MatrixUse::B,MatrixScope::Wave> b=fb.Cast<ComponentType::I8>();
    uint stage=group.z*Publish*4+1024;
    a.Store(Scratch,stage,32,MatrixLayout::RowMajor);
    b.Store(Scratch,stage+256,16,MatrixLayout::RowMajor);
    DeviceMemoryBarrierWithGroupSync();
    a=Matrix<ComponentType::I8,8,32,MatrixUse::A,MatrixScope::Wave>::Load(Scratch,stage,32,MatrixLayout::RowMajor);
    b=Matrix<ComponentType::I8,32,16,MatrixUse::B,MatrixScope::Wave>::Load(Scratch,stage+256,16,MatrixLayout::RowMajor);
    C c=C::Splat(0);c.MultiplyAccumulate(a,b);
    for(uint e=0;e<c.Length();++e){uint2 xy=c.GetCoordinate(e);
        Scratch.Store((group.z*Publish+Tokens*128+xy.x*16+xy.y)*4,asuint(float(c.Get(e))));}
}
