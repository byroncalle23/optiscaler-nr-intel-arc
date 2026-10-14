#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer InputA:register(t0);
ByteAddressBuffer InputB:register(t1);
RWByteAddressBuffer Scratch:register(u0);
RWByteAddressBuffer Result:register(u1);
cbuffer Shape:register(b0){uint Rows;uint Columns;uint Inner;uint Fused;float InverseScale;float OutputScale;}
int quantize(float x){
    float v=clamp(x*InverseScale,-127.0,127.0),lo=floor(v),f=v-lo;
    return int(lo)+int(f>.5||(f==.5&&(int(lo)&1)));
}
// Pack each activation 8x32 tile once, independently of output-column count.
[numthreads(64,1,1)]
void Quantize(uint3 id:SV_DispatchThreadID){
    uint words=Rows*Inner/4;
    uint word=id.x+id.y*min((words+63)/64,4096u)*64;if(word>=words)return;
    uint tile=word/64,local=word%64,kt=tile%(Inner/32),rt=tile/(Inner/32);
    uint row=rt*8+local/8,k=kt*32+(local%8)*4,packed=0;
    [unroll]for(uint j=0;j<4;++j)packed|=(uint(quantize(asfloat(InputA.Load((row*Inner+k+j)*4))))&255u)<<(j*8);
    Scratch.Store(word*4,packed);
}
void publish(uint tile,uint lane){
    uint rt=tile/(Columns/16),ct=tile%(Columns/16);
    for(uint i=lane;i<128;i+=16){
        uint column=ct*16+i%16;
        float value=float(asint(Scratch.Load((tile*128+i)*4)))*OutputScale;
        value*=asfloat(InputB.Load(Inner*Columns+column*4));
        Result.Store(((rt*8+i/16)*Columns+ct*16+i%16)*4,asuint(value));
    }
}
[WaveSize(16)]
[numthreads(16,1,1)]
void Product(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    using A=Matrix<ComponentType::I8,8,32,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::I8,32,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::I32,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    uint rt=group.x,ct=group.y,tile=rt*(Columns/16)+ct;
    C c=C::Splat(0);
    for(uint kt=0;kt<Inner/32;++kt){
        A a=A::Load(InputA,(rt*(Inner/32)+kt)*256,32,MatrixLayout::RowMajor);
        B b=B::Load(InputB,(ct*(Inner/32)+kt)*512,16,MatrixLayout::RowMajor);
        c.MultiplyAccumulate(a,b);
    }
    c.Store(Scratch,tile*512,64,MatrixLayout::RowMajor);
    if(Fused){
        // Typed matrix store remains in VRAM. This fuses dispatches, not registers.
        DeviceMemoryBarrierWithGroupSync();publish(tile,lane);
    }
}
[numthreads(16,1,1)]
void Publish(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){publish(group.x*(Columns/16)+group.y,lane);}
