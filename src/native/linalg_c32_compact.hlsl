// Share the validated root layout, IEEE rounding, gate and publish/pack helpers.
#include "linalg_branch_compact.hlsl"

[numthreads(128,1,1)]
void PackC32Weights(uint3 id:SV_DispatchThreadID){
    uint word=id.x;if(word>=2048)return;
    uint nCount=Groups,kCount=4096/nCount,kTiles=kCount/16;
    uint h=word*2,tile=h/256,local=h%256;
    uint k=(tile%kTiles)*16+local/16,n=(tile/kTiles)*16+local%16;
    uint source=k*nCount+n;
    Compact.Store(word*4,f32tof16(asfloat(Input.Load(source*4)))|(f32tof16(asfloat(Input.Load((source+1)*4)))<<16));
}

[WaveSize(16)]
[numthreads(16,1,1)]
void ExpandC32Gate(uint3 group:SV_GroupID){
    uint rt=group.x+(group.y/8)*4096,nt=group.y%8;
    if(rt>=Pixels/8)return;
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    C c=C::Splat(0.0f);
    [unroll]for(uint kt=0;kt<2;++kt){
        A a=A::Load(Input,rt*512+kt*256,32,MatrixLayout::RowMajor);
        B b=B::Load(ExpandWeights,(nt*2+kt)*512,32,MatrixLayout::RowMajor);
        c.MultiplyAccumulate(a,b);
    }
    [unroll]for(uint i=0;i<8;++i)c.Set(i,float16_t(gate(float(c.Get(i)))));
    c.Store(Compact,rt*2048+nt*256,32,MatrixLayout::RowMajor);
}

#if NR_C32_FEED_FUSED
ByteAddressBuffer FfnCos:register(t3);
cbuffer WindowGeometry:register(b1){uint Width;uint Height;uint Top;uint Left;uint PaddedWidth;}
#endif
groupshared float16_t C32Partials[512];
[WaveSize(16)]
[numthreads(16,1,1)]
void ProjectC32(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint rt=group.x+(group.y/2)*4096,nt=group.y%2;
#if NR_C32_FEED_FUSED
    if(rt>=Words/8)return;
    for(uint i=lane;i<128;i+=16){
        uint packedPixel=rt*8+i/16,window=packedPixel/64,token=packedPixel%64;
        int y=int(window/(PaddedWidth/8)*8+token/8)-int(Top);
        int x=int(window%(PaddedWidth/8)*8+token%8)-int(Left);
        if(y<0||x<0||y>=int(Height)||x>=int(Width))Output.Store((packedPixel*32+nt*16+i%16)*4,0);
    }
    if(rt>=Pixels/8)return;
#else
    if(rt>=Pixels/8)return;
#endif
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
#if NR_C32_PARTIALS
    [unroll]for(uint chunk=0;chunk<4;++chunk){
        C c=C::Splat(0.0f);
        [unroll]for(uint part=0;part<2;++part){
            uint kt=chunk*2+part;
            A a=A::Load(Input,rt*2048+kt*256,32,MatrixLayout::RowMajor);
            B b=B::Load(ProjectWeights,(nt*8+kt)*512,32,MatrixLayout::RowMajor);
            c.MultiplyAccumulate(a,b);
        }
        c.Store(C32Partials,chunk*128,16,MatrixLayout::RowMajor);
    }
#else
    C c=C::Splat(0.0f);
    [unroll]for(uint kt=0;kt<8;++kt){
        A a=A::Load(Input,rt*2048+kt*256,32,MatrixLayout::RowMajor);
        B b=B::Load(ProjectWeights,(nt*8+kt)*512,32,MatrixLayout::RowMajor);
        c.MultiplyAccumulate(a,b);
    }
    c.Store(C32Partials,0,16,MatrixLayout::RowMajor);
#endif
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<128;i+=16){
        precise float value=float(C32Partials[i]);
#if NR_C32_PARTIALS
        [unroll]for(uint chunk=1;chunk<4;++chunk)value+=float(C32Partials[chunk*128+i]);
#endif
        // C32's original contraction is unrounded FP32 at this boundary.
        // Do not insert E4 publication or round the sum back to FP16.
#if NR_C32_FEED_FUSED
        uint pixel=rt*8+i/16,channel=nt*16+i%16,source=pixel*32+channel;
        precise float scaled=asfloat(ExpandWeights.Load(source*4))*asfloat(FfnCos.Load(channel*4));
        precise float feed=value+scaled;
        uint y=pixel/Width+Top,x=pixel%Width+Left;
        uint window=(y/8)*(PaddedWidth/8)+x/8,token=(y%8)*8+x%8;
        Compact.Store(source*4,asuint(feed));
        Output.Store(((window*64+token)*32+channel)*4,asuint(publishE4(feed)));
#else
        Output.Store(((rt*8+i/16)*32+nt*16+i%16)*4,asuint(value));
#endif
    }
}
