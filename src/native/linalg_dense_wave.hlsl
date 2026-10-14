#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer Weights:register(t0);
StructuredBuffer<float> Input:register(t1);
RWStructuredBuffer<float> Output:register(u0);
cbuffer Shape:register(b0){uint Rows;uint Columns;uint Inner;uint Unused;}
groupshared float16_t InputTile[512];
groupshared uint WaveCounter;
[WaveSize(16)]
[numthreads(64,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex){
    if(thread==0)WaveCounter=0;
    GroupMemoryBarrierWithGroupSync();
    uint wave=0;if(WaveIsFirstLane())InterlockedAdd(WaveCounter,1,wave);wave=WaveReadLaneFirst(wave);
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    precise float sum[8];[unroll]for(uint i=0;i<8;++i)sum[i]=0.0f;
    uint columnTile=group.y*4+wave;
    for(uint block=0;block<Inner/64;++block){
        for(uint i=thread;i<512;i+=64){
            uint row=group.x*8+i/64;
            // No early return: tail rows still participate in both barriers.
            [branch]if(row<Rows)InputTile[i]=float16_t(Input[row*Inner+block*64+i%64]);
            else InputTile[i]=float16_t(0.0f);
        }
        GroupMemoryBarrierWithGroupSync();
        C c=C::Splat(0.0f);
        [unroll]for(uint part=0;part<4;++part){
            A a=A::Load(InputTile,part*16,64,MatrixLayout::RowMajor);
            B b=B::Load(Weights,(columnTile*(Inner/16)+block*4+part)*512,32,MatrixLayout::RowMajor);
            c.MultiplyAccumulate(a,b);
        }
        // Preserve K64 FP16 partial / FP32 outer sum boundaries. Internal MMA
        // association differs from Thread Multiply; real-model RGB must be tested.
        [unroll]for(uint i=0;i<8;++i)sum[i]+=float(c.Get(i));
        GroupMemoryBarrierWithGroupSync();
    }
    C coordinate=C::Splat(0.0f);
    [unroll]for(uint i=0;i<8;++i){
        uint2 pos=coordinate.GetCoordinate(i);uint row=group.x*8+pos.x,column=columnTile*16+pos.y;
        if(row<Rows)Output[row*Columns+column]=sum[i];
    }
}
