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
// Static SDR frame I/O. Noise is an immutable frame-zero LUT uploaded at setup.
StructuredBuffer<float> Source : register(t0);
StructuredBuffer<float> Other : register(t1);
RWStructuredBuffer<float> Output : register(u0);
cbuffer Shape : register(b0) {uint Height;uint Width;uint TargetHeight;uint TargetWidth;}
float halfRound(float x) {
    uint bits=asuint(x),sign=bits&0x80000000,magnitude=bits&0x7fffffff;
    if(magnitude>=0x7f800000)return x;
    if(magnitude>=0x477ff000)return asfloat(sign|0x7f800000);
    if(magnitude>=0x38800000)return asfloat(sign|((magnitude+4095+((magnitude>>13)&1))&0xffffe000));
    float scaled=asfloat(magnitude)*16777216.0,lo=floor(scaled),fraction=scaled-lo;
    float nearest=lo+((fraction>.5||(fraction==.5&&(uint(lo)&1)))?1.0:0.0);
    return asfloat(sign|asuint(nearest*5.960464477539063e-8));
}
uint index(uint3 id){return id.x+id.y*65535u*128u;}
uint reflectOnce(uint p,uint extent){return p<extent?p:uint(max(0,int(2*extent-2)-int(p)));}
[numthreads(128,1,1)]
void Features(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint i=index(id);if(i>=TargetHeight*TargetWidth*16)return;
    uint c=i%16,p=i/16,y=p/TargetWidth,x=p%TargetWidth;
    float value=0;
    if(c<3)value=Other[NR_INDEX(p*3+c,1)];
    else if(c==3||c==11||c==12)value=1;
    else if(c>=4&&c<10){
        uint rgb=(c-4)%3;
        precise float sample=halfRound(Source[NR_INDEX((reflectOnce(y,Height)*Width+reflectOnce(x,Width))*3+rgb,0)]);
        precise float centered=halfRound(sample-0.5);
        value=halfRound(centered*0.125);
    }else if(c==13||c==14)value=-1;
    Output[NR_INDEX(i,3)]=value;
}
[numthreads(128,1,1)]
void Compose(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint i=index(id);if(i>=TargetHeight*TargetWidth*3)return;
    uint c=i%3,p=i/3,y=p/TargetWidth,x=p%TargetWidth;
    precise float residual=halfRound(Source[NR_INDEX((y*Width+x)*4+c,0)])*0.25;
    precise float colour=Other[NR_INDEX(i,1)];
    precise float prediction=clamp(colour+residual,0.0,1.0);
    precise float delta=prediction-colour;
    precise float result=colour+delta;
    Output[NR_INDEX(i,3)]=clamp(result,0.0,1.0);
}
