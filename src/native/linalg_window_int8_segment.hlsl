#include <hlsl/dx/linalg.h>
// Isolated Wave8-row normalization experiment. Reuse the exact published rounding functions and t0/t2/u0, b0 declarations.
#include "block_ops.hlsl"
using namespace dx::linalg;
#ifndef NR_ATTENTION_THREADS
#define NR_ATTENTION_THREADS 128
#endif
#if NR_ATTENTION_THREADS != 64 && NR_ATTENTION_THREADS != 128 && NR_ATTENTION_THREADS != 256
#error Unsupported attention thread partition
#endif
#define NR_ATTENTION_WAVES (NR_ATTENTION_THREADS/16)
#if NR_ATTENTION_THREADS == 256
#define NR_ATTENTION_COLUMNS 2
#else
#define NR_ATTENTION_COLUMNS 1
#endif

groupshared float16_t FusedQ[2048],FusedK[2048],FusedV[2048],FusedScores[4096],FusedReciprocal[64];
// Offline whole-window segment: quantized integer values in FP16 shared storage,
// explicit F16->I8 matrix conversion in registers. No VRAM staging dispatches.
groupshared float QScale[64],KScale[64],PScale[64],VScale[32];
groupshared uint FusedWaveCounter;
float quantValue(float value,float step){
    float x=clamp(value/step,-127.0,127.0),lo=floor(x),f=x-lo;
    int q=int(lo);if(f>0.5 || (f==0.5 && (q&1)!=0))++q;
    return float(q);
}
float quantStep(float maximum){return max(maximum/127.0,1e-20);}

#if NORMALIZE_INPUT
float fusedNorm(uint row,bool query){
    float partial[8],xorTwo[8],xorOne[8];
    [unroll]for(uint c=0;c<8;++c){
        float v0=query?float(FusedQ[row*32+c]):float(FusedK[row*32+c]);
        float v8=query?float(FusedQ[row*32+c+8]):float(FusedK[row*32+c+8]);
        float v16=query?float(FusedQ[row*32+c+16]):float(FusedK[row*32+c+16]);
        float v24=query?float(FusedQ[row*32+c+24]):float(FusedK[row*32+c+24]);
        precise float p0=v0*v0,p8=v8*v8,p16=v16*v16,p24=v24*v24;
        partial[c]=halfRound(halfRound(p8+halfRound(p0))+halfRound(p24+halfRound(p16)));
    }
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorTwo[l*2+p]=halfRound(partial[l*2+p]+partial[(l^2)*2+p]);}
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorOne[l*2+p]=halfRound(xorTwo[l*2+p]+xorTwo[(l^1)*2+p]);}
    float norm=max(halfRound(xorOne[0]+xorOne[1]),0.00006198883056640625);
    return halfRound(1.0/sqrt(norm));
}
#endif
#if NR_NORM_COOPERATIVE && NR_ATTENTION_THREADS != 256
#error Cooperative normalization requires WG256
#endif
#if NR_NORM_COOPERATIVE
// Eight lanes cover c=0..7; XOR4 then XOR2 preserves fusedNorm's exact tree.
float cooperativeNorm(uint row,bool query){
    uint lane=WaveGetLaneIndex(),c=lane%8,base=lane-c;
    float v0=query?float(FusedQ[row*32+c]):float(FusedK[row*32+c]);
    float v8=query?float(FusedQ[row*32+c+8]):float(FusedK[row*32+c+8]);
    float v16=query?float(FusedQ[row*32+c+16]):float(FusedK[row*32+c+16]);
    float v24=query?float(FusedQ[row*32+c+24]):float(FusedK[row*32+c+24]);
    precise float p0=v0*v0,p8=v8*v8,p16=v16*v16,p24=v24*v24;
    float partial=halfRound(halfRound(p8+halfRound(p0))+halfRound(p24+halfRound(p16)));
    float xorTwo=halfRound(partial+WaveReadLaneAt(partial,lane^4));
    float xorOne=halfRound(xorTwo+WaveReadLaneAt(xorTwo,lane^2));
    float norm=max(halfRound(WaveReadLaneAt(xorOne,base)+WaveReadLaneAt(xorOne,base+1)),0.00006198883056640625);
    return halfRound(1.0/sqrt(norm));
}
#endif
#if NR_SOFTMAX_COOPERATIVE && NR_ATTENTION_THREADS != 256
#error Softmax Wave8 requires WG256
#endif
// Positive transformed softmax terms. New tree is FP32, not FP16 partial sums.
float softmaxRowSum(uint row){
    precise float total=0;
#if NR_SOFTMAX_COOPERATIVE
    uint lane=WaveGetLaneIndex(),c=lane%8;
    [unroll]for(uint part=0;part<8;++part)total+=float(FusedScores[row*64+c+part*8]);
    total=total+WaveReadLaneAt(total,lane^4);
    total=total+WaveReadLaneAt(total,lane^2);
    total=total+WaveReadLaneAt(total,lane^1);
    return WaveReadLaneAt(total,lane-c);
#else
    [loop]for(uint key=0;key<64;++key)total+=float(FusedScores[row*64+key]);
    return total;
#endif
}
// b0: Tokens=window-head groups, Channels=heads, Count=input lane stride,
// Publish=output lane stride. All 64-token windows have independent shared data.
[WaveSize(16)]
[numthreads(NR_ATTENTION_THREADS,1,1)]
void Main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex){
    uint head=group.x%Channels,window=group.x/Channels;
    uint base=group.z*Count+group.x*2048,plane=Tokens*2048;
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS){
#if NORMALIZE_INPUT
        uint source=group.z*Count+(window*64+i/32)*Channels*32*3+head*32+i%32;
        FusedQ[i]=float16_t(halfRound(Source[source]));
        FusedK[i]=float16_t(halfRound(Source[source+Channels*32]));
        FusedV[i]=float16_t(publishE4(Source[source+Channels*64]));
#else
        FusedQ[i]=float16_t(Source[base+i]);
        FusedK[i]=float16_t(Source[base+plane+i]);
        FusedV[i]=float16_t(Source[base+plane*2+i]);
#endif
    }
    if(thread==0)FusedWaveCounter=0;
    GroupMemoryBarrierWithGroupSync();
    // Allocate a unique query tile without assuming SV_GroupIndex-to-wave mapping.
    uint wave=0;
    if(WaveIsFirstLane())InterlockedAdd(FusedWaveCounter,1,wave);
    wave=WaveReadLaneFirst(wave);
#if NORMALIZE_INPUT
    // Scores are not live yet; reuse their storage for the Q/K reciprocals.
#if NR_NORM_COOPERATIVE
    [unroll]for(uint iteration=0;iteration<2;++iteration){
        uint row=wave*2+WaveGetLaneIndex()/8+iteration*32;
        float q=cooperativeNorm(row,true),k=cooperativeNorm(row,false);
        if(WaveGetLaneIndex()%8==0){FusedScores[row*2]=float16_t(q);FusedScores[row*2+1]=float16_t(k);}
    }
#else
    if(thread<64){FusedScores[thread*2]=float16_t(fusedNorm(thread,true));FusedScores[thread*2+1]=float16_t(fusedNorm(thread,false));}
#endif
    GroupMemoryBarrierWithGroupSync();
    float scale=halfRound(Skip[head]);
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS){
        FusedQ[i]=float16_t(publishE4(halfRound(halfRound(float(FusedQ[i])*float(FusedScores[(i/32)*2]))*scale)));
        FusedK[i]=float16_t(publishE4(halfRound(float(FusedK[i])*float(FusedScores[(i/32)*2+1]))));
    }
    GroupMemoryBarrierWithGroupSync();
#endif
#if NR_NORM_CAPTURE
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS){
        uint dst=group.z*Publish+(window*64+i/32)*Channels*64+head*32+i%32;
        Output[dst]=float(FusedQ[i]);Output[dst+Channels*32]=float(FusedK[i]);
    }
    return;
#endif
    if(thread<64){
        float qm=0,km=0;
        [unroll]for(uint c=0;c<32;++c){qm=max(qm,abs(float(FusedQ[thread*32+c])));km=max(km,abs(float(FusedK[thread*32+c])));}
        QScale[thread]=quantStep(qm);KScale[thread]=quantStep(km);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS){
        FusedQ[i]=float16_t(quantValue(float(FusedQ[i]),QScale[i/32]));
        FusedK[i]=float16_t(quantValue(float(FusedK[i]),KScale[i/32]));
    }
    GroupMemoryBarrierWithGroupSync();
    using FA=Matrix<ComponentType::F16,8,32,MatrixUse::A,MatrixScope::Wave>;
    using FB=Matrix<ComponentType::F16,32,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::I32,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    using A=Matrix<ComponentType::I8,8,32,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::I8,32,16,MatrixUse::B,MatrixScope::Wave>;
    uint columnPart=wave%NR_ATTENTION_COLUMNS;
    uint firstQuery=wave/NR_ATTENTION_COLUMNS;
    uint queryStride=NR_ATTENTION_WAVES/NR_ATTENTION_COLUMNS;
    for(uint query=firstQuery;query<8;query+=queryStride){
        for(uint column=columnPart*(64/NR_ATTENTION_COLUMNS);column<(columnPart+1)*(64/NR_ATTENTION_COLUMNS);column+=16){
            C c=C::Splat(0);
            FA fa=FA::Load(FusedQ,query*8*32,32,MatrixLayout::RowMajor);
            FB fb=FB::Load(FusedK,column*32,32,MatrixLayout::ColMajor);
            A a=fa.Cast<ComponentType::I8>();B b=fb.Cast<ComponentType::I8>();
            c.MultiplyAccumulate(a,b);
            for(uint e=0;e<c.Length();++e){uint2 xy=c.GetCoordinate(e);uint r=query*8+xy.x,col=column+xy.y;
                FusedScores[r*64+col]=float16_t(halfRound(float(c.Get(e))*QScale[r]*KScale[col]));}

        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Each pair has one owner; preserve WindowSoftmax's packed transform/carry.
    for(uint pair=thread;pair<2048;pair+=NR_ATTENTION_THREADS){
        uint index=pair*2,bias=head*4096+index;
        precise float scoreA=float(FusedScores[index])+Parameter[bias];
        precise float scoreB=float(FusedScores[index+1])+Parameter[bias+1];
        precise float affineA=halfRound(scoreA)*0.044921875;affineA+=1.30078125;
        precise float affineB=halfRound(scoreB)*0.044921875;affineB+=1.30078125;
        uint packed=halfBits(clamp(affineA,1.03125,1.5693359375))|(halfBits(clamp(affineB,1.03125,1.5693359375))<<16);
        uint transformed=(packed<<5)+0x7FF88000;
        FusedScores[index]=float16_t(f16tof32(transformed&65535));
        FusedScores[index+1]=float16_t(f16tof32(transformed>>16));
    }
    GroupMemoryBarrierWithGroupSync();
#if NR_SOFTMAX_COOPERATIVE
    [unroll]for(uint iteration=0;iteration<2;++iteration){
        uint row=wave*2+WaveGetLaneIndex()/8+iteration*32;
        float total=softmaxRowSum(row);
        if(WaveGetLaneIndex()%8==0)FusedReciprocal[row]=float16_t(halfRound(1.0/halfRound(total)));
    }
#else
    if(thread<64){float total=softmaxRowSum(thread);FusedReciprocal[thread]=float16_t(halfRound(1.0/halfRound(total)));}
#endif
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<4096;i+=NR_ATTENTION_THREADS)
        FusedScores[i]=float16_t(publishE4(halfRound(float(FusedScores[i])*float(FusedReciprocal[i/64]))));
    GroupMemoryBarrierWithGroupSync();
    if(thread<64){
        float maximum=0;
        [loop]for(uint k=0;k<64;++k)maximum=max(maximum,abs(float(FusedScores[thread*64+k])));
        PScale[thread]=quantStep(maximum);
    }
    if(thread<32){
        float maximum=0;
        [loop]for(uint k=0;k<64;++k)maximum=max(maximum,abs(float(FusedV[k*32+thread])));
        VScale[thread]=quantStep(maximum);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<4096;i+=NR_ATTENTION_THREADS)FusedScores[i]=float16_t(quantValue(float(FusedScores[i]),PScale[i/64]));
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS)FusedV[i]=float16_t(quantValue(float(FusedV[i]),VScale[i%32]));
    GroupMemoryBarrierWithGroupSync();
    // Queries are dead now: reuse their shared allocation for the context.
    for(uint query=firstQuery;query<8;query+=queryStride){
        for(uint column=columnPart*(32/NR_ATTENTION_COLUMNS);column<(columnPart+1)*(32/NR_ATTENTION_COLUMNS);column+=16){
            C c=C::Splat(0);
            for(uint inner=0;inner<64;inner+=32){
                FA fa=FA::Load(FusedScores,query*8*64+inner,64,MatrixLayout::RowMajor);
                FB fb=FB::Load(FusedV,inner*32+column,32,MatrixLayout::RowMajor);
                A a=fa.Cast<ComponentType::I8>();B b=fb.Cast<ComponentType::I8>();
                c.MultiplyAccumulate(a,b);
            }
            for(uint e=0;e<c.Length();++e){uint2 xy=c.GetCoordinate(e);uint r=query*8+xy.x,col=column+xy.y;
                FusedQ[r*32+col]=float16_t(halfRound(float(c.Get(e))*PScale[r]*VScale[col]));}

        }
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS){
        uint token=i/32,channel=i%32;
        Output[group.z*Publish+(window*64+token)*Channels*32+head*32+channel]=publishE4(float(FusedQ[i]));
    }
}
