// Included after block_ops.hlsl's shared rounding functions and resource declarations.
[numthreads(128,1,1)]
void PackHeads(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    if(id.x>=Count)return;
    uint padded=(Tokens+63)/64*64, qSize=Tokens*Channels, plane=padded*Channels;
    uint part=id.x<qSize?0u:(id.x<qSize+plane?1u:2u);
    uint localIndex=id.x-(part==0?0u:(part==1?qSize:qSize+plane));
    uint rows=part==0?Tokens:padded;
    uint head=localIndex/(rows*32), token=(localIndex/32)%rows, channel=localIndex%32;
    Output[NR_INDEX(id.x,3)]=token<Tokens ? Source[NR_INDEX(token*Channels*3+part*Channels+head*32+channel,0)] : 0;
}
groupshared float SharedWeights[64];
groupshared float SharedColumns[8];
[numthreads(64,1,1)]
void SoftmaxWeights(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) { NR_SET_LANE(group.z);
    uint rows=Tokens*(Channels/32), row=group.x, lane=thread.x;
    if(row>=rows)return;
    uint padded=(Tokens+63)/64*64;
    float total=0;
    [loop] for(uint first=0;first<padded;first+=64) {
        float w=weight(Source[NR_INDEX(row*padded+first+lane,0)]);
        SharedWeights[lane]=w;Output[NR_INDEX(row*padded+first+lane,3)]=publishE4(w);
        GroupMemoryBarrierWithGroupSync();
        if(lane<8){
            float a=halfRound(SharedWeights[lane]+SharedWeights[lane+8]);
            float b=halfRound(SharedWeights[lane+16]+SharedWeights[lane+24]);
            float c=halfRound(SharedWeights[lane+32]+SharedWeights[lane+40]);
            float d=halfRound(SharedWeights[lane+48]+SharedWeights[lane+56]);
            SharedColumns[lane]=halfRound(halfRound(halfRound(a+b)+c)+d);
        }
        GroupMemoryBarrierWithGroupSync();
        if(lane==0){
            float even=halfRound(halfRound(halfRound(SharedColumns[0]+SharedColumns[2])+SharedColumns[4])+SharedColumns[6]);
            float odd=halfRound(halfRound(halfRound(SharedColumns[1]+SharedColumns[3])+SharedColumns[5])+SharedColumns[7]);
            total=halfRound(total+halfRound(even+odd));
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0){
        if(padded>Tokens)total=halfRound(total-halfRound(weight(0)*float(padded-Tokens)));
        Output[NR_INDEX(rows*padded+row,3)]=halfRound(1.0/total);
    }
}
[numthreads(128,1,1)]
void MergeContext(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    if(id.x>=Count)return;
    uint token=id.x/Channels, channel=id.x%Channels, head=channel/32, localChannel=channel%32;
    uint row=head*Tokens+token, padded=(Tokens+63)/64*64;
    float reciprocal=Parameter[NR_INDEX(Tokens*(Channels/32)*padded+row,2)];
    Output[NR_INDEX(id.x,3)]=publishE4(halfRound(halfRound(Source[NR_INDEX(row*32+localChannel,0)])*reciprocal));
}
