Texture2D<float4> InputTexture : register(t0);
StructuredBuffer<float> RGB : register(t1);
RWStructuredBuffer<float> Colour : register(u0);
RWTexture2D<float4> OutputTexture : register(u1);
RWStructuredBuffer<float> Accumulator : register(u2);
cbuffer Shape : register(b0){uint Width;uint Height;uint ModelWidth;uint ModelHeight;uint TileX;uint TileY;uint Core;uint Halo;uint Tiled;uint Stride;uint WorkWidth;uint WorkHeight;}
int reflectPixel(int p,int extent){if(extent<=1)return 0;int period=2*(extent-1);p=((p%period)+period)%period;return p<extent?p:period-p;}
void pixelCenterMap(uint2 pixel,uint2 from,uint2 to,out uint2 a,out uint2 b,out float2 f){
    // Quotient/remainder keeps fractional sampling accurate at large coordinates.
    int2 denominator=int2(from*2);
    int2 numerator=clamp(int2((pixel*2+1)*to)-int2(from),int2(0,0),int2(to-1)*denominator);
    a=uint2(numerator/denominator);b=min(a+1,to-1);f=float2(numerator%denominator)/float2(denominator);
}
float3 workColour(uint2 pixel){
    if(WorkWidth==Width&&WorkHeight==Height)return InputTexture.Load(int3(pixel,0)).rgb;
    uint2 a,b;float2 f;pixelCenterMap(pixel,uint2(WorkWidth,WorkHeight),uint2(Width,Height),a,b,f);
    return lerp(lerp(InputTexture.Load(int3(a,0)).rgb,InputTexture.Load(int3(b.x,a.y,0)).rgb,f.x),
                lerp(InputTexture.Load(int3(a.x,b.y,0)).rgb,InputTexture.Load(int3(b,0)).rgb,f.x),f.y);
}
float3 workResidual(uint2 pixel){uint b=(pixel.y*WorkWidth+pixel.x)*4;return float3(Accumulator[b],Accumulator[b+1],Accumulator[b+2])/max(Accumulator[b+3],1e-12);}
float textureHalfRound(float x){
    uint bits=asuint(x),sign=bits&0x80000000,magnitude=bits&0x7fffffff;
    if(magnitude>=0x7f800000)return x;
    if(magnitude>=0x477ff000)return asfloat(sign|0x7f800000);
    if(magnitude>=0x38800000)return asfloat(sign|((magnitude+4095+((magnitude>>13)&1))&0xffffe000));
    float scaled=asfloat(magnitude)*16777216.0,lo=floor(scaled),fraction=scaled-lo;
    float nearest=lo+((fraction>.5||(fraction==.5&&(uint(lo)&1)))?1.0:0.0);
    return asfloat(sign|asuint(nearest*5.960464477539063e-8));
}
float textureUnormRound(float x){
    precise float scaled=saturate(x)*255.0;
    float lo=floor(scaled),fraction=scaled-lo;
    float nearest=lo+((fraction>.5||(fraction==.5&&(uint(lo)&1)))?1.0:0.0);
    return nearest/255.0;
}
float textureUnorm10Round(float x){
    precise float scaled=saturate(x)*1023.0;
    float lo=floor(scaled),fraction=scaled-lo;
    float nearest=lo+((fraction>.5||(fraction==.5&&(uint(lo)&1)))?1.0:0.0);
    return nearest/1023.0;
}
[numthreads(8,8,1)]
void TextureToRGB(uint3 id:SV_DispatchThreadID){
    if(id.x>=ModelWidth||id.y>=ModelHeight)return;
    uint2 source=min(uint2((float2(id.xy)+0.5)*float2(Width,Height)/float2(ModelWidth,ModelHeight)),uint2(Width-1,Height-1));
    if(Tiled)source=uint2(reflectPixel(int(TileX)+int(id.x)-int(Halo),int(WorkWidth)),reflectPixel(int(TileY)+int(id.y)-int(Halo),int(WorkHeight)));
    float3 value=Tiled?workColour(source):InputTexture.Load(int3(source,0)).rgb;
#if GLOBAL_RESIDUAL
    // Area reduction: 3x3 at 1080p, 4x4 at 1440p. Fractional extents are
    // weighted too, so resizing never stretches the network's padded geometry.
    float2 begin=float2(id.xy)*float2(Width,Height)/float2(ModelWidth,ModelHeight);
    float2 end=float2(id.xy+1)*float2(Width,Height)/float2(ModelWidth,ModelHeight);
    precise float3 sum=0;
    for(uint y=uint(floor(begin.y));y<uint(ceil(end.y));++y)
        for(uint x=uint(floor(begin.x));x<uint(ceil(end.x));++x){
            float weight=max(0,min(end.x,float(x+1))-max(begin.x,float(x)))*max(0,min(end.y,float(y+1))-max(begin.y,float(y)));
            sum+=InputTexture.Load(int3(min(uint2(x,y),uint2(Width-1,Height-1)),0)).rgb*weight;
        }
    value=sum/((end.x-begin.x)*(end.y-begin.y));
#endif
    uint base=(id.y*ModelWidth+id.x)*3;
    Colour[base]=value.r;Colour[base+1]=value.g;Colour[base+2]=value.b;
}
[numthreads(8,8,1)]
void ClearAccumulation(uint3 id:SV_DispatchThreadID){
    if(id.x>=WorkWidth||id.y>=WorkHeight)return;uint b=(id.y*WorkWidth+id.x)*4;
    Accumulator[b]=0;Accumulator[b+1]=0;Accumulator[b+2]=0;Accumulator[b+3]=0;
}
float edgeWeight(uint p,uint origin,uint extent){
    uint overlap=Core-Stride;float weight=1;
    if(origin>0&&p<overlap)weight=min(weight,(float(p)+.5)/float(overlap));
    if(origin+Core<extent&&p>=Stride)weight=min(weight,(float(Core-p)-.5)/float(overlap));
    return weight;
}
[numthreads(8,8,1)]
void AccumulateTile(uint3 id:SV_DispatchThreadID){
    uint2 pixel=id.xy+uint2(TileX,TileY);
    if(id.x>=Core||id.y>=Core||pixel.x>=WorkWidth||pixel.y>=WorkHeight)return;
    uint model=((id.y+Halo)*ModelWidth+id.x+Halo)*3,b=(pixel.y*WorkWidth+pixel.x)*4;
    float weight=edgeWeight(id.x,TileX,WorkWidth)*edgeWeight(id.y,TileY,WorkHeight);
    Accumulator[b]+=(RGB[model]-Colour[model])*weight;
    Accumulator[b+1]+=(RGB[model+1]-Colour[model+1])*weight;
    Accumulator[b+2]+=(RGB[model+2]-Colour[model+2])*weight;
    Accumulator[b+3]+=weight;
}
[numthreads(8,8,1)]
void RGBToTexture(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height)return;
    uint base=(id.y*Width+id.x)*3;
    float3 value;
    float alpha=1.0;
    if(Tiled){
        float4 original=InputTexture.Load(int3(id.xy,0));alpha=original.a;
        float3 residual;
        if(WorkWidth==Width&&WorkHeight==Height)residual=workResidual(id.xy);
        else{
            uint2 a,b;float2 f;pixelCenterMap(id.xy,uint2(Width,Height),uint2(WorkWidth,WorkHeight),a,b,f);
            residual=lerp(lerp(workResidual(a),workResidual(uint2(b.x,a.y)),f.x),lerp(workResidual(uint2(a.x,b.y)),workResidual(b),f.x),f.y);
        }
        value=saturate(original.rgb+residual);
    }else if(Width==ModelWidth&&Height==ModelHeight){
        value=float3(RGB[base],RGB[base+1],RGB[base+2]);
    }else{
#if GLOBAL_RESIDUAL
        uint2 a,b;float2 f;pixelCenterMap(id.xy,uint2(Width,Height),uint2(ModelWidth,ModelHeight),a,b,f);
#else
        float2 p=clamp((float2(id.xy)+0.5)*float2(ModelWidth,ModelHeight)/float2(Width,Height)-0.5,0.0,float2(ModelWidth-1,ModelHeight-1));
        uint2 a=uint2(floor(p)),b=min(a+1,uint2(ModelWidth-1,ModelHeight-1));
        float2 f=frac(p);
#endif
        uint offsets[4]={(a.y*ModelWidth+a.x)*3,(a.y*ModelWidth+b.x)*3,(b.y*ModelWidth+a.x)*3,(b.y*ModelWidth+b.x)*3};
        float3 delta[4],guide[4];
        for(uint i=0;i<4;++i){uint o=offsets[i];guide[i]=float3(Colour[o],Colour[o+1],Colour[o+2]);delta[i]=float3(RGB[o],RGB[o+1],RGB[o+2])-guide[i];}
        float3 residual=lerp(lerp(delta[0],delta[1],f.x),lerp(delta[2],delta[3],f.x),f.y);
        float4 original=InputTexture.Load(int3(id.xy,0));
#if GLOBAL_RESIDUAL
        // TileX is unused in the non-tiled global path; use it as an opt-in
        // edge-guidance flag without new resources, descriptors or dispatches.
        if(TileX!=0){
            float3 lo=guide[0],hi=guide[0];
            [unroll]for(uint j=1;j<4;++j){lo=min(lo,guide[j]);hi=max(hi,guide[j]);}
            float3 range=hi-lo;float edge=smoothstep(.02,.08,max(range.x,max(range.y,range.z)));
            if(edge>0&&!(all(delta[0]==delta[1])&&all(delta[0]==delta[2])&&all(delta[0]==delta[3]))){
                float spatial[4]={(1-f.x)*(1-f.y),f.x*(1-f.y),(1-f.x)*f.y,f.x*f.y};
                float3 guided=0;float total=0;
                [unroll]for(uint j=0;j<4;++j){float3 diff=original.rgb-guide[j];float weight=spatial[j]/(1+dot(diff,diff)/.0009);guided+=delta[j]*weight;total+=weight;}
                if(total>1e-12)residual=lerp(residual,guided/total,edge);
            }
        }
#endif
        value=saturate(original.rgb+residual);alpha=original.a;
    }
#if OUTPUT_HALF
    value=float3(textureHalfRound(value.r),textureHalfRound(value.g),textureHalfRound(value.b));
#endif
#if OUTPUT_UNORM
    value=float3(textureUnormRound(value.r),textureUnormRound(value.g),textureUnormRound(value.b));
#endif
#if OUTPUT_UNORM10
    value=float3(textureUnorm10Round(value.r),textureUnorm10Round(value.g),textureUnorm10Round(value.b));
#endif
    OutputTexture[id.xy]=float4(value,alpha);
}
