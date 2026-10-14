// Whole C32 FFN: published input -> expansion/gate -> contraction -> feed/pack.
#include "linalg_branch_compact.hlsl"
ByteAddressBuffer FfnCos:register(t3);
cbuffer WindowGeometry:register(b1){uint Width;uint Height;uint Top;uint Left;uint PaddedWidth;}

// Two 8-row wave tiles share input/gate allocation and optional immutable weights.
#ifndef NR_FFN_SHARED_WEIGHTS
#define NR_FFN_SHARED_WEIGHTS 0
#endif
groupshared float16_t SharedInput[512],SharedGate[2048];
#if NR_FFN_SHARED_WEIGHTS
groupshared float16_t SharedExpand[4096],SharedProject[4096];
#endif
groupshared uint FullWaveCounter;
[WaveSize(16)]
[numthreads(64,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex){
    uint rt=group.x+group.y*4096;
    if(rt>=Words/16)return;
    if(thread==0)FullWaveCounter=0;
    GroupMemoryBarrierWithGroupSync();
    uint wave=0;if(WaveIsFirstLane())InterlockedAdd(FullWaveCounter,1,wave);wave=WaveReadLaneFirst(wave);
    uint rowPart=wave/2,columnPart=wave%2;
    // All padded output cells have one owner; logical cells are written below.
    for(uint i=thread;i<512;i+=64){
        uint packedPixel=rt*16+i/32,window=packedPixel/64,token=packedPixel%64;
        int y=int(window/(PaddedWidth/8)*8+token/8)-int(Top);
        int x=int(window%(PaddedWidth/8)*8+token%8)-int(Left);
        if(y<0||x<0||y>=int(Height)||x>=int(Width))Output.Store((packedPixel*32+i%32)*4,0);
    }
    if(rt>=Pixels/16)return;
    for(uint i=thread;i<512;i+=64)SharedInput[i]=float16_t(publishE4(asfloat(ExpandWeights.Load((rt*512+i)*4))));
#if NR_FFN_SHARED_WEIGHTS
    for(uint word=thread;word<2048;word+=64){
        uint a=Input.Load(word*4),b=ProjectWeights.Load(word*4);
        SharedExpand[word*2]=float16_t(f16tof32(a&65535));SharedExpand[word*2+1]=float16_t(f16tof32(a>>16));
        SharedProject[word*2]=float16_t(f16tof32(b&65535));SharedProject[word*2+1]=float16_t(f16tof32(b>>16));
    }
#endif
    GroupMemoryBarrierWithGroupSync();
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    // Each hidden column is computed once, across two waves / four iterations.
    [unroll]for(uint iteration=0;iteration<4;++iteration){
        uint nt=iteration*2+columnPart;C c=C::Splat(0.0f);
        [unroll]for(uint kt=0;kt<2;++kt){
            A a=A::Load(SharedInput,rowPart*256+kt*16,32,MatrixLayout::RowMajor);
            #if NR_FFN_SHARED_WEIGHTS
            B b=B::Load(SharedExpand,(nt*2+kt)*256,16,MatrixLayout::RowMajor);
#else
            B b=B::Load(Input,(nt*2+kt)*512,32,MatrixLayout::RowMajor);
#endif
            c.MultiplyAccumulate(a,b);
        }
        [unroll]for(uint i=0;i<8;++i)c.Set(i,float16_t(gate(float(c.Get(i)))));
        c.Store(SharedGate,rowPart*1024+nt*16,128,MatrixLayout::RowMajor);
    }
    GroupMemoryBarrierWithGroupSync();
    C c=C::Splat(0.0f);
    [unroll]for(uint kt=0;kt<8;++kt){
        A a=A::Load(SharedGate,rowPart*1024+kt*16,128,MatrixLayout::RowMajor);
        #if NR_FFN_SHARED_WEIGHTS
        B b=B::Load(SharedProject,(columnPart*8+kt)*256,16,MatrixLayout::RowMajor);
#else
        B b=B::Load(ProjectWeights,(columnPart*8+kt)*512,32,MatrixLayout::RowMajor);
#endif
        c.MultiplyAccumulate(a,b);
    }
    [unroll]for(uint i=0;i<8;++i){
        uint2 pos=c.GetCoordinate(i);uint pixel=rt*16+rowPart*8+pos.x,channel=columnPart*16+pos.y,source=pixel*32+channel;
        precise float scaled=asfloat(ExpandWeights.Load(source*4))*asfloat(FfnCos.Load(channel*4));
        precise float feed=float(c.Get(i))+scaled;
        uint y=pixel/Width+Top,x=pixel%Width+Left,window=(y/8)*(PaddedWidth/8)+x/8,token=(y%8)*8+x%8;
        Compact.Store(source*4,asuint(feed));Output.Store(((window*64+token)*32+channel)*4,asuint(publishE4(feed)));
    }
}
