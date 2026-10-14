#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer Weights : register(t0);
ByteAddressBuffer Input : register(t1);
RWStructuredBuffer<float> Output : register(u0);
cbuffer Shape : register(b0) { uint Rows; uint Columns; uint Inner; uint WeightTileStride; };
#ifndef TILE_N
#define TILE_N 64
#endif
#ifndef TILE_K
#define TILE_K 64
#endif
[numthreads(32,1,1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if(id.x>=Rows)return;
    vector<float,TILE_N> sum=(vector<float,TILE_N>)0;
    using MatrixTy=Matrix<ComponentType::F16,TILE_N,TILE_K,MatrixUse::A,MatrixScope::Thread>;
    for(uint block=0;block<Inner/TILE_K;++block){
        vector<float16_t,TILE_K> a;
        for(uint k=0;k<TILE_K;++k)a[k]=float16_t(f16tof32((Input.Load(((id.x*Inner+block*TILE_K+k)/2)*4)>>(((id.x*Inner+block*TILE_K+k)%2)*16))&65535));
        uint offset=(id.y*(Inner/TILE_K)+block)*WeightTileStride;
#ifdef OPTIMAL
        MatrixTy matrix=MatrixTy::Load<MatrixLayout::MulOptimal>(Weights,offset,0);
#else
        MatrixTy matrix=MatrixTy::Load<MatrixLayout::RowMajor>(Weights,offset,TILE_K*2);
#endif
#ifdef NR_LINALG_FP32_PARTIAL
        sum+=Multiply<float>(matrix,a);
#else
        sum+=(vector<float,TILE_N>)Multiply<float16_t>(matrix,a);
#endif
    }
    for(uint n=0;n<TILE_N;++n)Output[id.x*Columns+id.y*TILE_N+n]=sum[n];
}
