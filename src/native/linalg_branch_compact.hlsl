#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer Input:register(t0),ExpandWeights:register(t1),ProjectWeights:register(t2);
RWByteAddressBuffer Compact:register(u0),Output:register(u1);
cbuffer Settings:register(b0){uint Pixels;uint Heads;uint Groups;uint Words;}
float halfRound(float x){
    uint bits=asuint(x),sign=bits&0x80000000,magnitude=bits&0x7fffffff;
    if(magnitude>=0x7f800000)return x;
    if(magnitude>=0x477ff000)return asfloat(sign|0x7f800000);
    if(magnitude>=0x38800000){uint rounded=(magnitude+4095+((magnitude>>13)&1))&0xffffe000;return asfloat(sign|rounded);}
    float scaled=asfloat(magnitude)*16777216.0,lo=floor(scaled),f=scaled-lo;
    float v=lo+((f>.5||(f==.5&&(uint(lo)&1)))?1.0:0.0);return asfloat(sign|asuint(v*5.960464477539063e-8));
}
float publishE4(float x){
    float magnitude=min(abs(x),448.0);uint exponent=max((asuint(magnitude)>>23)&255,121)-3;
    float step=asfloat(exponent<<23),scaled=magnitude*asfloat((254-exponent)<<23),lo=floor(scaled),f=scaled-lo;
    float v=lo+((f>.5||(f==.5&&(uint(lo)&1)))?1:0);return x<0?-v*step:v*step;
}
float gate(float value){
    precise float x=halfRound(value),z=clamp(x,-4.0,4.0),a=abs(z)*-0.055908203125;
    a=halfRound(a+0.447265625);precise float b=a*z;b=halfRound(b+0.89453125);return publishE4(halfRound(x*b));
}
[numthreads(128,1,1)]
void PackWeights(uint3 id:SV_DispatchThreadID){
    uint word=id.x;if(word>=Groups*512)return;
    uint halfIndex=word*2,group=halfIndex/1024,tile=(halfIndex%1024)/256,local=halfIndex%256;
    uint k=(tile%2)*16+local/16,n=(tile/2)*16+local%16;
    uint source=(group*32+k)*32+n;
    uint lo=f32tof16(asfloat(Input.Load(source*4))),hi=f32tof16(asfloat(Input.Load((source+1)*4)));
    Compact.Store(word*4,lo|(hi<<16));
}
[numthreads(128,1,1)]
void PublishPack(uint3 id:SV_DispatchThreadID){
    uint word=id.x+id.y*4096*128;if(word>=Heads*Pixels*16)return;
    uint h=word*2,tile=h/128,kt=tile%2,rt=(tile/2)%(Pixels/8),head=tile/(2*(Pixels/8)),local=h%128;
    uint pixel=rt*8+local/16,col=kt*16+local%16,source=pixel*Heads*32+head*32+col;
    uint lo=f32tof16(publishE4(asfloat(Input.Load(source*4)))),hi=f32tof16(publishE4(asfloat(Input.Load((source+1)*4))));
    Compact.Store(word*4,lo|(hi<<16));
}
[WaveSize(16)]
[numthreads(16,1,1)]
void ExpandGate(uint3 group:SV_GroupID){
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    uint rt=group.x,branch=group.y/2,nt=group.y%2;
    precise float total[8];
    for(uint head=0;head<Heads;++head){
        C c=C::Splat(0.0f);
        [unroll]for(uint kt=0;kt<2;++kt){
            A a=A::Load(Input,(head*(Pixels/8)+rt)*512+kt*256,32,MatrixLayout::RowMajor);
            B b=B::Load(ExpandWeights,((branch*Heads+head)*4+nt*2+kt)*512,32,MatrixLayout::RowMajor);
            c.MultiplyAccumulate(a,b);
        }
        [unroll]for(uint i=0;i<8;++i){if(head==0)total[i]=float(c.Get(i));else total[i]+=float(c.Get(i));}
    }
    C result=C::Splat(0.0f);[unroll]for(uint i=0;i<8;++i)result.Set(i,float16_t(gate(total[i])));
    result.Store(Compact,(branch*(Pixels/8)+rt)*512+nt*256,32,MatrixLayout::RowMajor);
}
groupshared float16_t FinalTile[128];
[WaveSize(16)]
[numthreads(16,1,1)]
void ProjectSum(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    uint rt=group.x,head=group.y/2,nt=group.y%2;precise float total[8];
    [unroll]for(uint branch=0;branch<4;++branch){
        uint index=head*4+branch;C c=C::Splat(0.0f);
        [unroll]for(uint kt=0;kt<2;++kt){
            A a=A::Load(Input,(index*(Pixels/8)+rt)*512+kt*256,32,MatrixLayout::RowMajor);
            B b=B::Load(ProjectWeights,(index*4+nt*2+kt)*512,32,MatrixLayout::RowMajor);c.MultiplyAccumulate(a,b);
        }
        [unroll]for(uint i=0;i<8;++i){if(branch==0)total[i]=float(c.Get(i));else total[i]+=float(c.Get(i));}
    }
    C result=C::Splat(0.0f);[unroll]for(uint i=0;i<8;++i)result.Set(i,float16_t(publishE4(total[i])));
    result.Store(FinalTile,0,16,MatrixLayout::RowMajor);GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<128;i+=16)Output.Store(((rt*8+i/16)*Heads*32+head*32+nt*16+i%16)*4,asuint(float(FinalTile[i])));
}
