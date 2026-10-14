StructuredBuffer<uint> Packed : register(t0);
StructuredBuffer<float> Base : register(t1);
StructuredBuffer<float> Low : register(t2);
StructuredBuffer<float> Composed : register(t3);
RWByteAddressBuffer Result : register(u0);
cbuffer Shape : register(b0){uint SourceWidth;uint SourceHeight;uint Width;uint Height;uint LowWidth;uint LowHeight;}
float3 unpack(uint2 p){uint v=Packed[p.y*SourceWidth+p.x];return float3(v&1023,(v>>10)&1023,(v>>20)&1023)/1023.0;}
float3 baseAt(uint2 p){uint i=(p.y*Width+p.x)*3;return float3(Base[i],Base[i+1],Base[i+2]);}
float3 residualAt(uint2 p){uint i=(p.y*LowWidth+p.x)*3;return float3(Composed[i]-Low[i],Composed[i+1]-Low[i+1],Composed[i+2]-Low[i+2]);}
void centerMap(uint2 pixel,uint2 from,uint2 to,out uint2 a,out uint2 b,out float2 f){
    int2 denominator=int2(from*2);
    int2 numerator=clamp(int2((pixel*2+1)*to)-int2(from),int2(0,0),int2(to-1)*denominator);
    a=uint2(numerator/denominator);b=min(a+1,to-1);f=float2(numerator%denominator)/float2(denominator);
}
[numthreads(128,1,1)]
void PrepareBase(uint3 id:SV_DispatchThreadID){
    uint i=id.x+id.y*65535u*128u;if(i>=Width*Height)return;
    uint2 a,b;float2 f;centerMap(uint2(i%Width,i/Width),uint2(Width,Height),uint2(SourceWidth,SourceHeight),a,b,f);
    float3 v=lerp(lerp(unpack(a),unpack(uint2(b.x,a.y)),f.x),lerp(unpack(uint2(a.x,b.y)),unpack(b),f.x),f.y);
    Result.Store3(i*12,asuint(v));
}
[numthreads(128,1,1)]
void Reduce(uint3 id:SV_DispatchThreadID){
    uint i=id.x+id.y*65535u*128u;if(i>=LowWidth*LowHeight)return;
    uint2 ratio=uint2(Width/LowWidth,Height/LowHeight),origin=uint2(i%LowWidth,i/LowWidth)*ratio;
    precise float3 total=0;
    [loop]for(uint y=0;y<ratio.y;++y){[loop]for(uint x=0;x<ratio.x;++x)total+=baseAt(origin+uint2(x,y));}
    Result.Store3(i*12,asuint(total/float(ratio.x*ratio.y)));
}
uint unorm10(float v){float s=saturate(v)*1023;float lo=floor(s),f=s-lo;return uint(lo)+uint(f>.5||(f==.5&&(uint(lo)&1)));}
[numthreads(128,1,1)]
void ComposeHigh(uint3 id:SV_DispatchThreadID){
    uint i=id.x+id.y*65535u*128u;if(i>=Width*Height)return;
    uint2 pixel=uint2(i%Width,i/Width),a,b;float2 f;
    centerMap(pixel,uint2(Width,Height),uint2(LowWidth,LowHeight),a,b,f);
    float3 residual=lerp(lerp(residualAt(a),residualAt(uint2(b.x,a.y)),f.x),lerp(residualAt(uint2(a.x,b.y)),residualAt(b),f.x),f.y);
    float3 v=saturate(baseAt(pixel)+residual);
    // SDR fixture path publishes opaque alpha; no HDR or temporal history claim.
    Result.Store(i*4,unorm10(v.r)|(unorm10(v.g)<<10)|(unorm10(v.b)<<20)|0xc0000000u);
}
