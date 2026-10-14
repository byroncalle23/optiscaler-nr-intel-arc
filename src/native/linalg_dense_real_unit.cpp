#define NOMINMAX
#include <windows.h>
#include <array>
extern "C" {__declspec(dllexport) extern const UINT D3D12SDKVersion=721;__declspec(dllexport) extern const char* D3D12SDKPath=".\\native_nr_runtime\\";}
#define main nr_unused_graph_main
#include "dml_batch_graph.cpp"
#undef main
class Probe:public BatchGraphProbe{public:void run(){recordGemm(*jobs.back());submit();}};
uint16_t halfDyadic(float x){uint32_t u;memcpy(&u,&x,4);if((u&0x7fffffff)==0)return uint16_t(u>>16);return uint16_t(((u>>16)&32768)|((((u>>23)&255)-112)<<10)|((u&0x7fffff)>>13));}
std::vector<float> readFloats(const std::filesystem::path& p,UINT count){std::ifstream f(p,std::ios::binary);std::vector<float> v(count);if(!f.read(reinterpret_cast<char*>(v.data()),UINT64(count)*4))throw std::runtime_error("Real tensor read");return v;}
int main(int argc,char** argv){try{
 if(argc!=4)throw std::runtime_error("Usage: real_unit capture shaders output");auto dir=nativePath(argv[1]);auto output=nativePath(argv[3]);std::filesystem::create_directories(output);
 nrLinAlgShaderDirectory=LR"(<GAME_LIBRARY>\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem\native_nr_global360\shaders)";
 UINT m,n,k;std::ifstream shape(dir/"shape.txt");if(!(shape>>m>>n>>k))throw std::runtime_error("Shape read");
 auto input=readFloats(dir/"input.bin",m*k),weights=readFloats(dir/"weights.bin",k*n),original=readFloats(dir/"original.bin",m*n);
 for(UINT policy=0;policy<4;++policy){
  SetEnvironmentVariableA("NR_STUDY_FP16_DENSE_DIR",policy?argv[2]:nullptr);
  SetEnvironmentVariableA("NR_STUDY_FP16_DENSE_NATIVE_LOAD",policy==2?"1":nullptr);
  SetEnvironmentVariableA("NR_STUDY_FP16_DENSE_WAVE",policy==3?"1":nullptr);
  Probe probe;auto ctx=probe.sharedContext();ctx->linAlg=true;ctx->denseTileN=32;
  UINT logical=m*k;auto a=probe.make(policy?logical+logical/2:logical),b=probe.make(k*n,false),out=probe.make(m*n);b->immutableWeight=true;
  if(policy){std::vector<float> storage(logical+logical/2);std::copy(input.begin(),input.end(),storage.begin());for(UINT i=0;i<logical;i+=2){uint32_t packed=halfDyadic(input[i])|(uint32_t(halfDyadic(input[i+1]))<<16);memcpy(&storage[logical+i/2],&packed,4);}probe.uploadLanes(a,storage);a->elements=logical;a->denseHalfShadow=true;}else probe.uploadLanes(a,input);
  probe.uploadLanes(b,weights);probe.addGemm(a,b,out,m,n,k);probe.submit();std::vector<float> first;
  for(UINT rep=0;rep<3;++rep){probe.run();auto got=probe.downloadLanes(out);if(!rep)first=got;else if(memcmp(got.data(),first.data(),got.size()*4))throw std::runtime_error("Real GEMM repeats differ");}
  double err=0,den=0;UINT changed=0;float maxdiff=0;for(UINT i=0;i<m*n;++i){if(!std::isfinite(first[i]))throw std::runtime_error("Real GEMM nonfinite");double d=double(first[i])-original[i];err+=d*d;den+=double(original[i])*original[i];changed+=first[i]!=original[i];maxdiff=std::max(maxdiff,float(abs(d)));}
  std::ofstream f(output/("policy"+std::to_string(policy)+".bin"),std::ios::binary);f.write(reinterpret_cast<char*>(first.data()),first.size()*4);
  printf("REAL_GEMM_COMPARE policy=%u m=%u n=%u k=%u rel_rmse=%.12g changed=%u maxdiff=%.12g repeat_exact=1\n",policy,m,n,k,sqrt(err/std::max(den,1e-30)),changed,maxdiff);
 }
 return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}}
