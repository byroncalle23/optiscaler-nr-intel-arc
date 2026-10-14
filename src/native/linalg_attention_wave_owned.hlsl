// Whole attention study: each wave owns eight queries through QK, softmax, PV.
// Q/K normalization retains the accepted rounded tree. No shared score tensor.
#include <hlsl/dx/linalg.h>
#include "block_ops.hlsl"
using namespace dx::linalg;
using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
groupshared float16_t Q[2048],K[2048],V[2048],Norm[128];
groupshared uint Counter;
float normRow(uint row,bool query){
    uint lane=WaveGetLaneIndex(),c=lane%8,base=lane-c;
    float v0=query?float(Q[row*32+c]):float(K[row*32+c]);
    float v8=query?float(Q[row*32+c+8]):float(K[row*32+c+8]);
    float v16=query?float(Q[row*32+c+16]):float(K[row*32+c+16]);
    float v24=query?float(Q[row*32+c+24]):float(K[row*32+c+24]);
    precise float p0=v0*v0,p8=v8*v8,p16=v16*v16,p24=v24*v24;
    float partial=halfRound(halfRound(p8+halfRound(p0))+halfRound(p24+halfRound(p16)));
    float x2=halfRound(partial+WaveReadLaneAt(partial,lane^4));
    float x1=halfRound(x2+WaveReadLaneAt(x2,lane^2));
    float n=max(halfRound(WaveReadLaneAt(x1,base)+WaveReadLaneAt(x1,base+1)),0.00006198883056640625);
    return halfRound(1.0/sqrt(n));
}
C scoreTile(uint query,uint column,uint head){
    C c=C::Splat(0.0f);
    for(uint inner=0;inner<32;inner+=16){
        A a=A::Load(Q,query*8*32+inner,32,MatrixLayout::RowMajor);
        B b=B::Load(K,column*32+inner,32,MatrixLayout::ColMajor);
        c.MultiplyAccumulate(a,b);
    }
    for(uint e=0;e<8;++e){
        uint2 xy=c.GetCoordinate(e);
        precise float s=float(c.Get(e))+Parameter[head*4096+(query*8+xy.x)*64+column+xy.y];
        precise float affine=halfRound(s)*0.044921875;affine+=1.30078125;
        // Independent half transform equals the accepted packed carry for this clamp.
        uint bits=(halfBits(clamp(affine,1.03125,1.5693359375))<<5)+0x8000;
        c.Set(e,float16_t(f16tof32(bits&65535)));
    }
    return c;
}
float localRow(C c,uint row){
    precise float sum=0;
    for(uint e=0;e<8;++e)if(c.GetCoordinate(e).x==row)sum+=float(c.Get(e));
    return sum;
}
C probabilities(C c,float reciprocal[8]){
    for(uint e=0;e<8;++e){uint row=c.GetCoordinate(e).x;
        c.Set(e,float16_t(publishE4(halfRound(float(c.Get(e))*reciprocal[row]))));}
    return c;
}
[WaveSize(16)] [numthreads(128,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex){
    uint head=group.x%Channels,window=group.x/Channels;
    for(uint i=thread;i<2048;i+=128){
        uint src=group.z*Count+(window*64+i/32)*Channels*96+head*32+i%32;
        Q[i]=float16_t(halfRound(Source[src]));K[i]=float16_t(halfRound(Source[src+Channels*32]));
        V[i]=float16_t(publishE4(Source[src+Channels*64]));
    }
    if(thread==0)Counter=0;
    GroupMemoryBarrierWithGroupSync();
    uint wave=0;if(WaveIsFirstLane())InterlockedAdd(Counter,1,wave);wave=WaveReadLaneFirst(wave);
    for(uint iteration=0;iteration<4;++iteration){
        uint row=wave*2+WaveGetLaneIndex()/8+iteration*16;
        float q=normRow(row,true),k=normRow(row,false);
        if(WaveGetLaneIndex()%8==0){Norm[row*2]=float16_t(q);Norm[row*2+1]=float16_t(k);}
    }
    GroupMemoryBarrierWithGroupSync();
    float scale=halfRound(Skip[head]);
    for(uint i=thread;i<2048;i+=128){
        Q[i]=float16_t(publishE4(halfRound(halfRound(float(Q[i])*float(Norm[(i/32)*2]))*scale)));
        K[i]=float16_t(publishE4(halfRound(float(K[i])*float(Norm[(i/32)*2+1]))));
    }
    GroupMemoryBarrierWithGroupSync();
    C s0=scoreTile(wave,0,head),s1=scoreTile(wave,16,head),s2=scoreTile(wave,32,head),s3=scoreTile(wave,48,head);
    float reciprocal[8];
    for(uint row=0;row<8;++row){
        precise float local=localRow(s0,row);local+=localRow(s1,row);local+=localRow(s2,row);local+=localRow(s3,row);
        float total=WaveActiveSum(local);
        reciprocal[row]=halfRound(1.0/halfRound(total));
    }
    s0=probabilities(s0,reciprocal);s1=probabilities(s1,reciprocal);s2=probabilities(s2,reciprocal);s3=probabilities(s3,reciprocal);
    A a0=s0.Cast<ComponentType::F16,MatrixUse::A>(),a1=s1.Cast<ComponentType::F16,MatrixUse::A>();
    A a2=s2.Cast<ComponentType::F16,MatrixUse::A>(),a3=s3.Cast<ComponentType::F16,MatrixUse::A>();
    for(uint column=0;column<32;column+=16){
        C context=C::Splat(0.0f);
        B b0=B::Load(V,column,32,MatrixLayout::RowMajor);context.MultiplyAccumulate(a0,b0);
        B b1=B::Load(V,16*32+column,32,MatrixLayout::RowMajor);context.MultiplyAccumulate(a1,b1);
        B b2=B::Load(V,32*32+column,32,MatrixLayout::RowMajor);context.MultiplyAccumulate(a2,b2);
        B b3=B::Load(V,48*32+column,32,MatrixLayout::RowMajor);context.MultiplyAccumulate(a3,b3);
        for(uint e=0;e<8;++e){uint2 xy=context.GetCoordinate(e);
            Output[group.z*Publish+(window*64+wave*8+xy.x)*Channels*32+head*32+column+xy.y]=publishE4(float(context.Get(e)));}
    }
}
