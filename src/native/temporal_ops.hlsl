// All motion inputs are verified current -> previous normalized UV offsets.
// Engine depth/disocclusion validation supplies Confidence; it is not guessed here.
StructuredBuffer<float> Current : register(t0);
StructuredBuffer<float> Noise : register(t1);
StructuredBuffer<float> Head : register(t2);
StructuredBuffer<float> History : register(t3);
StructuredBuffer<float> Motion : register(t4);
StructuredBuffer<float> Confidence : register(t5);
StructuredBuffer<float> PreviousBase : register(t6);
StructuredBuffer<float> PreviousResidual : register(t7);
RWStructuredBuffer<float> Reprojected : register(u0); // RGB + validity
RWStructuredBuffer<float> Features : register(u1);
RWStructuredBuffer<float> NextHistory : register(u2);
RWStructuredBuffer<float> Composed : register(u3);
RWStructuredBuffer<float> NextBase : register(u4);
RWStructuredBuffer<float> NextResidual : register(u5);
RWStructuredBuffer<float> StabilityTrace : register(u6);
cbuffer TemporalShape : register(b0) {
    uint Width; uint Height; uint PaddedWidth; uint PaddedHeight;
    uint HistoryValid; uint Reset; float ClampMargin; uint ClampEnabled;
    float ResidualStability; uint BoundedResidual; uint TraceEnabled; uint HistoryDamping;
}
// Match the accepted frame adapter's round-to-nearest-even contract explicitly.
float halfRound(float x) {
    uint bits=asuint(x),sign=bits&0x80000000,magnitude=bits&0x7fffffff;
    if(magnitude>=0x7f800000)return x;
    if(magnitude>=0x477ff000)return asfloat(sign|0x7f800000);
    if(magnitude>=0x38800000)return asfloat(sign|((magnitude+4095+((magnitude>>13)&1))&0xffffe000));
    float scaled=asfloat(magnitude)*16777216.0,lo=floor(scaled),fraction=scaled-lo;
    float nearest=lo+((fraction>.5||(fraction==.5&&(uint(lo)&1)))?1.0:0.0);
    return asfloat(sign|asuint(nearest*5.960464477539063e-8));
}
float3 currentAt(uint2 p) { uint i=(p.y*Width+p.x)*3; return float3(Current[i],Current[i+1],Current[i+2]); }
float3 historyAt(uint2 p) { uint i=(p.y*Width+p.x)*3; return float3(History[i],History[i+1],History[i+2]); }
float3 linearHistory(float2 center) {
    float2 pixel=clamp(center-.5,0,float2(Width-1,Height-1));
    uint2 a=uint2(floor(pixel)),b=min(a+1,uint2(Width-1,Height-1));float2 f=frac(pixel);
    return lerp(lerp(historyAt(a),historyAt(uint2(b.x,a.y)),f.x),lerp(historyAt(uint2(a.x,b.y)),historyAt(b),f.x),f.y);
}
void catmullAxis(float normalized,uint extent,out float3 positions,out float3 weights) {
    float pixel=normalized*extent-.5,base=floor(pixel),t=pixel-base,t2=t*t,t3=t2*t;
    float w0=-.5*t+t2-.5*t3,w1=1-2.5*t2+1.5*t3,w2=.5*t+2*t2-1.5*t3,w3=-.5*t2+.5*t3,g=w1+w2;
    positions=clamp(base+.5+float3(-1,w2/g,2),.5,float(extent)-.5);
    weights=float3(w0,g,w3);
}
float3 catmullHistory(float2 uv) {
    float3 x,y,wx,wy;catmullAxis(uv.x,Width,x,wx);catmullAxis(uv.y,Height,y,wy);
    float a=wx.x*wy.y,b=wx.y*wy.x,c=wx.y*wy.y,d=wx.y*wy.z,e=wx.z*wy.y;
    return (linearHistory(float2(x.x,y.y))*a+linearHistory(float2(x.y,y.x))*b+
            linearHistory(float2(x.y,y.y))*c+linearHistory(float2(x.y,y.z))*d+
            linearHistory(float2(x.z,y.y))*e)/(a+b+c+d+e);
}
uint mirror(uint p,uint extent) { return p<extent?p:uint(max(0,int(2*extent-2)-int(p))); }
float3 stabilitySample(float2 uv,bool residual);
[numthreads(8,8,1)]
void Reproject(uint3 id:SV_DispatchThreadID) {
    if(id.x>=Width||id.y>=Height)return;uint p=id.y*Width+id.x;
    float2 mv=float2(Motion[p*2],Motion[p*2+1]),uv=(float2(id.xy)+.5)/float2(Width,Height)+mv;
    float confidence=Confidence[p];
    bool valid=HistoryValid!=0&&Reset==0&&all(isfinite(mv))&&isfinite(confidence)&&
               all(uv>=0)&&all(uv<=1)&&confidence>0;
    float3 colour=currentAt(id.xy),history=colour;
    if(valid) {
        history=catmullHistory(uv);
        if(!all(isfinite(history)))valid=false;
        else if(ClampEnabled!=0) {
            float3 lo=colour,hi=colour;
            [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
                float3 v=currentAt(uint2(clamp(int2(id.xy)+int2(x,y),0,int2(Width-1,Height-1))));
                lo=min(lo,v);hi=max(hi,v);
            }
            history=clamp(history,lo-ClampMargin,hi+ClampMargin);
        }
    }
    if(!valid)history=colour;
    Reprojected[p*4]=history.r;Reprojected[p*4+1]=history.g;Reprojected[p*4+2]=history.b;
    Reprojected[p*4+3]=valid?saturate(confidence):0;
}
[numthreads(8,8,1)]
void BuildFeatures(uint3 id:SV_DispatchThreadID) {
    if(id.x>=PaddedWidth||id.y>=PaddedHeight)return;
    uint p=id.y*PaddedWidth+id.x,q=mirror(id.y,Height)*Width+mirror(id.x,Width);
    [unroll]for(uint c=0;c<16;++c) {
        float value=0;
        if(c<3)value=Noise[p*3+c];
        else if(c==3||c==11||c==12)value=1;
        else if(c>=4&&c<7)value=halfRound(halfRound(halfRound(Current[q*3+c-4])-.5)*.125);
        else if(c>=7&&c<10){
            float history=Reprojected[q*4+c-7];
#ifdef NR_HISTORY_EXPERIMENTS
            if(HistoryDamping>=2&&HistoryValid!=0&&Reset==0&&Reprojected[q*4+3]>.9){
                uint2 pixel=uint2(q%Width,q/Width);float2 mv=float2(Motion[q*2],Motion[q*2+1]);
                float2 uv=(float2(pixel)+.5)/float2(Width,Height)+mv;
                if(all(isfinite(mv))&&all(uv>=0)&&all(uv<=1))history=lerp(history,stabilitySample(uv,false)[c-7],HistoryDamping==2?1:.25);
            }
#endif
            value=halfRound(halfRound(halfRound(history)-.5)*.125);
        }
        else if(c==13||c==14)value=-1;
        Features[p*16+c]=value;
    }
}
float3 stabilityAt(uint2 p,bool residual) {
    uint i=(p.y*Width+p.x)*3;
    return residual?float3(PreviousResidual[i],PreviousResidual[i+1],PreviousResidual[i+2]):
                    float3(PreviousBase[i],PreviousBase[i+1],PreviousBase[i+2]);
}
float3 stabilitySample(float2 uv,bool residual) {
    float2 p=clamp(uv*float2(Width,Height)-.5,0,float2(Width-1,Height-1));
    uint2 a=uint2(floor(p)),b=min(a+1,uint2(Width-1,Height-1));float2 f=p-a;
    return lerp(lerp(stabilityAt(a,residual),stabilityAt(uint2(b.x,a.y),residual),f.x),
                lerp(stabilityAt(uint2(a.x,b.y),residual),stabilityAt(b,residual),f.x),f.y);
}
float maxAbs(float3 v){v=abs(v);return max(v.x,max(v.y,v.z));}
[numthreads(8,8,1)]
void ComposeTemporal(uint3 id:SV_DispatchThreadID) {
    if(id.x>=Width||id.y>=Height)return;
    uint p=id.y*Width+id.x,h=(id.y*PaddedWidth+id.x)*4;
    float logit=halfRound(Head[h+3]);
    float alpha=isfinite(logit)?saturate((1/(1+exp(-logit)))*.73974609375)*Reprojected[p*4+3]:0;
    float3 result;
    [unroll]for(uint c=0;c<3;++c) {
        float current=Current[p*3+c],prediction=saturate(current+halfRound(Head[h+c])*.25);
        // Decode the actual quantized history channels presented to the network.
        float history=Features[(id.y*PaddedWidth+id.x)*16+7+c]*8+.5;
#ifdef NR_HISTORY_EXPERIMENTS
        if(HistoryDamping>=2)history=Reprojected[p*4+c];
#endif
        float value=saturate(prediction+alpha*(history-prediction));
        result[c]=value;
    }
    float3 base=currentAt(id.xy),residual=result-base;
    float4 trace=float4(0,0,-1,-1);
    if(ResidualStability>0&&HistoryValid!=0&&Reset==0&&Reprojected[p*4+3]>.9) {
        float2 mv=float2(Motion[p*2],Motion[p*2+1]),uv=(float2(id.xy)+.5)/float2(Width,Height)+mv;
        float pixels=length(mv*float2(Width,Height));
        if(all(isfinite(mv))&&all(uv>=0)&&all(uv<=1)&&pixels<(BoundedResidual==3?8:.75)) {
            float3 previousBase=stabilitySample(uv,false),previousResidual=stabilitySample(uv,true);
            if(all(isfinite(previousBase))&&all(isfinite(previousResidual))) {
                float residualChange=maxAbs(residual-previousResidual),baseChange=maxAbs(base-previousBase);
                float weight=ResidualStability*(1-smoothstep(.25,.75,pixels))*
                    (1-smoothstep(.005,.02,baseChange));
                float legacyWeight=weight*(1-smoothstep(.01,.025,residualChange));
                if(BoundedResidual==2||BoundedResidual==3){
                    // Stronger residual stabilization is reserved for coherent,
                    // nearly stationary neighborhoods. Do not retain residuals
                    // across hair/surface motion boundaries or rejected history.
                    float maxMotion=pixels,minConfidence=Reprojected[p*4+3],dispersion=0;
                    float3 meanCurrent=0,meanPrevious=0;
                    [unroll]for(int dy=-1;dy<=1;++dy)[unroll]for(int dx=-1;dx<=1;++dx){
                        uint2 n=uint2(clamp(int2(id.xy)+int2(dx,dy),0,int2(Width-1,Height-1)));uint q=n.y*Width+n.x;
                        float2 other=float2(Motion[q*2],Motion[q*2+1]);
                        if(!all(isfinite(other)))minConfidence=0;
                        else {
                            maxMotion=max(maxMotion,length(other*float2(Width,Height)));
                            dispersion=max(dispersion,length((other-mv)*float2(Width,Height)));
                            if(BoundedResidual==3){
                                float2 priorUV=(float2(n)+.5)/float2(Width,Height)+other;
                                if(any(priorUV<0)||any(priorUV>1))minConfidence=0;
                                meanCurrent+=currentAt(n);meanPrevious+=stabilitySample(priorUV,false);
                            }
                        }
                        minConfidence=min(minConfidence,Reprojected[q*4+3]);
                    }
                    weight=min(.75,ResidualStability*3)*(1-smoothstep(.05,.25,maxMotion))*
                        (1-smoothstep(.005,.02,baseChange))*smoothstep(.9,1,minConfidence);
                    if(BoundedResidual==3){
                        // A coherent camera translation is reprojectable. Retain
                        // the residual on that surface; reject motion boundaries.
                        float moving=smoothstep(.05,.25,maxMotion);
                        float coherent=(1-smoothstep(.05,.25,dispersion))*(1-smoothstep(4,8,maxMotion));
                        float appearance=lerp(baseChange,maxAbs((meanCurrent-meanPrevious)/9),moving);
                        weight=min(.75,ResidualStability*3)*lerp(1,coherent,moving)*
                            (1-smoothstep(.005,.02,appearance))*smoothstep(.9,1,minConfidence);
                    }
                }
                if(BoundedResidual==0)weight=legacyWeight;
                trace=float4(weight,legacyWeight,residualChange,baseChange);
                // With trusted stable input, a large model jump is not itself
                // proof of motion. Bound the correction by the current signal
                // instead: at strength .25, <=1% of the local peak RGB.
                float limit=BoundedResidual!=0?min(.02,maxAbs(result)*.04):.02;
                if(BoundedResidual==2||BoundedResidual==3)limit=min(.04,maxAbs(result)*.02);
                // Smooth only the small changing NR residual, never the base image.
                residual=lerp(residual,clamp(previousResidual,residual-limit,residual+limit),weight);
                result=saturate(base+residual);
            }
        }
    }
    if(TraceEnabled!=0){[unroll]for(uint i=0;i<4;++i)StabilityTrace[p*4+i]=trace[i];}
    [unroll]for(uint c=0;c<3;++c){
        NextBase[p*3+c]=base[c];NextResidual[p*3+c]=result[c]-base[c];
        // Opt-in experiment: damp feedback only in validated stable areas.
        // The displayed residual stays on the existing composition route.
        NextHistory[p*3+c]=result[c];
#ifdef NR_HISTORY_EXPERIMENTS
        float feedbackWeight=HistoryDamping==1?trace.x:0;
        NextHistory[p*3+c]=lerp(result[c],Reprojected[p*4+c],min(1,feedbackWeight*3));
#endif
        Composed[p*3+c]=result[c];
    }
}
