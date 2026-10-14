StructuredBuffer<float> Input:register(t0);
RWByteAddressBuffer Output:register(u0);
cbuffer Shape:register(b0){uint Columns;uint Inner;}
[numthreads(256,1,1)]
void Main(uint3 id:SV_DispatchThreadID){
    uint word=id.x;if(word>=Columns*Inner/2)return;
    uint index=word*2,tile=index/256,local=index%256;
    uint nt=tile/(Inner/16),kt=tile%(Inner/16),row=kt*16+local/16,column=nt*16+local%16;
    uint a=f32tof16(Input[row*Columns+column]),b=f32tof16(Input[row*Columns+column+1]);
    Output.Store(word*4,a|(b<<16));
}
