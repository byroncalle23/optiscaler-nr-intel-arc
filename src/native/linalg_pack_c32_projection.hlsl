StructuredBuffer<float> Source : register(t0);
RWByteAddressBuffer Packed : register(u0);
cbuffer Shape : register(b0) { uint Columns; uint Inner; }
// Transpose KxN into N/TILE_N x K/32 row-major TILE_N x 32 FP16 blocks.
#ifndef TILE_N
#define TILE_N 32
#endif
[numthreads(256,1,1)]
void Main(uint3 id : SV_DispatchThreadID) {
    uint pair=id.x;
    if(pair>=Columns*Inner/2)return;
    uint index=pair*2, tile=index/(TILE_N*32), local=index%(TILE_N*32);
    uint n=(tile/(Inner/32))*TILE_N+local/32;
    uint k=(tile%(Inner/32))*32+local%32;
    uint lo=f32tof16(Source[k*Columns+n]);
    uint hi=f32tof16(Source[(k+1)*Columns+n]);
    Packed.Store(pair*4,lo|(hi<<16));
}
