#define NOMINMAX
#include <windows.h>
#include <array>
extern "C" {__declspec(dllexport) extern const UINT D3D12SDKVersion=721;__declspec(dllexport) extern const char* D3D12SDKPath=".\\native_nr_runtime\\";}
#define main nr_unused_graph_main
#include "dml_batch_graph.cpp"
#undef main
class Probe:public BatchGraphProbe{public:void run(){recordGemm(*jobs.back());submit();}};
uint16_t halfDyadic(float x){uint32_t u;memcpy(&u,&x,4);if((u&0x7fffffff)==0)return uint16_t(u>>16);return uint16_t(((u>>16)&32768)|((((u>>23)&255)-112)<<10)|((u&0x7fffff)>>13));}
int main(int argc,char** argv){try{
 if(argc!=2)throw std::runtime_error("Usage: unit shader_directory");SetEnvironmentVariableA("NR_STUDY_FP16_DENSE_DIR",argv[1]);
 nrLinAlgShaderDirectory=LR"(<GAME_LIBRARY>\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem\native_nr_global360\shaders)";
 for(auto shape:std::vector<std::array<UINT,3>>{{9,128,128},{17,384,128},{33,768,256},{32,1536,512}}){UINT m=shape[0],n=shape[1],k=shape[2];std::vector<float> reference;
  for(UINT policy=0;policy<2;++policy){SetEnvironmentVariableA("NR_STUDY_FP16_DENSE_WAVE",policy?"1":nullptr);Probe probe;auto ctx=probe.sharedContext();ctx->linAlg=true;ctx->denseTileN=32;
   UINT logical=m*k;auto a=probe.make(logical+logical/2),b=probe.make(k*n,false),out=probe.make(m*n);b->immutableWeight=true;
   std::vector<float> input(logical),weights(k*n);for(UINT i=0;i<logical;++i)input[i]=float(int((i*17+i/k*13)%9)-4)/16;for(UINT i=0;i<k*n;++i)weights[i]=float(int((i*11+i/n*5)%9)-4)/16;
   auto upload=[&](const std::vector<float>& x){std::vector<float> storage(logical+logical/2);std::copy(x.begin(),x.end(),storage.begin());for(UINT i=0;i<logical;i+=2){uint32_t packed=halfDyadic(x[i])|(uint32_t(halfDyadic(x[i+1]))<<16);memcpy(&storage[logical+i/2],&packed,4);}a->elements=storage.size();probe.uploadLanes(a,storage);a->elements=logical;a->denseHalfShadow=true;};
   upload(input);probe.uploadLanes(b,weights);probe.addGemm(a,b,out,m,n,k);probe.submit();std::vector<float> first;
   auto verify=[&](const std::vector<float>& got,const std::vector<float>& x){for(UINT r=0;r<m;++r)for(UINT col=0;col<n;++col){double v=0;for(UINT q=0;q<k;++q)v+=double(x[r*k+q])*weights[q*n+col];if(got[r*n+col]!=float(v)||!std::isfinite(got[r*n+col]))throw std::runtime_error("Dense FP16 oracle mismatch");}};
   for(UINT rep=0;rep<3;++rep){probe.run();auto got=probe.downloadLanes(out);verify(got,input);if(rep==0)first=got;else if(memcmp(first.data(),got.data(),got.size()*4))throw std::runtime_error("Dense FP16 repeat mismatch");}
   if(policy==0)reference=first;else if(memcmp(first.data(),reference.data(),first.size()*4))throw std::runtime_error("Dense FP16 policy mismatch");
   auto changed=input;for(UINT q=0;q<k;++q)changed[(m-1)*k+q]*=-1;upload(changed);probe.run();auto alt=probe.downloadLanes(out);verify(alt,changed);bool affected=false;for(UINT i=0;i<m*n;++i){if(i<(m-1)*n&&alt[i]!=first[i])throw std::runtime_error("Dense FP16 row contamination");affected|=alt[i]!=first[i];}if(!affected)throw std::runtime_error("Dense FP16 insensitivity");
   printf("DENSE_FP16_UNIT_PASS m=%u n=%u k=%u policy=%u oracle_exact=1 repeat=1 isolation=1\n",m,n,k,policy);
  }
 }
 puts("DENSE_FP16_UNITS_PASS cases=8");return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
