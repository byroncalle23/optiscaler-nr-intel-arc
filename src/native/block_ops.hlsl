// Tile lanes use independent activation slices; immutable parameters have stride zero.
#if NR_TILE_BATCH
cbuffer NrBatchLayout : register(b15) {uint NrStride0;uint NrStride1;uint NrStride2;uint NrStride3;uint NrStride4;uint NrStride5;}
static uint NrLane=0;
#define NR_INDEX(i,s) ((i)+NrLane*NrStride##s)
#define NR_SET_LANE(z) NrLane=(z)
#else
#define NR_INDEX(i,s) (i)
#define NR_SET_LANE(z)
#endif
// Vendor rounding and global attention for the first native bottleneck block.
// Float buffers carry exactly published E4M3 values between matrix operations.
StructuredBuffer<float> Source : register(t0);
StructuredBuffer<float> Skip : register(t1);
StructuredBuffer<float> Parameter : register(t2);
RWStructuredBuffer<float> Output : register(u0);
cbuffer Shape : register(b0) { uint Tokens; uint Channels; uint Count; uint Publish; }

uint halfBits(float x) {
    // Explicit IEEE round-to-nearest-even, including subnormals and exponent carry.
    uint bits=asuint(x), sign=(bits>>16)&32768, mantissa=bits&0x7fffff;
    int exponent=int((bits>>23)&255)-112;
    if(exponent>=31) return sign|0x7c00;
    if(exponent<=0) {
        if(exponent < -10) return sign;
        uint full=mantissa|0x800000, shift=uint(14-exponent);
        uint value=full>>shift, remainder=full&((1u<<shift)-1u), midpoint=1u<<(shift-1u);
        value += (remainder>midpoint || (remainder==midpoint && (value&1))) ? 1u : 0u;
        return sign|value;
    }
    uint value=(uint(exponent)<<10)|(mantissa>>13), remainder=mantissa&8191;
    value += (remainder>4096 || (remainder==4096 && (value&1))) ? 1u : 0u;
    return sign|value;
}
float halfRound(float x) {
#if NR_NATIVE_HALF
    if((asuint(x)&0x7fffffff)>=0x7f800000)return x;
    return f16tof32(f32tof16(x));
#else
    uint bits=asuint(x), sign=bits&0x80000000, magnitude=bits&0x7fffffff;
    if(magnitude>=0x7f800000) return x;
    if(magnitude>=0x477ff000) return asfloat(sign|0x7f800000);
    if(magnitude>=0x38800000) {
        uint rounded=(magnitude+4095+((magnitude>>13)&1))&0xffffe000;
        return asfloat(sign|rounded);
    }
    float scaled=asfloat(magnitude)*16777216.0, lo=floor(scaled), fraction=scaled-lo;
    float nearest=lo+((fraction>.5 || (fraction==.5 && (uint(lo)&1))) ? 1.0 : 0.0);
    return asfloat(sign|asuint(nearest*5.960464477539063e-8));
#endif
}
float publishE4(float x) {
    float magnitude = min(abs(x), 448.0);
    uint exponent = max((asuint(magnitude) >> 23) & 255, 121) - 3;
    float step = asfloat(exponent << 23);
    float scaled = magnitude * asfloat((254 - exponent) << 23);
    float lo = floor(scaled), fraction = scaled - lo;
    float nearest = lo + ((fraction > .5 || (fraction == .5 && (uint(lo) & 1))) ? 1 : 0);
    return x < 0 ? -nearest * step : nearest * step;
}
[numthreads(128,1,1)]
void PublishState(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    if(id.x<Count)Output[NR_INDEX(id.x,3)]=publishE4(Source[NR_INDEX(id.x,0)]);
}
[numthreads(128,1,1)]
void Gate(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    if (id.x >= Count) return;
    precise float x = halfRound(Source[NR_INDEX(id.x,0)]);
    precise float z = clamp(x, -4.0, 4.0);
    precise float a = abs(z) * -0.055908203125;
    a = halfRound(a + 0.447265625);
    precise float b = a * z;
    b = halfRound(b + 0.89453125);
    Output[NR_INDEX(id.x,3)] = publishE4(halfRound(x * b));
}
[numthreads(128,1,1)]
void Residual(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    if (id.x >= Count) return;
    float value = mad(Skip[NR_INDEX(id.x,1)], Parameter[NR_INDEX(id.x % Channels,2)], Source[NR_INDEX(id.x,0)]);
    Output[NR_INDEX(id.x,3)] = Publish ? publishE4(value) : value;
}
float reciprocalNorm(float v[32]) {
    float sums[16];
    [unroll] for (uint i=0; i<16; ++i) {
        precise float low = v[i] * v[i];
        float high = halfRound(v[i+16] * v[i+16]);
        sums[i] = halfRound(low + high);
    }
    [unroll] for (uint j=0; j<8; ++j) sums[j] = halfRound(sums[j] + sums[j+8]);
    [unroll] for (uint k=0; k<4; ++k) sums[k] = halfRound(sums[k] + sums[k+4]);
    float total = halfRound(halfRound(sums[0]+sums[2]) + halfRound(sums[1]+sums[3]));
    return halfRound(1.0 / sqrt(max(total, 0.00006198883056640625)));
}
[numthreads(32,1,1)]
void NormalizeQkv(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    uint heads = Channels / 32, head = id.x % heads, token = id.x / heads;
    if (token >= Tokens) return;
    float q[32], k[32];
    uint base = token * Channels * 3 + head * 32;
    [unroll] for (uint i=0; i<32; ++i) { q[i]=halfRound(Source[NR_INDEX(base+i,0)]); k[i]=halfRound(Source[NR_INDEX(base+Channels+i,0)]); }
    float nq=reciprocalNorm(q), nk=reciprocalNorm(k), scale=halfRound(Parameter[NR_INDEX(head,2)]);
    [unroll] for (uint c=0; c<32; ++c) {
        Output[NR_INDEX(base+c,3)] = publishE4(halfRound(halfRound(halfRound(q[c]*nq)*5.65625)*scale));
        Output[NR_INDEX(base+Channels+c,3)] = publishE4(halfRound(k[c]*nk));
        Output[NR_INDEX(base+Channels*2+c,3)] = publishE4(halfRound(Source[NR_INDEX(base+Channels*2+c,0)]));
    }
}
float weight(float score) {
    precise float a = halfRound(score) * 0.08953857421875;
    a = halfRound(a + 1.708984375);
    a = clamp(a, 1.439453125, 1.9775390625);
    uint bits = ((halfBits(a) << 4) + 0x4000) & 65535;
    return f16tof32(bits);
}
[numthreads(32,1,1)]
void Attention(uint3 id : SV_DispatchThreadID) { NR_SET_LANE(id.z);
    // Stream 64-key blocks, including vendor denominator correction for padding.
    uint heads=Channels/32, head=id.x%heads, row=id.x/heads;
    if (row>=Tokens) return;
    float weights[64], sums[32];
    [unroll] for (uint c=0; c<32; ++c) sums[c]=0;
    uint query=row*Channels*3+head*32;
    float total=0;
    uint padded=(Tokens+63)/64*64;
    [loop] for(uint first=0;first<padded;first+=64) {
    [loop] for (uint slot=0; slot<64; ++slot) {
        uint key=first+slot;
        precise float score=0;
        if(key<Tokens) {
        [unroll] for (uint d=0; d<32; ++d)
            score += Source[NR_INDEX(query+d,0)] * Source[NR_INDEX(key*Channels*3+Channels+head*32+d,0)];
        }
        weights[slot]=weight(score);
        float w=publishE4(weights[slot]);
        if(key<Tokens) {
        [unroll] for (uint j=0; j<32; ++j)
            sums[j] += w * Source[NR_INDEX(key*Channels*3+2*Channels+head*32+j,0)];
        }
    }
    float columns[8];
    [unroll] for (uint col=0; col<8; ++col) {
        float a=halfRound(weights[col]+weights[col+8]);
        float b=halfRound(weights[col+16]+weights[col+24]);
        float c=halfRound(weights[col+32]+weights[col+40]);
        float d=halfRound(weights[col+48]+weights[col+56]);
        columns[col]=halfRound(halfRound(halfRound(a+b)+c)+d);
    }
    float even=halfRound(halfRound(halfRound(columns[0]+columns[2])+columns[4])+columns[6]);
    float odd=halfRound(halfRound(halfRound(columns[1]+columns[3])+columns[5])+columns[7]);
    total=halfRound(total+halfRound(even+odd));
    }
    if(padded>Tokens)total=halfRound(total-halfRound(weight(0)*float(padded-Tokens)));
    float reciprocal=halfRound(1.0/total);
    [unroll] for (uint outChannel=0; outChannel<32; ++outChannel)
        Output[NR_INDEX(row*Channels+head*32+outChannel,3)]=publishE4(halfRound(halfRound(sums[outChannel])*reciprocal));
}

#include "attention_ops.hlsl"
