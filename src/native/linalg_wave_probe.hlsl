#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer InputA : register(t0);
ByteAddressBuffer InputB : register(t1);
RWByteAddressBuffer Output : register(u0);
// One independent product per wave: [8x16] * [16x16], all FP16.
[WaveSize(16)]
[numthreads(16,1,1)]
void Main(uint3 group : SV_GroupID) {
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    A a=A::Load(InputA,group.x*256,32,MatrixLayout::RowMajor);
    B b=B::Load(InputB,group.x*512,32,MatrixLayout::RowMajor);
    C c=C::Splat(0.0f);
    c.MultiplyAccumulate(a,b);
    c.Store(Output,group.x*256,32,MatrixLayout::RowMajor);
}
