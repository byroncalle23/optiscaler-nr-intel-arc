#define NR_WINDOW_REUSE_FEED 1
#include "window_ops.hlsl"
// One owner per adjacent half pair; raw FP32 remains in the first region.
[numthreads(128,1,1)]
void DenseWindowPublish(uint3 id:SV_DispatchThreadID){
    uint i=linearIndex(id);if(i>=DispatchElements)return;
    Output[i]=publishE4(Source[i]);
    if((i&1)==0)Output[DispatchElements+i/2]=asfloat(f32tof16(publishE4(Source[i]))|(f32tof16(publishE4(Source[i+1]))<<16));
}
[numthreads(128,1,1)]
void DenseWindowFeedPack(uint3 id:SV_DispatchThreadID){
    uint i=linearIndex(id);if(i>=DispatchElements)return;
    uint c=i%SpatialChannels,token=(i/SpatialChannels)%64,window=i/(SpatialChannels*64);
    int y=int(window/(PaddedWidth/8)*8+token/8)-int(Top),x=int(window%(PaddedWidth/8)*8+token%8)-int(Left);
    float value=0,next=0;
    [branch]if(y>=0&&x>=0&&y<int(Height)&&x<int(Width)){
        uint source=(uint(y)*Width+uint(x))*SpatialChannels+c;
        precise float scaled=Skip[source]*Parameter[c];precise float result=Source[source]+scaled;value=publishE4(result);
        if((i&1)==0){precise float scaledNext=Skip[source+1]*Parameter[c+1];precise float resultNext=Source[source+1]+scaledNext;next=publishE4(resultNext);}
    }
    Output[i]=value;
    if((i&1)==0)Output[DispatchElements+i/2]=asfloat(f32tof16(value)|(f32tof16(next)<<16));
}
[numthreads(128,1,1)]
void DenseSplitMergeOutput(uint3 id:SV_DispatchThreadID){
    uint i=linearIndex(id),pixels=Height*Width;if(i>=pixels*SpatialChannels)return;
    uint c=i%SpatialChannels,pixel=i/SpatialChannels,group=c/64,source=(group*pixels+pixel)*64+c%64;
    float value=publishE4(Source[source]);Output[i]=value;
    if((i&1)==0)Output[DispatchElements+i/2]=asfloat(f32tof16(value)|(f32tof16(publishE4(Source[source+1]))<<16));
}
