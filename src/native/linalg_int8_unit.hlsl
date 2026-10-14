#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer InputA:register(t0);
ByteAddressBuffer InputB:register(t1);
RWByteAddressBuffer Output:register(u0);
cbuffer Settings:register(b0){uint Products;uint Words;float InverseScale;float OutputScale;}
int quantize(float x){
    float v=clamp(x*InverseScale,-127.0,127.0),lo=floor(v),f=v-lo;
    return int(lo)+int(f>.5||(f==.5&&(int(lo)&1)));
}
[numthreads(64,1,1)]
void Quantize(uint3 id:SV_DispatchThreadID){
    uint i=id.x;if(i>=Words)return;uint packed=0;
    [unroll]for(uint j=0;j<4;++j)packed|=(uint(quantize(asfloat(InputA.Load((i*4+j)*4))))&255u)<<(j*8);
    Output.Store(i*4,packed);
}
[WaveSize(16)]
[numthreads(16,1,1)]
void IntegerProduct(uint3 group:SV_GroupID){
    using A=Matrix<ComponentType::I8,8,32,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::I8,32,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::I32,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    A a=A::Load(InputA,group.x*256,32,MatrixLayout::RowMajor);
    B b=B::Load(InputB,group.x*512,16,MatrixLayout::RowMajor);
    C c=C::Splat(0);c.MultiplyAccumulate(a,b);c.Store(Output,group.x*512,64,MatrixLayout::RowMajor);
}
[WaveSize(16)]
[numthreads(16,1,1)]
void HalfProduct(uint3 group:SV_GroupID){
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    C c=C::Splat(0.0f);
    [unroll]for(uint tile=0;tile<2;++tile){
        A a=A::Load(InputA,group.x*512+tile*256,32,MatrixLayout::RowMajor);
        B b=B::Load(InputB,group.x*1024+tile*512,32,MatrixLayout::RowMajor);
        c.MultiplyAccumulate(a,b);
    }
    c.Store(Output,group.x*256,32,MatrixLayout::RowMajor);
}
[numthreads(64,1,1)]
void Dequantize(uint3 id:SV_DispatchThreadID){
    uint i=id.x;if(i>=Products*128)return;
    Output.Store(i*4,asuint(float(asint(InputA.Load(i*4)))*OutputScale));
}
