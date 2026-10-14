#include "block_ops.hlsl"
cbuffer WindowShape : register(b1) {uint Height;uint Width;uint Top;uint Left;uint PaddedHeight;uint PaddedWidth;uint PublishOutput;uint HiddenChannels;uint SpatialChannels;uint DispatchElements;}
RWStructuredBuffer<float> ResidualOutput : register(u1);
uint linearIndex(uint3 id){return id.x+id.y*65535u*128u;}
[numthreads(128,1,1)]
void WindowPublish(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);uint index=linearIndex(id);if(index<DispatchElements)Output[NR_INDEX(index,3)]=publishE4(Source[NR_INDEX(index,0)]);}
[numthreads(128,1,1)]
void WindowGate(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id);if(index>=DispatchElements)return;
    precise float x=halfRound(Source[NR_INDEX(index,0)]),z=clamp(x,-4.0,4.0);
    precise float a=abs(z)*-0.055908203125;a=halfRound(a+0.447265625);
    precise float b=a*z;b=halfRound(b+0.89453125);
    float result=halfRound(x*b);
    Output[NR_INDEX(index,3)]=SpatialChannels==512 ? result : publishE4(result);
}
[numthreads(128,1,1)]
void WindowFeedResidual(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id);if(index>=Height*Width*SpatialChannels)return;
    precise float scaled=Skip[NR_INDEX(index,1)]*Parameter[NR_INDEX(index%SpatialChannels,2)];
    precise float result=Source[NR_INDEX(index,0)]+scaled;Output[NR_INDEX(index,3)]=SpatialChannels>32 ? publishE4(result) : result;
}
[numthreads(128,1,1)]
void WindowPack(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id);if(index>=PaddedHeight*PaddedWidth*SpatialChannels)return;
    uint c=index%SpatialChannels,token=(index/SpatialChannels)%64,window=index/(SpatialChannels*64);
    int y=int(window/(PaddedWidth/8)*8+token/8)-int(Top);
    int x=int(window%(PaddedWidth/8)*8+token%8)-int(Left);
    // Root SRVs have no bounds metadata: never speculate a load at a negative coordinate.
    [branch]if(y>=0 && x>=0 && y<int(Height) && x<int(Width)){
        Output[NR_INDEX(index,3)]=publishE4(Source[NR_INDEX((uint(y)*Width+uint(x))*SpatialChannels+c,0)]);
    }else Output[NR_INDEX(index,3)]=0;
}
[numthreads(128,1,1)]
void WindowFeedPack(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id);if(index>=DispatchElements)return;
    uint c=index%SpatialChannels,token=(index/SpatialChannels)%64,window=index/(SpatialChannels*64);
    int y=int(window/(PaddedWidth/8)*8+token/8)-int(Top);
    int x=int(window%(PaddedWidth/8)*8+token%8)-int(Left);
    [branch]if(y>=0 && x>=0 && y<int(Height) && x<int(Width)){
        uint source=(uint(y)*Width+uint(x))*SpatialChannels+c;
        precise float scaled=Skip[NR_INDEX(source,1)]*Parameter[NR_INDEX(c,2)];
        precise float result=Source[NR_INDEX(source,0)]+scaled;
        float feed=SpatialChannels>32?publishE4(result):result;
        // Every valid pixel has one window position. Preserve feed for the later residual.
#if NR_WINDOW_REUSE_FEED
        // C>32 feed is already published E4: windows is also the residual source.
        Output[NR_INDEX(index,3)]=feed;
#else
        ResidualOutput[NR_INDEX(source,4)]=feed;Output[NR_INDEX(index,3)]=publishE4(feed);
#endif
    }else Output[NR_INDEX(index,3)]=0;
}
groupshared float queryStage[128],keyStage[128],rowReciprocal[8];
groupshared float probabilityStage[128];
groupshared float queryPartial[32],keyPartial[32];
float windowPartial(uint base,uint c,bool query){
    float v0=query?queryStage[base+c]:keyStage[base+c];
    float v8=query?queryStage[base+c+8]:keyStage[base+c+8];
    float v16=query?queryStage[base+c+16]:keyStage[base+c+16];
    float v24=query?queryStage[base+c+24]:keyStage[base+c+24];
    precise float p0=v0*v0,p8=v8*v8,p16=v16*v16,p24=v24*v24;
    return halfRound(halfRound(p8+halfRound(p0))+halfRound(p24+halfRound(p16)));
}
float windowFinish(uint base,bool query){
    float partial[8],xorTwo[8],xorOne[8];
    [unroll]for(uint c=0;c<8;++c)partial[c]=query?queryPartial[base+c]:keyPartial[base+c];
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorTwo[l*2+p]=halfRound(partial[l*2+p]+partial[(l^2)*2+p]);}
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorOne[l*2+p]=halfRound(xorTwo[l*2+p]+xorTwo[(l^1)*2+p]);}
    float norm=max(halfRound(xorOne[0]+xorOne[1]),0.00006198883056640625);
    return halfRound(1.0/sqrt(norm));
}
float windowNorm(uint base,bool query){
    float partial[8],xorTwo[8],xorOne[8];
    [unroll]for(uint lane=0;lane<4;++lane){[unroll]for(uint parity=0;parity<2;++parity){
        uint c=lane*2+parity;
        float v0=query ? queryStage[base+c] : keyStage[base+c];
        float v8=query ? queryStage[base+c+8] : keyStage[base+c+8];
        float v16=query ? queryStage[base+c+16] : keyStage[base+c+16];
        float v24=query ? queryStage[base+c+24] : keyStage[base+c+24];
        precise float product0=v0*v0,product8=v8*v8,product16=v16*v16,product24=v24*v24;
        partial[c]=halfRound(halfRound(product8+halfRound(product0))+halfRound(product24+halfRound(product16)));
    }}
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorTwo[l*2+p]=halfRound(partial[l*2+p]+partial[(l^2)*2+p]);}
    [unroll]for(uint l=0;l<4;++l){[unroll]for(uint p=0;p<2;++p)xorOne[l*2+p]=halfRound(xorTwo[l*2+p]+xorTwo[(l^1)*2+p]);}
    float norm=max(halfRound(xorOne[0]+xorOne[1]),0.00006198883056640625);
    return halfRound(1.0/sqrt(norm));
}
[numthreads(128,1,1)]
void WindowNormalize(uint3 id:SV_DispatchThreadID,uint thread:SV_GroupIndex,uint3 group:SV_GroupID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),row=index/32,c=index%32,heads=SpatialChannels/32,rows=PaddedHeight*PaddedWidth*heads;
    // Window fields contain multiples of 64 rows, so no partial workgroup enters a barrier.
    if((group.x+group.y*65535u)*4>=rows)return;
    uint head=(row/64)%heads,window=row/(64*heads),token=row%64;
    uint source=(window*64+token)*SpatialChannels*3+head*32+c;
    queryStage[thread]=halfRound(Source[NR_INDEX(source,0)]);keyStage[thread]=halfRound(Source[NR_INDEX(source+SpatialChannels,0)]);
    GroupMemoryBarrierWithGroupSync();
    uint localRow=thread/32;
#if NR_WINDOW_PARALLEL_NORM
    if(c<8){queryPartial[localRow*8+c]=windowPartial(localRow*32,c,true);keyPartial[localRow*8+c]=windowPartial(localRow*32,c,false);}
    GroupMemoryBarrierWithGroupSync();
    if(c==0){rowReciprocal[localRow*2]=windowFinish(localRow*8,true);rowReciprocal[localRow*2+1]=windowFinish(localRow*8,false);}
#else
    if(c==0){rowReciprocal[localRow*2]=windowNorm(localRow*32,true);rowReciprocal[localRow*2+1]=windowNorm(localRow*32,false);}
#endif
    GroupMemoryBarrierWithGroupSync();
    float scale=halfRound(Parameter[NR_INDEX(head,2)]);
    Output[NR_INDEX(row*32+c,3)]=publishE4(halfRound(halfRound(queryStage[thread]*rowReciprocal[localRow*2])*scale));
    Output[NR_INDEX(rows*32+row*32+c,3)]=publishE4(halfRound(keyStage[thread]*rowReciprocal[localRow*2+1]));
    Output[NR_INDEX(rows*64+row*32+c,3)]=publishE4(Source[NR_INDEX(source+SpatialChannels*2,0)]);
}
[numthreads(128,1,1)]
void WindowSoftmax(uint3 id:SV_DispatchThreadID,uint thread:SV_GroupIndex,uint3 group:SV_GroupID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),row=index/64,lane=index%64,heads=SpatialChannels/32,rows=PaddedHeight*PaddedWidth*heads;
    if((group.x+group.y*65535u)*2>=rows)return;
    uint a=lane&~1u,b=a+1;
    uint biasBase=((row/64)%heads)*4096+(row%64)*64;
    precise float scoreA=Source[NR_INDEX(row*64+a,0)]+Parameter[NR_INDEX(biasBase+a,2)];
    precise float scoreB=Source[NR_INDEX(row*64+b,0)]+Parameter[NR_INDEX(biasBase+b,2)];
    precise float affineA=halfRound(scoreA)*0.044921875;affineA+=1.30078125;
    precise float affineB=halfRound(scoreB)*0.044921875;affineB+=1.30078125;
    uint packed=halfBits(clamp(affineA,1.03125,1.5693359375))|(halfBits(clamp(affineB,1.03125,1.5693359375))<<16);
    uint transformed=(packed<<5)+0x7FF88000;
    probabilityStage[thread]=f16tof32((lane&1) ? transformed>>16 : transformed&65535);
    GroupMemoryBarrierWithGroupSync();
    uint localRow=thread/64;
    if(lane==0){precise float total=0;[loop]for(uint k=0;k<64;++k)total+=probabilityStage[localRow*64+k];rowReciprocal[localRow]=halfRound(1.0/halfRound(total));}
    GroupMemoryBarrierWithGroupSync();
    Output[NR_INDEX(index,3)]=publishE4(halfRound(probabilityStage[thread]*rowReciprocal[localRow]));
}
[numthreads(128,1,1)]
void WindowCropResidual(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id);if(index>=Height*Width*SpatialChannels)return;
    uint channel=index%SpatialChannels,pixel=index/SpatialChannels,y=pixel/Width+Top,x=pixel%Width+Left;
    uint window=(y/8)*(PaddedWidth/8)+x/8,token=(y%8)*8+x%8;
#if NR_WINDOW_REUSE_FEED
    uint skipIndex=(window*64+token)*SpatialChannels+channel;
#else
    uint skipIndex=index;
#endif
    precise float scaled=Skip[NR_INDEX(skipIndex,1)]*Parameter[NR_INDEX(channel,2)];
    precise float result=Source[NR_INDEX((window*64+token)*SpatialChannels+channel,0)]+scaled;
    Output[NR_INDEX(index,3)]=PublishOutput ? publishE4(result) : result;
}

[numthreads(128,1,1)]
void WindowMergeContext(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id);if(index>=PaddedHeight*PaddedWidth*SpatialChannels)return;
    uint channel=index%SpatialChannels,token=(index/SpatialChannels)%64,window=index/(SpatialChannels*64);
    uint head=channel/32,heads=SpatialChannels/32;
    Output[NR_INDEX(index,3)]=publishE4(Source[NR_INDEX(((window*heads+head)*64+token)*32+channel%32,0)]);
}

// Literal branched FFN: keep each input-head GEMM and the reference's addition order.
// Diagnostic split contraction: half gate and E4M3 weights have products that
// fit FP32 exactly. TwoSum retains low sum bits before the next publication.
[numthreads(128,1,1)]
void SplitCompensatedProject(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),pixels=Height*Width;
    if(index>=DispatchElements)return;
    uint channel=index%64,pixel=(index/64)%pixels,group=index/(pixels*64);
    precise float total=0.0,correction=0.0;
    [loop]for(uint k=0;k<256;++k){
        precise float product=Source[NR_INDEX((group*pixels+pixel)*256+k,0)]*Parameter[NR_INDEX((group*256+k)*64+channel,2)];
        precise float next=total+product;
        precise float part=next-total;
        precise float error=(total-(next-part))+(product-part);
        correction+=error;total=next;
    }
    Output[NR_INDEX(index,3)]=total+correction;
}

[numthreads(128,1,1)]
void BranchPackInput(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),pixels=Height*Width,heads=SpatialChannels/32;
    if(index>=DispatchElements)return;
    uint channel=index%32,pixel=(index/32)%pixels,batch=index/(32*pixels),inputHead=batch%heads;
    Output[NR_INDEX(index,3)]=Source[NR_INDEX(pixel*SpatialChannels+inputHead*32+channel,0)];
}
[numthreads(128,1,1)]
void BranchSumInput(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),pixels=Height*Width,heads=SpatialChannels/32;
    if(index>=DispatchElements)return;
    uint within=index%(pixels*32),branch=index/(pixels*32);
    precise float total=Source[NR_INDEX(branch*heads*pixels*32+within,0)];
    [loop]for(uint head=1;head<heads;++head)total+=Source[NR_INDEX((branch*heads+head)*pixels*32+within,0)];
    Output[NR_INDEX(index,3)]=total;
}
[numthreads(128,1,1)]
void BranchSumGate(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),pixels=Height*Width,heads=SpatialChannels/32;
    if(index>=DispatchElements)return;
    uint within=index%(pixels*32),branch=index/(pixels*32);
    precise float total=Source[NR_INDEX(branch*heads*pixels*32+within,0)];
    [loop]for(uint head=1;head<heads;++head)total+=Source[NR_INDEX((branch*heads+head)*pixels*32+within,0)];
    // Preserve the same summation and gate rounding as the two separate passes.
    precise float x=halfRound(total),z=clamp(x,-4.0,4.0);
    precise float a=abs(z)*-0.055908203125;a=halfRound(a+0.447265625);
    precise float b=a*z;b=halfRound(b+0.89453125);
    Output[NR_INDEX(index,3)]=publishE4(halfRound(x*b));
}
[numthreads(128,1,1)]
void BranchSumOutput(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),pixels=Height*Width;if(index>=pixels*SpatialChannels)return;
    uint channel=index%SpatialChannels,pixel=index/SpatialChannels,head=channel/32;
    uint base=head*4*pixels*32+pixel*32+channel%32;
    precise float total=Source[NR_INDEX(base,0)];
    [unroll]for(uint branch=1;branch<4;++branch)total+=Source[NR_INDEX(base+branch*pixels*32,0)];
    Output[NR_INDEX(index,3)]=publishE4(total);
}

[numthreads(128,1,1)]
void SplitPackInput(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),pixels=Height*Width;
    if(index>=pixels*SpatialChannels)return;
    uint channel=index%64,pixel=(index/64)%pixels,group=index/(64*pixels);
    Output[NR_INDEX(index,3)]=Source[NR_INDEX(pixel*SpatialChannels+group*64+channel,0)];
}
[numthreads(128,1,1)]
void SplitMergeOutput(uint3 id:SV_DispatchThreadID){ NR_SET_LANE(id.z);
    uint index=linearIndex(id),pixels=Height*Width;
    if(index>=pixels*SpatialChannels)return;
    uint channel=index%SpatialChannels,pixel=index/SpatialChannels,group=channel/64;
    Output[NR_INDEX(index,3)]=publishE4(Source[NR_INDEX((group*pixels+pixel)*64+channel%64,0)]);
}
