#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer Weights:register(t0);
StructuredBuffer<float> Input:register(t1);
RWStructuredBuffer<float> Output:register(u0);
cbuffer Shape:register(b0){uint Rows;uint Columns;uint Inner;uint Unused;}
groupshared uint WaveCounter;
[WaveSize(16)]
[numthreads(32,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex){
    if(thread==0)WaveCounter=0;
    GroupMemoryBarrierWithGroupSync();
    uint wave=0;if(WaveIsFirstLane())InterlockedAdd(WaveCounter,1,wave);wave=WaveReadLaneFirst(wave);
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    precise float sum0[8],sum1[8];
    [unroll]for(uint i=0;i<8;++i){sum0[i]=0;sum1[i]=0;}
    uint rowBase=group.x*16+wave*8,columnBase=group.y*32;
    for(uint block=0;block<Inner/64;++block){
        C c0=C::Splat(0.0f),c1=C::Splat(0.0f);
        [unroll]for(uint part=0;part<4;++part){
            // Each lane populates its native A fragment directly. No shared
            // activation staging or group barriers inside the K loop.
            A a=A::Splat(0.0f);
            [unroll]for(uint i=0;i<8;++i){uint2 pos=a.GetCoordinate(i);uint row=rowBase+pos.x;
                if(row<Rows)a.Set(i,float16_t(Input[row*Inner+block*64+part*16+pos.y]));}
            B b0=B::Load(Weights,((columnBase/16)*(Inner/16)+block*4+part)*512,32,MatrixLayout::RowMajor);
            B b1=B::Load(Weights,((columnBase/16+1)*(Inner/16)+block*4+part)*512,32,MatrixLayout::RowMajor);
            c0.MultiplyAccumulate(a,b0);c1.MultiplyAccumulate(a,b1);
        }
        [unroll]for(uint i=0;i<8;++i){sum0[i]+=float(c0.Get(i));sum1[i]+=float(c1.Get(i));}
    }
    C coords=C::Splat(0.0f);
    [unroll]for(uint i=0;i<8;++i){uint2 pos=coords.GetCoordinate(i);uint row=rowBase+pos.x;
        if(row<Rows){Output[row*Columns+columnBase+pos.y]=sum0[i];Output[row*Columns+columnBase+16+pos.y]=sum1[i];}}
}
