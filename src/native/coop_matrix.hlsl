#include <hlsl/dx/linalg.h>
// Standalone gate: FP32 activation -> FP16 registers -> cooperative multiply
// -> FP32 accumulation/output. No intermediate conversion buffers.
ByteAddressBuffer Weights : register(t0);
StructuredBuffer<float> Input : register(t1);
RWStructuredBuffer<float> Output : register(u0);
cbuffer Shape : register(b0) { uint Rows; uint Columns; uint Inner; uint WeightTileStride; };
#ifndef TILE_N
#define TILE_N 128
#endif
#ifndef TILE_K
#define TILE_K 256
#endif
[numthreads(32,1,1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Rows) return;
    vector<float, TILE_N> sum = (vector<float,TILE_N>)0;
    for (uint block = 0; block < Inner / TILE_K; ++block) {
        vector<float16_t, TILE_K> a;
        for (uint k=0; k<TILE_K; ++k)
            a[k] = float16_t(Input[id.x * Inner + block * TILE_K + k]);
        uint offset = (id.y * (Inner / TILE_K) + block) * WeightTileStride;
#ifdef OPTIMAL
        dx::linalg::MatrixRef<dx::linalg::DATA_TYPE_FLOAT16,TILE_N,TILE_K,dx::linalg::MATRIX_LAYOUT_MUL_OPTIMAL> matrix = {Weights,offset,0};
#else
        dx::linalg::MatrixRef<dx::linalg::DATA_TYPE_FLOAT16,TILE_N,TILE_K,dx::linalg::MATRIX_LAYOUT_ROW_MAJOR> matrix = {Weights,offset,TILE_K*2};
#endif
#ifdef CV_RESULT_HALF
        sum += (vector<float,TILE_N>)dx::linalg::Mul<float16_t>(matrix, dx::linalg::MakeInterpretedVector<dx::linalg::DATA_TYPE_FLOAT16>(a));
#else
        sum += dx::linalg::Mul<float>(matrix, dx::linalg::MakeInterpretedVector<dx::linalg::DATA_TYPE_FLOAT16>(a));
#endif
    }
    for (uint n=0; n<TILE_N; ++n)
        Output[id.x * Columns + id.y * TILE_N + n] = sum[n];
}
