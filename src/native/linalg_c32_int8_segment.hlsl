// Offline complete C32 FFN. Quantize at producers; consumers load native I8 bytes.
#include "linalg_branch_compact.hlsl"
ByteAddressBuffer FfnCos:register(t3);
cbuffer WindowGeometry:register(b1){uint Width;uint Height;uint Top;uint Left;uint PaddedWidth;}
float stepFor(float maximum){return max(maximum/127.0,1e-20);}
float quant(float x,float step){x=clamp(x/step,-127.0,127.0);float lo=floor(x),f=x-lo;return lo+float(f>.5||(f==.5&&(int(lo)&1)));}
[numthreads(128,1,1)]
void PackI8Weights(uint3 id:SV_DispatchThreadID){
    uint nCount=Groups,kCount=4096/nCount,col=id.x;if(col>=nCount)return;
    float maximum=0;for(uint k=0;k<kCount;++k)maximum=max(maximum,abs(asfloat(Input.Load((k*nCount+col)*4))));
    Compact.Store(4096+col*4,asuint(stepFor(maximum)));
    // Each four-byte word belongs to one thread, avoiding byte-level write races.
    // Pack in a separate setup dispatch after scales are visible.
}
[numthreads(128,1,1)]
void PackI8Bytes(uint3 id:SV_DispatchThreadID){
    uint word=id.x;if(word>=1024)return;
    uint nCount=Groups,kCount=4096/nCount,kTiles=kCount/32;
    uint component=word*4,tile=component/512,local=component%512;
    uint k=(tile%kTiles)*32+local/16,n=(tile/kTiles)*16+local%16,packed=0;
    for(uint j=0;j<4;++j){float step=asfloat(Compact.Load(4096+(n+j)*4));
        packed|=(uint(int(quant(asfloat(Input.Load((k*nCount+n+j)*4)),step)))&255)<<(j*8);}
    Compact.Store(word*4,packed);
}
groupshared float16_t Shared[256];
groupshared float RowScale[8];
using FA=Matrix<ComponentType::F16,8,32,MatrixUse::A,MatrixScope::Wave>;
using IA=Matrix<ComponentType::I8,8,32,MatrixUse::A,MatrixScope::Wave>;
using IB=Matrix<ComponentType::I8,32,16,MatrixUse::B,MatrixScope::Wave>;
using IC=Matrix<ComponentType::I32,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
void quantShared(uint lane){
    if(lane<8){float maximum=0;for(uint c=0;c<32;++c)maximum=max(maximum,abs(float(Shared[lane*32+c])));RowScale[lane]=stepFor(maximum);}
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<256;i+=16)Shared[i]=float16_t(quant(float(Shared[i]),RowScale[i/32]));
    GroupMemoryBarrierWithGroupSync();
}
[WaveSize(16)] [numthreads(16,1,1)]
void PublishI8(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint rt=group.x+group.y*4096;if(rt>=Pixels/8)return;
    for(uint i=lane;i<256;i+=16)Shared[i]=float16_t(publishE4(asfloat(Input.Load((rt*256+i)*4))));
    GroupMemoryBarrierWithGroupSync();quantShared(lane);
    FA f=FA::Load(Shared,0,32,MatrixLayout::RowMajor);IA a=f.Cast<ComponentType::I8>();
    a.Store(Compact,rt*256,32,MatrixLayout::RowMajor);
    if(lane<8)Compact.Store(Pixels*32+(rt*8+lane)*4,asuint(RowScale[lane]));
}
[WaveSize(16)] [numthreads(16,1,1)]
void ExpandI8(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint rt=group.x+(group.y/4)*4096,nt=group.y%4;if(rt>=Pixels/8)return;
    IA a=IA::Load(Input,rt*256,32,MatrixLayout::RowMajor);
    for(uint part=0;part<2;++part){uint tile=nt*2+part;IB b=IB::Load(ExpandWeights,tile*512,16,MatrixLayout::RowMajor);
        IC c=IC::Splat(0);c.MultiplyAccumulate(a,b);
        for(uint e=0;e<c.Length();++e){uint2 xy=c.GetCoordinate(e);float scale=asfloat(Input.Load(Pixels*32+(rt*8+xy.x)*4))*asfloat(ExpandWeights.Load(4096+(tile*16+xy.y)*4));
            Shared[xy.x*32+part*16+xy.y]=float16_t(gate(float(c.Get(e))*scale));}}
    GroupMemoryBarrierWithGroupSync();quantShared(lane);
    FA f=FA::Load(Shared,0,32,MatrixLayout::RowMajor);IA packed=f.Cast<ComponentType::I8>();
    packed.Store(Compact,(rt*4+nt)*256,32,MatrixLayout::RowMajor);
    if(lane<8)Compact.Store(Pixels*128+((rt*4+nt)*8+lane)*4,asuint(RowScale[lane]));
}
[WaveSize(16)] [numthreads(16,1,1)]
void ProjectI8Feed(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint rt=group.x+(group.y/2)*4096,nt=group.y%2;if(rt>=Words/8)return;
    for(uint i=lane;i<128;i+=16){uint pp=rt*8+i/16,window=pp/64,token=pp%64;
        int y=int(window/(PaddedWidth/8)*8+token/8)-int(Top),x=int(window%(PaddedWidth/8)*8+token%8)-int(Left);
        if(y<0||x<0||y>=int(Height)||x>=int(Width))Output.Store((pp*32+nt*16+i%16)*4,0);}
    if(rt>=Pixels/8)return;
    precise float total[8];
    for(uint chunk=0;chunk<4;++chunk){IA a=IA::Load(Input,(rt*4+chunk)*256,32,MatrixLayout::RowMajor);
        IB b=IB::Load(ProjectWeights,(nt*4+chunk)*512,16,MatrixLayout::RowMajor);IC c=IC::Splat(0);c.MultiplyAccumulate(a,b);
        for(uint e=0;e<8;++e){uint2 xy=c.GetCoordinate(e);float scale=asfloat(Input.Load(Pixels*128+((rt*4+chunk)*8+xy.x)*4))*asfloat(ProjectWeights.Load(4096+(nt*16+xy.y)*4));
            float value=float(c.Get(e))*scale;if(chunk==0)total[e]=value;else total[e]+=value;}}
    IC coords=IC::Splat(0);
    for(uint e=0;e<8;++e){uint2 xy=coords.GetCoordinate(e);uint pixel=rt*8+xy.x,channel=nt*16+xy.y,source=pixel*32+channel;
        precise float scaled=asfloat(ExpandWeights.Load(source*4))*asfloat(FfnCos.Load(channel*4));precise float feed=total[e]+scaled;
        uint y=pixel/Width+Top,x=pixel%Width+Left,window=(y/8)*(PaddedWidth/8)+x/8,token=(y%8)*8+x%8;
        Compact.Store(source*4,asuint(feed));Output.Store(((window*64+token)*32+channel)*4,asuint(publishE4(feed)));}
}
