#include <hlsl/dx/linalg.h>
#include "block_ops.hlsl"
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
    if(group.x==0)fb=FB::Load(SharedB,0,16,MatrixLayout::RowMajor);
    else fb=FB::Load(SharedB,0,32,MatrixLayout::ColMajor);
    Matrix<ComponentType::I8,8,32,MatrixUse::A,MatrixScope::Wave> a=fa.Cast<ComponentType::I8>();
    Matrix<ComponentType::I8,32,16,MatrixUse::B,MatrixScope::Wave> b=fb.Cast<ComponentType::I8>();
    C c=C::Splat(0);c.MultiplyAccumulate(a,b);
    for(uint e=0;e<c.Length();++e){uint2 xy=c.GetCoordinate(e);
        Output[group.z*Publish+group.x*128+xy.x*16+xy.y]=float(c.Get(e));}
}
