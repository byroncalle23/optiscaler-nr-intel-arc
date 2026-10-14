#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
StructuredBuffer<float> InputA:register(t0);
StructuredBuffer<float> InputB:register(t1);
RWStructuredBuffer<float> Output:register(u0);
cbuffer Shape:register(b0) { uint M;uint N;uint K;uint Groups;uint LaneStrideA;uint LaneStrideB;uint TransposeB;uint Lanes; }
groupshared float16_t SharedA[128],SharedB[256],Result[128];
[WaveSize(16)]
[numthreads(16,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex) {
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    uint lane=group.z/Groups,head=group.z%Groups;
    uint aBase=lane*LaneStrideA+head*M*K,bBase=lane*LaneStrideB+head*K*N;
    C c=C::Splat(0.0f);
    for(uint inner=0;inner<K;inner+=16){
        for(uint i=thread;i<128;i+=16){
            uint row=group.x*8+i/16,column=inner+i%16;
            float v=0;
            [branch]if(row<M&&column<K)v=InputA[aBase+row*K+column];
            SharedA[i]=float16_t(v);
        }
        for(uint j=thread;j<256;j+=16){
            uint row=inner+j/16,column=group.y*16+j%16;
            float v=0;
            [branch]if(row<K&&column<N)v=InputB[bBase+(TransposeB?column*K+row:row*N+column)];
            SharedB[j]=float16_t(v);
        }
        GroupMemoryBarrierWithGroupSync();
        A a=A::Load(SharedA,0,16,MatrixLayout::RowMajor);
        B b=B::Load(SharedB,0,16,MatrixLayout::RowMajor);
        c.MultiplyAccumulate(a,b);
        GroupMemoryBarrierWithGroupSync();
    }
    c.Store(Result,0,16,MatrixLayout::RowMajor);
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<128;i+=16){
        uint row=group.x*8+i/16,column=group.y*16+i%16;
        if(row<M&&column<N)Output[(group.z*M+row)*N+column]=float(Result[i]);
    }
}
