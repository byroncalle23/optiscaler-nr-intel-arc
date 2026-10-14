#include <hlsl/dx/linalg.h>
ByteAddressBuffer Weights : register(t0);
RWStructuredBuffer<float> Output : register(u0);
[numthreads(32,1,1)]
void Main(uint3 id:SV_DispatchThreadID){
    vector<float16_t,16> v;
    for(uint k=0;k<16;++k)v[k]=float16_t((k+id.x%7)/16.0);
    dx::linalg::MatrixRef<dx::linalg::DATA_TYPE_FLOAT16,16,16,dx::linalg::MATRIX_LAYOUT_ROW_MAJOR> matrix={Weights,0,32};
    vector<float16_t,16> r=dx::linalg::Mul<float16_t>(matrix,dx::linalg::MakeInterpretedVector<dx::linalg::DATA_TYPE_FLOAT16>(v));
    for(uint k=0;k<16;++k)Output[id.x*16+k]=float(r[k]);
}
