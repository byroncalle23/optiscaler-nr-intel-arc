#include "block_ops.hlsl"
cbuffer TargetBounds : register(b1){uint TargetElements;}

float transitionTap(uint y,uint x,uint channel){
    [branch]if(y<Tokens && x<Channels)return Source[NR_INDEX((y*Channels+x)*Count+channel,0)];
    return 0;
}

// Pool constants: source height, source width, channels, padding multiple.
[numthreads(128,1,1)]
void PoolPublish(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    uint index=id.x+id.y*65535u*128u;
    uint multiple=max(Publish,2u);
    uint height=((Tokens+multiple-1)/multiple*multiple)/2;
    uint width=((Channels+multiple-1)/multiple*multiple)/2;
    if(index>=height*width*Count)return;
    uint channel=index%Count, pixel=index/Count;
    uint y=pixel/width*2, x=pixel%width*2;
    precise float a=transitionTap(y,x,channel),b=transitionTap(y+1,x,channel);
    precise float c=transitionTap(y,x+1,channel),d=transitionTap(y+1,x+1,channel);
    precise float sum=((a+b)+c)+d;
    Output[NR_INDEX(index,3)]=publishE4(sum*.25);
}

// Merge constants: source width, channels, target width, publication flag.
[numthreads(128,1,1)]
void UpsampleMerge(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    uint index=id.x+id.y*65535u*128u;
    if(index>=TargetElements)return;
    uint channel=index%Channels, pixel=index/Channels;
    uint y=pixel/Count, x=pixel%Count;
    precise float scaled=Skip[NR_INDEX(index,1)]*Parameter[NR_INDEX(channel,2)];
    precise float merged=Source[NR_INDEX(((y/2)*Tokens+x/2)*Channels+channel,0)]+scaled;
    Output[NR_INDEX(index,3)]=Publish ? publishE4(merged) : merged;
}

[numthreads(128,1,1)]
void TransitionPublish(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    uint index=id.x+id.y*65535u*128u;
    if(index<Count)Output[NR_INDEX(index,3)]=publishE4(Source[NR_INDEX(index,0)]);
}
