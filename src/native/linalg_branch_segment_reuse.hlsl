// Whole branched FFN: publish while loading, reuse A across two output branches.
// Contraction consumes the exact accepted packed gate format.
#include "linalg_branch_compact.hlsl"
[WaveSize(16)] [numthreads(16,1,1)]
void ExpandPublishPair(uint3 group:SV_GroupID){
    using A=Matrix<ComponentType::F16,8,16,MatrixUse::A,MatrixScope::Wave>;
    using B=Matrix<ComponentType::F16,16,16,MatrixUse::B,MatrixScope::Wave>;
    using C=Matrix<ComponentType::F16,8,16,MatrixUse::Accumulator,MatrixScope::Wave>;
    uint rt=group.x,branch=group.y*2;
    precise float s0[8],s1[8],s2[8],s3[8];
    for(uint head=0;head<Heads;++head){
        C c0=C::Splat(0.0f),c1=C::Splat(0.0f),c2=C::Splat(0.0f),c3=C::Splat(0.0f);
        for(uint kt=0;kt<2;++kt){
            A a=A::Splat(0.0f);
            for(uint e=0;e<8;++e){uint2 xy=a.GetCoordinate(e);uint index=((rt*8+xy.x)*Heads+head)*32+kt*16+xy.y;
                a.Set(e,float16_t(publishE4(asfloat(Input.Load(index*4)))));}
            B b0=B::Load(ExpandWeights,((branch*Heads+head)*4+kt)*512,32,MatrixLayout::RowMajor);
            B b1=B::Load(ExpandWeights,((branch*Heads+head)*4+2+kt)*512,32,MatrixLayout::RowMajor);
            B b2=B::Load(ExpandWeights,(((branch+1)*Heads+head)*4+kt)*512,32,MatrixLayout::RowMajor);
            B b3=B::Load(ExpandWeights,(((branch+1)*Heads+head)*4+2+kt)*512,32,MatrixLayout::RowMajor);
            c0.MultiplyAccumulate(a,b0);c1.MultiplyAccumulate(a,b1);c2.MultiplyAccumulate(a,b2);c3.MultiplyAccumulate(a,b3);
        }
        for(uint e=0;e<8;++e){if(head==0){s0[e]=float(c0.Get(e));s1[e]=float(c1.Get(e));s2[e]=float(c2.Get(e));s3[e]=float(c3.Get(e));}
            else{s0[e]+=float(c0.Get(e));s1[e]+=float(c1.Get(e));s2[e]+=float(c2.Get(e));s3[e]+=float(c3.Get(e));}}
    }
    C g0=C::Splat(0.0f),g1=C::Splat(0.0f),g2=C::Splat(0.0f),g3=C::Splat(0.0f);
    for(uint e=0;e<8;++e){g0.Set(e,float16_t(gate(s0[e])));g1.Set(e,float16_t(gate(s1[e])));g2.Set(e,float16_t(gate(s2[e])));g3.Set(e,float16_t(gate(s3[e])));}
    uint base=(branch*(Pixels/8)+rt)*512,next=((branch+1)*(Pixels/8)+rt)*512;
    g0.Store(Compact,base,32,MatrixLayout::RowMajor);g1.Store(Compact,base+256,32,MatrixLayout::RowMajor);
    g2.Store(Compact,next,32,MatrixLayout::RowMajor);g3.Store(Compact,next+256,32,MatrixLayout::RowMajor);
}
