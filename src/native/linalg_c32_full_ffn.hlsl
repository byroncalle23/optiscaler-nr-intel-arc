// Whole C32 FFN: published input -> expansion/gate -> contraction -> feed/pack.
#include "linalg_branch_compact.hlsl"
ByteAddressBuffer FfnCos:register(t3);
cbuffer WindowGeometry:register(b1){uint Width;uint Height;uint Top;uint Left;uint PaddedWidth;}

groupshared float16_t SharedInput[256],SharedGate[1024];
groupshared uint FullWaveCounter;
[WaveSize(16)]
[numthreads(32,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex){
    uint rt=group.x+group.y*4096;
    if(rt>=Words/8)return;
    if(thread==0)FullWaveCounter=0;
    GroupMemoryBarrierWithGroupSync();
    uint wave=0;if(WaveIsFirstLane())InterlockedAdd(FullWaveCounter,1,wave);wave=WaveReadLaneFirst(wave);
    uint lane=WaveGetLaneIndex();
    // Zero padded positions, disjoint from the logical pixels written below.
    for(uint i=lane;i<128;i+=16){
        uint packedPixel=rt*8+i/16,window=packedPixel/64,token=packedPixel%64;
        int y=int(window/(PaddedWidth/8)*8+token/8)-int(Top);
        int x=int(window%(PaddedWidth/8)*8+token%8)-int(Left);
        if(y<0||x<0||y>=int(Height)||x>=int(Width))Output.Store((packedPixel*32+wave*16+i%16)*4,0);
    }
    if(rt>=Pixels/8)return; // Uniform across the workgroup, before data barriers.
    for(uint i=thread;i<256;i+=32)SharedInput[i]=float16_t(publishE4(asfloat(ExpandWeights.Load((rt*256+i)*4))));
    GroupMemoryBarrierWithGroupSync();
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    // Each hidden column is computed once, across two waves / four iterations.
    [unroll]for(uint iteration=0;iteration<4;++iteration){
        uint nt=iteration*2+wave;C c=C::Splat(0.0f);
        [unroll]for(uint kt=0;kt<2;++kt){
            A a=A::Load(SharedInput,kt*16,32,MatrixLayout::RowMajor);
            B b=B::Load(Input,(nt*2+kt)*512,32,MatrixLayout::RowMajor);
            c.MultiplyAccumulate(a,b);
        }
        [unroll]for(uint i=0;i<8;++i)c.Set(i,float16_t(gate(float(c.Get(i)))));
        c.Store(SharedGate,nt*16,128,MatrixLayout::RowMajor);
    }
    GroupMemoryBarrierWithGroupSync();
    C c=C::Splat(0.0f);
    [unroll]for(uint kt=0;kt<8;++kt){
        A a=A::Load(SharedGate,kt*16,128,MatrixLayout::RowMajor);
        B b=B::Load(ProjectWeights,(wave*8+kt)*512,32,MatrixLayout::RowMajor);
        c.MultiplyAccumulate(a,b);
    }
    [unroll]for(uint i=0;i<8;++i){
        uint2 pos=c.GetCoordinate(i);uint pixel=rt*8+pos.x,channel=wave*16+pos.y,source=pixel*32+channel;
        precise float scaled=asfloat(ExpandWeights.Load(source*4))*asfloat(FfnCos.Load(channel*4));
        precise float feed=float(c.Get(i))+scaled;
        uint y=pixel/Width+Top,x=pixel%Width+Left,window=(y/8)*(PaddedWidth/8)+x/8,token=(y%8)*8+x%8;
        Compact.Store(source*4,asuint(feed));Output.Store(((window*64+token)*32+channel)*4,asuint(publishE4(feed)));
    }
}
