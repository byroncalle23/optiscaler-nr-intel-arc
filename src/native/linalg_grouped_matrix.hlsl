#include <hlsl/dx/linalg.h>
using namespace dx::linalg;
ByteAddressBuffer Weights : register(t0);
StructuredBuffer<float> Input : register(t1);
RWStructuredBuffer<float> Output : register(u0);
cbuffer Shape : register(b0) { uint Rows; uint Columns; uint Inner; uint WeightTileStride; uint Groups; uint Lanes; };
#ifndef TILE
#define TILE 64
#endif
// Physical activations: [lane][group][row][channel]. Weights have no lane axis.
[numthreads(32,1,1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if(id.x>=Rows||id.z>=Groups*Lanes)return;
    uint group=id.z%Groups;
    uint inputBase=id.z*Rows*Inner+id.x*Inner;
    uint outputBase=id.z*Rows*Columns+id.x*Columns+id.y*TILE;
    vector<float,TILE> sum=(vector<float,TILE>)0;
    using MatrixTy=Matrix<ComponentType::F16,TILE,TILE,MatrixUse::A,MatrixScope::Thread>;
    for(uint block=0;block<Inner/TILE;++block){
        vector<float16_t,TILE> a;
        for(uint k=0;k<TILE;++k)a[k]=float16_t(Input[inputBase+block*TILE+k]);
        uint weightTile=(group*(Columns/TILE)+id.y)*(Inner/TILE)+block;
        MatrixTy matrix=MatrixTy::Load<MatrixLayout::MulOptimal>(Weights,weightTile*WeightTileStride,0);
        sum+=(vector<float,TILE>)Multiply<float16_t>(matrix,a);
    }
    for(uint n=0;n<TILE;++n)Output[outputBase+n]=sum[n];
}
