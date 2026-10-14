#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer InputA:register(t0);
ByteAddressBuffer InputB:register(t1);
RWStructuredBuffer<float> Output:register(u0);
cbuffer Shape:register(b0) { uint M;uint N;uint K;uint MP;uint NP;uint KP;uint Groups;uint Lanes; }
groupshared float16_t Result[128];
[WaveSize(16)]
[numthreads(16,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex) {
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    C c=C::Splat(0.0f);
    for(uint inner=0;inner<KP;inner+=16){
        uint aOffset=(group.z*MP*KP+group.x*8*KP+inner)*2;
        uint bOffset=(group.z*KP*NP+inner*NP+group.y*16)*2;
        A a=A::Load(InputA,aOffset,KP*2,MatrixLayout::RowMajor);
        B b=B::Load(InputB,bOffset,NP*2,MatrixLayout::RowMajor);
        c.MultiplyAccumulate(a,b);
    }
    // Opaque matrix distribution is published in row order, never guessed per lane.
    c.Store(Result,0,16,MatrixLayout::RowMajor);
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<128;i+=16){
        uint row=group.x*8+i/16,column=group.y*16+i%16;
        if(row<M&&column<N)Output[(group.z*M+row)*N+column]=float(Result[i]);
    }
}
