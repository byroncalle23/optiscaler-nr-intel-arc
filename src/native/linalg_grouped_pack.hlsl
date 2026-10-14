StructuredBuffer<float> Source : register(t0);
RWByteAddressBuffer Packed : register(u0);
cbuffer Shape : register(b0) { uint Columns; uint Inner; uint Groups; uint Tile; }
// Independent KxN matrices -> group-major transposed FP16 Tile x Tile blocks.
[numthreads(256,1,1)]
void Main(uint3 id : SV_DispatchThreadID) {
    uint pair=id.x;
    if(pair>=Groups*Columns*Inner/2)return;
    uint index=pair*2, tileArea=Tile*Tile;
    uint group=index/(Columns*Inner), localGroup=index%(Columns*Inner);
    uint tile=localGroup/tileArea, local=localGroup%tileArea;
    uint n=(tile/(Inner/Tile))*Tile+local/Tile;
    uint k=(tile%(Inner/Tile))*Tile+local%Tile;
    uint base=group*Columns*Inner;
    uint lo=f32tof16(Source[base+k*Columns+n]);
    uint hi=f32tof16(Source[base+(k+1)*Columns+n]);
    Packed.Store(pair*4,lo|(hi<<16));
}
