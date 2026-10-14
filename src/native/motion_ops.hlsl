Texture2D<float2> EngineMotion : register(t0);
Texture2D<float> EngineDepth : register(t1);
Texture2D<float> EngineReactive : register(t2);
RWStructuredBuffer<float> NormalizedMotion : register(u0);
RWStructuredBuffer<float> Validity : register(u1);
RWStructuredBuffer<float> CurrentDepth : register(u2);
RWStructuredBuffer<uint> ReconstructedDepth : register(u3);
cbuffer MotionShape : register(b0) {
    uint Width;uint Height;uint MotionWidth;uint MotionHeight;
    uint DepthWidth;uint DepthHeight;uint InvertedDepth;uint ConventionValid;
    float ScaleX;float ScaleY;float JitterCorrectionX;float JitterCorrectionY;
    uint ReactiveWidth;uint ReactiveHeight;uint UseReactive;uint Reserved;
    float CameraNear;float CameraFar;float DepthTolerance;uint CameraRangeKnown;
}
[numthreads(8,8,1)]
void ClearProjectedDepth(uint3 id:SV_DispatchThreadID){if(id.x<Width&&id.y<Height)ReconstructedDepth[id.y*Width+id.x]=InvertedDepth!=0?0:0xffffffff;}
[numthreads(8,8,1)]
void NormalizeMotion(uint3 id:SV_DispatchThreadID) {
    if(id.x>=Width||id.y>=Height)return;
    float2 uv=(float2(id.xy)+.5)/float2(Width,Height);
    int2 center=min(int2(uv*float2(DepthWidth,DepthHeight)),int2(DepthWidth-1,DepthHeight-1));
    int2 selected=center;float depth=EngineDepth.Load(int3(center,0));
    // Select a foreground motion sample, never average across surface boundaries.
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        int2 p=clamp(center+int2(x,y),0,int2(DepthWidth-1,DepthHeight-1));
        float z=EngineDepth.Load(int3(p,0));
        if(isfinite(z)&&(!isfinite(depth)||(InvertedDepth!=0?z>depth:z<depth))){depth=z;selected=p;}
    }
    float2 sampleUV=(float2(selected)+.5)/float2(DepthWidth,DepthHeight);
    int2 mp=min(int2(sampleUV*float2(MotionWidth,MotionHeight)),int2(MotionWidth-1,MotionHeight-1));
    float2 mv=EngineMotion.Load(int3(mp,0))*float2(ScaleX,ScaleY)+float2(JitterCorrectionX,JitterCorrectionY);
    bool good=ConventionValid!=0&&isfinite(depth)&&depth>=0&&depth<=1&&(InvertedDepth!=0?depth>0:depth<1)&&all(isfinite(mv))&&all(abs(mv)<=1);
    uint i=id.y*Width+id.x;
    NormalizedMotion[i*2]=good?mv.x:0;NormalizedMotion[i*2+1]=good?mv.y:0;
    CurrentDepth[i]=good?depth:-1;
    // This is geometric input validity, not a previous-depth disocclusion test.
    // The temporal pass additionally clamps reprojected colour to the current neighborhood.
    float reactive=0;
    if(UseReactive!=0){int2 rp=min(int2(uv*float2(ReactiveWidth,ReactiveHeight)),int2(ReactiveWidth-1,ReactiveHeight-1));reactive=EngineReactive.Load(int3(rp,0));}
    Validity[i]=good&&isfinite(reactive)?1-saturate(reactive):0;
}
// Forward-splat CURRENT depth using current->previous motion, as in the bundled
// FSR reconstructed-depth method. Every compared Z belongs to the same camera
// frame: do not compare raw previous-frame Z under a moving camera.
[numthreads(8,8,1)]
void ProjectCurrentDepth(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height)return;uint i=id.y*Width+id.x;float z=CurrentDepth[i];if(z<0)return;
    float2 uv=(float2(id.xy)+.5)/float2(Width,Height)+float2(NormalizedMotion[i*2],NormalizedMotion[i*2+1]);
    float2 pixel=uv*float2(Width,Height)-.5;int2 base=int2(floor(pixel));float2 f=frac(pixel);
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){int2 p=base+int2(x,y);float weight=(x?f.x:1-f.x)*(y?f.y:1-f.y);
        if(all(p>=0)&&all(p<int2(Width,Height))&&weight>.01){uint unused;uint j=p.y*Width+p.x;
            if(InvertedDepth!=0)InterlockedMax(ReconstructedDepth[j],asuint(z),unused);else InterlockedMin(ReconstructedDepth[j],asuint(z),unused);}
    }
}
float viewDistance(float z){
    if(CameraRangeKnown!=0){float ratio=CameraNear/CameraFar;return CameraNear/max(InvertedDepth!=0?z+(1-z)*ratio:1-z*(1-ratio),1e-8);}
    return 1/max(InvertedDepth!=0?z:1-z,1e-6);
}
[numthreads(8,8,1)]
void RejectOccludedHistory(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height)return;uint i=id.y*Width+id.x;if(Validity[i]<=0)return;
    float2 uv=(float2(id.xy)+.5)/float2(Width,Height)+float2(NormalizedMotion[i*2],NormalizedMotion[i*2+1]);
    if(any(uv<0)||any(uv>1)){Validity[i]=0;return;}
    uint2 p=min(uint2(uv*float2(Width,Height)),uint2(Width-1,Height-1));uint projected=ReconstructedDepth[p.y*Width+p.x];
    if(projected==(InvertedDepth!=0?0:0xffffffff)){Validity[i]=0;return;}
    float expected=viewDistance(asfloat(projected)),current=viewDistance(CurrentDepth[i]);
    if(current>expected*(1+DepthTolerance))Validity[i]=0;
}
