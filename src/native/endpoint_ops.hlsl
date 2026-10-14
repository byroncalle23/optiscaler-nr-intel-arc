// Tile lanes use independent activation slices; immutable parameters have stride zero.
#if NR_TILE_BATCH
cbuffer NrBatchLayout : register(b15) {uint NrStride0;uint NrStride1;uint NrStride2;uint NrStride3;uint NrStride4;uint NrStride5;}
static uint NrLane=0;
#define NR_INDEX(i,s) ((i)+NrLane*NrStride##s)
#define NR_SET_LANE(z) NrLane=(z)
#else
#define NR_INDEX(i,s) (i)
#define NR_SET_LANE(z)
#endif
StructuredBuffer<float> Source : register(t0);
StructuredBuffer<float> Other : register(t1);
StructuredBuffer<float> Sine : register(t2);
StructuredBuffer<float> Cosine : register(t3);
RWStructuredBuffer<float> Output : register(u0);
cbuffer Shape : register(b0){uint Height;uint Width;uint TargetHeight;uint TargetWidth;}
uint endpointIndex(uint3 id){return id.x+id.y*65535u*128u;}
[numthreads(128,1,1)]
void CompensatedAdapter(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint i=endpointIndex(id);if(i>=Height*Width*32)return;
    uint row=i/32,column=i%32;precise float total=0.0,correction=0.0;
    [loop]for(uint k=0;k<16;++k){
        precise float product=Source[NR_INDEX(row*16+k,0)]*Other[NR_INDEX(k*32+column,1)];
        precise float next=total+product,part=next-total;
        precise float error=(total-(next-part))+(product-part);
        correction+=error;total=next;
    }Output[NR_INDEX(i,3)]=total+correction;
}
[numthreads(128,1,1)]
void PackGain(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);uint i=endpointIndex(id);if(i<Height*Width*16)Output[NR_INDEX(i,3)]=Source[NR_INDEX((i/16)*32+i%16,0)];}
[numthreads(128,1,1)]
void PackConv(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);uint i=endpointIndex(id);if(i<Height*Width*16)Output[NR_INDEX(i,3)]=Source[NR_INDEX((i/16)*32+16+i%16,0)];}
[numthreads(128,1,1)]
void AddHead(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);uint i=endpointIndex(id);if(i<Height*Width*4)Output[NR_INDEX(i,3)]=Source[NR_INDEX(i,0)]+Other[NR_INDEX(i,1)];}
[numthreads(128,1,1)]
void FinalInputMerge(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint i=endpointIndex(id);if(i>=TargetHeight*TargetWidth*32)return;
    uint c=i%32,pixel=i/32,y=pixel/TargetWidth,x=pixel%TargetWidth;
    precise float up=Source[NR_INDEX(((y/2)*Width+x/2)*32+c,0)]*Sine[NR_INDEX(c,2)];
    precise float skip=Other[NR_INDEX(i,1)]*Cosine[NR_INDEX(c,4)];
    precise float sum=up+skip;Output[NR_INDEX(i,3)]=sum;
}
