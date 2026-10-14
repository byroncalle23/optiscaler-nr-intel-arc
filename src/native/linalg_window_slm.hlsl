#include <hlsl/dx/linalg.h>
// Reuse the exact published rounding functions and t0/t2/u0, b0 declarations.
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

// Offline whole-window attention layout study. Arithmetic/order unchanged.
#ifndef NR_Q_STRIDE
#define NR_Q_STRIDE 32
#endif
#ifndef NR_SCORE_STRIDE
#define NR_SCORE_STRIDE 66
#endif
uint NrQIndex(uint i){return (i/32)*NR_Q_STRIDE+i%32;}
uint NrScoreIndex(uint i){return (i/64)*NR_SCORE_STRIDE+i%64;}
groupshared float16_t FusedQ[64*NR_Q_STRIDE],FusedK[64*NR_Q_STRIDE],FusedV[64*NR_Q_STRIDE],FusedScores[64*NR_SCORE_STRIDE],FusedReciprocal[64];
groupshared uint FusedWaveCounter;
#if NORMALIZE_INPUT
float fusedNorm(uint row,bool query){
    float partial[8],xorTwo[8],xorOne[8];
    [unroll]for(uint c=0;c<8;++c){
        float v0=query?float(FusedQ[NrQIndex(row*32+c)]):float(FusedK[NrQIndex(row*32+c)]);
        float v8=query?float(FusedQ[NrQIndex(row*32+c+8)]):float(FusedK[NrQIndex(row*32+c+8)]);
        float v16=query?float(FusedQ[NrQIndex(row*32+c+16)]):float(FusedK[NrQIndex(row*32+c+16)]);
        float v24=query?float(FusedQ[NrQIndex(row*32+c+24)]):float(FusedK[NrQIndex(row*32+c+24)]);
        precise float p0=v0*v0,p8=v8*v8,p16=v16*v16,p24=v24*v24;
        partial[c]=halfRound(halfRound(p8+halfRound(p0))+halfRound(p24+halfRound(p16)));
    }
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorTwo[l*2+p]=halfRound(partial[l*2+p]+partial[(l^2)*2+p]);}
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorOne[l*2+p]=halfRound(xorTwo[l*2+p]+xorTwo[(l^1)*2+p]);}
    float norm=max(halfRound(xorOne[0]+xorOne[1]),0.00006198883056640625);
    return halfRound(1.0/sqrt(norm));
}
#endif
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
        FusedQ[NrQIndex(i)]=float16_t(halfRound(Source[source]));
        FusedK[NrQIndex(i)]=float16_t(halfRound(Source[source+Channels*32]));
        FusedV[NrQIndex(i)]=float16_t(publishE4(Source[source+Channels*64]));
#else
        FusedQ[NrQIndex(i)]=float16_t(Source[base+i]);
        FusedK[NrQIndex(i)]=float16_t(Source[base+plane+i]);
        FusedV[NrQIndex(i)]=float16_t(Source[base+plane*2+i]);
#endif
    }
    if(thread==0)FusedWaveCounter=0;
    GroupMemoryBarrierWithGroupSync();
#if NORMALIZE_INPUT
    // Scores are not live yet; reuse their storage for the Q/K reciprocals.
    if(thread<64){FusedScores[NrScoreIndex(thread*2)]=float16_t(fusedNorm(thread,true));FusedScores[NrScoreIndex(thread*2+1)]=float16_t(fusedNorm(thread,false));}
    GroupMemoryBarrierWithGroupSync();
    float scale=halfRound(Skip[head]);
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS){
        FusedQ[NrQIndex(i)]=float16_t(publishE4(halfRound(halfRound(float(FusedQ[NrQIndex(i)])*float(FusedScores[NrScoreIndex((i/32)*2)]))*scale)));
        FusedK[NrQIndex(i)]=float16_t(publishE4(halfRound(float(FusedK[NrQIndex(i)])*float(FusedScores[NrScoreIndex((i/32)*2+1)]))));
    }
    GroupMemoryBarrierWithGroupSync();
#endif
    // Allocate a unique query tile without assuming SV_GroupIndex-to-wave mapping.
    uint wave=0;
    if(WaveIsFirstLane())InterlockedAdd(FusedWaveCounter,1,wave);
    wave=WaveReadLaneFirst(wave);
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    uint columnPart=wave%NR_ATTENTION_COLUMNS;
    uint firstQuery=wave/NR_ATTENTION_COLUMNS;
    uint queryStride=NR_ATTENTION_WAVES/NR_ATTENTION_COLUMNS;
    for(uint query=firstQuery;query<8;query+=queryStride){
        for(uint column=columnPart*(64/NR_ATTENTION_COLUMNS);column<(columnPart+1)*(64/NR_ATTENTION_COLUMNS);column+=16){
            C c=C::Splat(0.0f);
            for(uint inner=0;inner<32;inner+=16){
                A a=A::Load(FusedQ,query*8*NR_Q_STRIDE+inner,NR_Q_STRIDE,MatrixLayout::RowMajor);
                B b=B::Load(FusedK,column*NR_Q_STRIDE+inner,NR_Q_STRIDE,MatrixLayout::ColMajor);
                c.MultiplyAccumulate(a,b);
            }
            c.Store(FusedScores,query*8*NR_SCORE_STRIDE+column,NR_SCORE_STRIDE,MatrixLayout::RowMajor);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Each pair has one owner; preserve WindowSoftmax's packed transform/carry.
    for(uint pair=thread;pair<2048;pair+=NR_ATTENTION_THREADS){
        uint index=pair*2,bias=head*4096+index;
        precise float scoreA=float(FusedScores[NrScoreIndex(index)])+Parameter[bias];
        precise float scoreB=float(FusedScores[NrScoreIndex(index+1)])+Parameter[bias+1];
        precise float affineA=halfRound(scoreA)*0.044921875;affineA+=1.30078125;
        precise float affineB=halfRound(scoreB)*0.044921875;affineB+=1.30078125;
        uint packed=halfBits(clamp(affineA,1.03125,1.5693359375))|(halfBits(clamp(affineB,1.03125,1.5693359375))<<16);
        uint transformed=(packed<<5)+0x7FF88000;
        FusedScores[NrScoreIndex(index)]=float16_t(f16tof32(transformed&65535));
        FusedScores[NrScoreIndex(index+1)]=float16_t(f16tof32(transformed>>16));
    }
    GroupMemoryBarrierWithGroupSync();
    if(thread<64){
        precise float total=0;
        [loop]for(uint key=0;key<64;++key)total+=float(FusedScores[NrScoreIndex(thread*64+key)]);
        FusedReciprocal[thread]=float16_t(halfRound(1.0/halfRound(total)));
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<4096;i+=NR_ATTENTION_THREADS)
        FusedScores[NrScoreIndex(i)]=float16_t(publishE4(halfRound(float(FusedScores[NrScoreIndex(i)])*float(FusedReciprocal[i/64]))));
    GroupMemoryBarrierWithGroupSync();
    // Queries are dead now: reuse their shared allocation for the context.
    for(uint query=firstQuery;query<8;query+=queryStride){
        for(uint column=columnPart*(32/NR_ATTENTION_COLUMNS);column<(columnPart+1)*(32/NR_ATTENTION_COLUMNS);column+=16){
            C c=C::Splat(0.0f);
            for(uint inner=0;inner<64;inner+=16){
                A a=A::Load(FusedScores,query*8*NR_SCORE_STRIDE+inner,NR_SCORE_STRIDE,MatrixLayout::RowMajor);
                B b=B::Load(FusedV,inner*NR_Q_STRIDE+column,NR_Q_STRIDE,MatrixLayout::RowMajor);
                c.MultiplyAccumulate(a,b);
            }
            c.Store(FusedQ,query*8*NR_Q_STRIDE+column,NR_Q_STRIDE,MatrixLayout::RowMajor);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=thread;i<2048;i+=NR_ATTENTION_THREADS){
        uint token=i/32,channel=i%32;
        Output[group.z*Publish+(window*64+token)*Channels*32+head*32+channel]=publishE4(float(FusedQ[NrQIndex(i)]));
    }
}
