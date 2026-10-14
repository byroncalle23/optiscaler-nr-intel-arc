#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer Weights:register(t0),Input:register(t1);
RWStructuredBuffer<float> Output:register(u0);
cbuffer Shape:register(b0){uint Rows;uint Columns;uint Inner;uint Unused;}
[WaveSize(16)] [numthreads(16,1,1)]
void Main(uint3 group:SV_GroupID){
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    precise float s0[8],s1[8];for(uint e=0;e<8;++e){s0[e]=0;s1[e]=0;}
    uint row=group.x*16,col=group.y*16;
    // Model geometry has no tails. Units below check zero-safe register loading.
    for(uint block=0;block<Inner/64;++block){C c0=C::Splat(0.0f),c1=C::Splat(0.0f);
        for(uint part=0;part<4;++part){
            B b=B::Load(Weights,((col/16)*(Inner/16)+block*4+part)*512,32,MatrixLayout::RowMajor);
            A a0=A::Splat(0.0f),a1=A::Splat(0.0f);
            // ByteAddress loads with declared alignment16 when all8 rows valid.
            if(row+16<=Rows){
                a0=A::Load(Input,(row*Inner+block*64+part*16)*2,Inner*2,MatrixLayout::RowMajor,16);
                a1=A::Load(Input,((row+8)*Inner+block*64+part*16)*2,Inner*2,MatrixLayout::RowMajor,16);
            }else for(uint e=0;e<8;++e){uint2 xy=a0.GetCoordinate(e);uint k=block*64+part*16+xy.y;
                if(row+xy.x<Rows){uint index=(row+xy.x)*Inner+k;a0.Set(e,float16_t(f16tof32((Input.Load(index/2*4)>>((index%2)*16))&65535)));}
                if(row+8+xy.x<Rows){uint index=(row+8+xy.x)*Inner+k;a1.Set(e,float16_t(f16tof32((Input.Load(index/2*4)>>((index%2)*16))&65535)));}}
            c0.MultiplyAccumulate(a0,b);c1.MultiplyAccumulate(a1,b);
        }
        for(uint e=0;e<8;++e){s0[e]+=float(c0.Get(e));s1[e]+=float(c1.Get(e));}
    }
    C coords=C::Splat(0.0f);for(uint e=0;e<8;++e){uint2 xy=coords.GetCoordinate(e);
        if(row+xy.x<Rows)Output[(row+xy.x)*Columns+col+xy.y]=s0[e];
        if(row+8+xy.x<Rows)Output[(row+8+xy.x)*Columns+col+xy.y]=s1[e];}
}
