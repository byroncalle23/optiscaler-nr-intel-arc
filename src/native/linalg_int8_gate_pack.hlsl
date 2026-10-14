#include "linalg_int8_shapes.hlsl"
// Preserve the current Gate's explicit half and E4M3 publications.
float halfRoundGate(float x){
    uint bits=asuint(x),sign=bits&0x80000000,magnitude=bits&0x7fffffff;
    if(magnitude>=0x7f800000)return x;
    if(magnitude>=0x477ff000)return asfloat(sign|0x7f800000);
    if(magnitude>=0x38800000){uint rounded=(magnitude+4095+((magnitude>>13)&1))&0xffffe000;return asfloat(sign|rounded);}
    float scaled=asfloat(magnitude)*16777216.0,lo=floor(scaled),fraction=scaled-lo;
    float nearest=lo+((fraction>.5||(fraction==.5&&(uint(lo)&1)))?1.0:0.0);
    return asfloat(sign|asuint(nearest*5.960464477539063e-8));
}
float publishE4Gate(float x){
    float magnitude=min(abs(x),448.0);
    uint exponent=max((asuint(magnitude)>>23)&255,121)-3;
    float step=asfloat(exponent<<23),scaled=magnitude*asfloat((254-exponent)<<23);
    float lo=floor(scaled),fraction=scaled-lo;
    float nearest=lo+((fraction>.5||(fraction==.5&&(uint(lo)&1)))?1:0);
    return x<0?-nearest*step:nearest*step;
}
float gateValue(float value){
    precise float x=halfRoundGate(value),z=clamp(x,-4.0,4.0);
    precise float a=abs(z)*-0.055908203125;
    a=halfRoundGate(a+0.447265625);
    precise float b=a*z;b=halfRoundGate(b+0.89453125);
    return publishE4Gate(halfRoundGate(x*b));
}
[numthreads(64,1,1)]
void GatePack(uint3 id:SV_DispatchThreadID){
    uint words=Rows*Inner/4,word=id.x+id.y*min((words+63)/64,4096u)*64;
    if(word>=words)return;
    uint tile=word/64,local=word%64,kt=tile%(Inner/32),rt=tile/(Inner/32);
    uint row=rt*8+local/8,channel=kt*32+(local%8)*4,packed=0;
    [unroll]for(uint j=0;j<4;++j){
        uint col=channel+j,sourceTile=rt*(Inner/16)+col/16;
        float value=float(asint(InputA.Load((sourceTile*128+(row%8)*16+col%16)*4)))*OutputScale;
        value*=asfloat(InputB.Load(Fused*Inner+col*4));
        packed|=(uint(quantize(gateValue(value)))&255u)<<(j*8);
    }
    Scratch.Store(word*4,packed);
}
