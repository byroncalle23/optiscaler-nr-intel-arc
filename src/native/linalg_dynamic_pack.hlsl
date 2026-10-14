StructuredBuffer<float> Source : register(t0);
RWByteAddressBuffer Packed : register(u0);
cbuffer Shape : register(b0) { uint SourceRows; uint SourceColumns; uint Rows; uint Columns; uint Groups; uint LaneStride; uint Transpose; uint Pairs; }
float readValue(uint group,uint row,uint column,uint lane) {
    uint r=Transpose?column:row,c=Transpose?row:column;
    [branch]if(r>=SourceRows||c>=SourceColumns)return 0;
    return Source[lane*LaneStride+group*SourceRows*SourceColumns+r*SourceColumns+c];
}
[numthreads(128,1,1)]
void Main(uint3 id:SV_DispatchThreadID) {
    uint pair=id.x+id.y*65535u*128u;
    if(pair>=Pairs)return;
    uint index=pair*2,group=index/(Rows*Columns),local=index%(Rows*Columns);
    uint row=local/Columns,column=local%Columns;
    uint lo=f32tof16(readValue(group,row,column,id.z));
    uint hi=f32tof16(readValue(group,row,column+1,id.z));
    Packed.Store((id.z*Pairs+pair)*4,lo|(hi<<16));
}
