// Pre-launch smoke test for exactly the session/cache/texture adapter used by
// the DLL. SDK616 host, early SDK721 selection, same-device DIRECT submission.
#define NOMINMAX
#include <windows.h>
extern "C" {__declspec(dllexport) extern const UINT D3D12SDKVersion=616;__declspec(dllexport) extern const char* D3D12SDKPath=".\\D3D12\\";}
#include "nr_command_cache.h"
#include "nr_submission.h"
#include "nr_pair_capture.h"
#include "native_assets.h"
#ifdef NR_GAME_FACTORY_PROBE
#include "../../integration/optiscaler/NativeNrRuntime.cpp"
#endif
#include <dxgidebug.h>
#include <chrono>
#include <future>
void saveBytes(const std::string& path,const void* data,size_t size){std::ofstream f(nativePath(path),std::ios::binary);f.write(static_cast<const char*>(data),size);if(!f)throw std::runtime_error("Probe output write");}
ComPtr<ID3D12Resource> probeEngineTexture(std::shared_ptr<NativeContext> ctx,DXGI_FORMAT format,UINT components,float value,bool horizontalOnly=false){
 D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=1280;d.Height=720;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
 D3D12_HEAP_PROPERTIES p{};p.Type=D3D12_HEAP_TYPE_DEFAULT;p.CreationNodeMask=p.VisibleNodeMask=1;ComPtr<ID3D12Resource> target;
 check(ctx->device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&target)),"Engine fixture texture");
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;ctx->device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);
 auto up=buffer(ctx->device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped;D3D12_RANGE empty{0,0};check(up->Map(0,&empty,&mapped),"Engine fixture upload");
 for(UINT y=0;y<720;++y){auto row=reinterpret_cast<float*>(static_cast<char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch);for(UINT x=0;x<1280*components;++x)row[x]=horizontalOnly&&x%components?0:value;}up->Unmap(0,&empty);
 D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=target.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=up.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;
 ctx->cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);transition(ctx->cmd.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 check(ctx->cmd->Close(),"Engine fixture close");ID3D12CommandList* lists[]={ctx->cmd.Get()};ctx->queue->ExecuteCommandLists(1,lists);check(ctx->queue->Signal(ctx->fence.Get(),++ctx->serial),"Engine fixture signal");check(ctx->fence->SetEventOnCompletion(ctx->serial,ctx->event),"Engine fixture fence");if(WaitForSingleObject(ctx->event,60000)!=WAIT_OBJECT_0)throw std::runtime_error("Engine fixture timeout");check(ctx->allocator->Reset(),"Engine fixture allocator");check(ctx->cmd->Reset(ctx->allocator.Get(),nullptr),"Engine fixture reset");return target;
}
int main(int argc,char** argv){try{
 bool compactTimeline=false;
 bool encoderDiagnostic=false;
#ifdef NR_COMPACT_GEOMETRY
 wchar_t encoderDirectory[32768]{};
 encoderDiagnostic=GetEnvironmentVariableW(L"NR_ENCODER_DIAG_DIR",encoderDirectory,32768)>0;
 wchar_t encoderOracleDirectory[32768]{};
 bool encoderOracle=GetEnvironmentVariableW(L"NR_ENCODER_ORACLE_INPUT",encoderOracleDirectory,32768)>0;
 if(encoderOracle&&!encoderDiagnostic)throw std::runtime_error("Encoder oracle needs diagnostic output directory");
 wchar_t forwardCaptureDirectory[32768]{};
 bool forwardCapture=GetEnvironmentVariableW(L"NR_NATIVE_FORWARD_CAPTURE",forwardCaptureDirectory,32768)>0;
 if(forwardCapture&&!encoderDiagnostic)throw std::runtime_error("Forward capture requires explicit single-reset diagnostic");
 if(forwardCapture&&encoderOracle)throw std::runtime_error("Forward capture cannot inject encoder oracle tensors");
 setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
 compactTimeline=GetEnvironmentVariableA("NR_COMPACT_TIMELINE",nullptr,0)!=0;
#endif
#ifdef NR_CLEAN_PROFILE
 const bool cleanProfile=GetEnvironmentVariableA("NR_CLEAN_PROFILE",nullptr,0)!=0;
 const bool detailedProfile=GetEnvironmentVariableA("NR_CLEAN_DETAILED",nullptr,0)!=0;
 wchar_t capturePath[32768]{};bool pixCapture=GetEnvironmentVariableW(L"NR_PIX_CAPTURE",capturePath,32768)>0;
 HMODULE pixModule=nullptr;
 if(pixCapture){pixModule=LoadLibraryW(L"C:\\Program Files\\Microsoft PIX Preview\\2606.18-preview\\WinPixGpuCapturer.dll");if(!pixModule)throw std::runtime_error("PIX capturer load failed");}
#else
 const bool cleanProfile=false,detailedProfile=false;
#endif
#ifdef NR_GAME_FACTORY_PROBE
 if(argc==2&&std::string(argv[1])=="--fallback-check"){
  DWORD mode=0,size=sizeof(mode);auto query=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",L"AllowDevelopmentWithoutDevLicense",RRF_RT_REG_DWORD,nullptr,&mode,&size);
  if(query==ERROR_SUCCESS&&mode==1)throw std::runtime_error("Fallback check requires experimental mode disabled");
  ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Fallback DXGI");ComPtr<IDXGIAdapter1> adapter;
  for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};candidate->GetDesc1(&d);if(d.VendorId==0x8086&&d.DeviceId==0xe20b){adapter=candidate;break;}}
  if(!adapter)throw std::runtime_error("B580 missing");
  ComPtr<ID3D12Device> before,optional,after;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&before)),"Original before");
  HRESULT result=S_OK;bool handled=NativeNrCreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),reinterpret_cast<void**>(optional.GetAddressOf()),&result);
  if(handled||optional||SUCCEEDED(result))throw std::runtime_error("Experimental failure did not fall back");
  check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&after)),"Original after");
  if(before.Get()!=after.Get())throw std::runtime_error("Legacy singleton changed");check(after->GetDeviceRemovedReason(),"Legacy health");
  printf("OPTIONAL_NR_FAILURE_FALLBACK_PASS nrHRESULT=0x%08X gameDevice=S_OK identity=UNCHANGED\n",unsigned(result));return 0;
 }
#endif
 if(argc==3&&std::string(argv[1])=="--cpu-check"){
  NativeNrAssets package(nativePath(argv[2]));
  for(auto unorm:{"0","1"})for(auto unorm10:{"0","1"})for(const char* entry:{"TextureToRGB","RGBToTexture"}){
   D3D_SHADER_MACRO macros[]={{"GLOBAL_RESIDUAL","1"},{"OUTPUT_UNORM",unorm},{"OUTPUT_UNORM10",unorm10},{nullptr,nullptr}};
   compileNativeShader("texture_ops.hlsl",entry,macros);
  }
  puts("CPU_PACKAGE_AND_GLOBAL_SHADERS_PASS");return 0;
 }
 const bool dampedSequence=argc==8&&std::string(argv[6])=="--engine-damped-sequence";
 const bool baseSequence=argc==8&&std::string(argv[6])=="--engine-base-sequence";
 const bool mixedSequence=argc==8&&std::string(argv[6])=="--engine-mixed-sequence";
 const bool adaptiveTest=argc==8&&(std::string(argv[6])=="--engine-adaptive-stability"||std::string(argv[6])=="--engine-adaptive-sequence");
 const bool switchCheck=argc==8&&std::string(argv[6])=="--engine-switch-check";
 const bool edgeTest=switchCheck||(argc==8&&(std::string(argv[6])=="--engine-edge-stability"||std::string(argv[6])=="--engine-edge-sequence"));
 const bool motionTest=edgeTest||(argc==8&&(std::string(argv[6])=="--engine-motion-stability"||std::string(argv[6])=="--engine-motion-sequence"));
 const bool translatedTest=argc==8&&(std::string(argv[6])=="--engine-motion-sequence"||std::string(argv[6])=="--engine-motion-reference"||std::string(argv[6])=="--engine-edge-sequence");
 const UINT historyMode=mixedSequence?3:baseSequence?2:UINT(dampedSequence);
#ifndef NR_HISTORY_EXPERIMENTS
 if(historyMode)throw std::runtime_error("History experiments require explicit NR_HISTORY_EXPERIMENTS test build; not present in game DLL");
#endif
 const bool sequenceTest=translatedTest||mixedSequence||baseSequence||dampedSequence||(argc==8&&(std::string(argv[6])=="--engine-sequence"||std::string(argv[6])=="--engine-adaptive-sequence"));
 const bool engineTest=motionTest||adaptiveTest||sequenceTest||(argc==8&&(std::string(argv[6])=="--engine-temporal"||std::string(argv[6])=="--engine-stability"||std::string(argv[6])=="--engine-detail-stability"));
 const bool temporalTest=engineTest||(argc==8&&std::string(argv[6])=="--temporal");
 if(argc!=6&&!temporalTest)throw std::runtime_error("Usage: session_probe package input output width height [--temporal fixture]");
#ifdef NR_GAME_FACTORY_PROBE
 ComPtr<ID3D12Device> legacy;check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&legacy)),"Existing legacy device");
#else
 ComPtr<ID3D12SDKConfiguration> sdk;check(D3D12GetInterface(CLSID_D3D12SDKConfiguration,IID_PPV_ARGS(&sdk)),"SDK configuration");
 check(sdk->SetSDKVersion(721,".\\native_nr_runtime\\"),"SDK721 selection");
#endif
 // Enable before either the caller device or the session's private setup queue.
 NativeRuntime caller;auto ctx=caller.sharedContext();UINT width=std::stoul(argv[4]),height=std::stoul(argv[5]);
#ifdef NR_GAME_FACTORY_PROBE
 ComPtr<ID3D12Device> second;HRESULT reused;
 if(!NativeNrCreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),reinterpret_cast<void**>(second.GetAddressOf()),&reused))throw std::runtime_error("Default adapter routing failed");
 check(reused,"Second factory request");if(second.Get()!=ctx->device.Get())throw std::runtime_error("Primary factory device identity changed");
 check(legacy->GetDeviceRemovedReason(),"Legacy device preserved");puts("PRIVATE_FACTORY_DEVICE_IDENTITY_AND_LEGACY_HEALTH_PASS");
#endif
 DXGI_FORMAT format=std::string(argv[2]).find("rgba8")!=std::string::npos?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R10G10B10A2_UNORM;
 D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
 desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;props.CreationNodeMask=props.VisibleNodeMask=1;
 ComPtr<ID3D12Resource> target;check(ctx->device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&target)),"Probe target");
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes;ctx->device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
 auto upload=buffer(ctx->device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 auto readback=buffer(ctx->device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 std::ifstream input(nativePath(argv[2]),std::ios::binary|std::ios::ate);if(!input||input.tellg()!=std::streamoff(UINT64(width)*height*4))throw std::runtime_error("Input geometry");
 std::vector<char> pixels(size_t(width)*height*4);input.seekg(0);input.read(pixels.data(),pixels.size());if(!input)throw std::runtime_error("Input read");
 void* mapped;D3D12_RANGE empty{0,0};check(upload->Map(0,&empty,&mapped),"Upload map");
 for(UINT y=0;y<height;++y)memcpy(static_cast<char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch,pixels.data()+size_t(y)*width*4,size_t(width)*4);upload->Unmap(0,&empty);
 D3D12_TEXTURE_COPY_LOCATION texture{};texture.pResource=target.Get();texture.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 D3D12_TEXTURE_COPY_LOCATION up{};up.pResource=upload.Get();up.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;up.PlacedFootprint=footprint;
 D3D12_TEXTURE_COPY_LOCATION rb=up;rb.pResource=readback.Get();
 ctx->cmd->CopyTextureRegion(&texture,0,0,0,&up,nullptr);transition(ctx->cmd.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);caller.submit();
 NativeNrAssets verified(nativePath(argv[1]));auto session=std::make_shared<NativeNrSession>(target.Get(),verified.folder(),true,true,false,true);
 if(edgeTest)session->setEdgeGuidedResidual(true);
 if(temporalTest){auto bad=target->GetDesc();NativeMotionGeometry invalid{bad,bad};bool rejected=false;
  try{session->prepareTemporalEngine(invalid);}catch(const std::exception&){rejected=true;}
  if(!rejected||session->temporalEnabled()||session->motionPrepared())throw std::runtime_error("Failed bridge partially published temporal graph");
  puts("TEMPORAL_TRANSACTIONAL_PREPARATION_FAILURE_PASS");}
 std::shared_ptr<NativeGpuProfile> temporalProfile;
 ComPtr<ID3D12Resource> engineMotion,engineDepth;NativeNrTemporal::FrameInput engineFrame{};
 if(engineTest){if(std::string(argv[6])=="--engine-stability"||std::string(argv[6])=="--engine-detail-stability"||sequenceTest||adaptiveTest||motionTest)session->setResidualStability(.25f,std::string(argv[6])=="--engine-detail-stability"||sequenceTest||adaptiveTest||motionTest,historyMode,adaptiveTest||motionTest||translatedTest,motionTest);engineMotion=probeEngineTexture(ctx,DXGI_FORMAT_R32G32_FLOAT,2,translatedTest?float(width/session->lowWidth())/width:0,translatedTest);engineDepth=probeEngineTexture(ctx,DXGI_FORMAT_R32_FLOAT,1,.3f);
  session->enableStabilityDiagnostic(!cleanProfile);session->prepareTemporalEngine({engineMotion->GetDesc(),engineDepth->GetDesc()});
  engineFrame.motion={engineMotion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,true};engineFrame.depth={engineDepth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,true};engineFrame.handle=17;engineFrame.renderWidth=1280;engineFrame.renderHeight=720;engineFrame.displayWidth=width;engineFrame.displayHeight=height;engineFrame.motionScaleX=1280;engineFrame.motionScaleY=-720;engineFrame.scalesKnown=engineFrame.jitterKnown=engineFrame.lowResolutionMotion=engineFrame.motionConventionVerified=true;engineFrame.motionReferenceWidth=1280;engineFrame.motionReferenceHeight=720;
 }
 if(temporalTest){if(!engineTest)session->prepareTemporal()->seedTestFixture(argv[7]);temporalProfile=session->temporalProfileForTest();}
 std::shared_ptr<NativeGpuProfile> detailed;
 std::shared_ptr<NativeGpuProfile> bridgeProfile;
 if(cleanProfile)bridgeProfile=std::make_shared<NativeGpuProfile>(ctx->device.Get(),16);
#ifdef NR_CLEAN_PROFILE
 if(detailedProfile){detailed=std::make_shared<NativeGpuProfile>(ctx->device.Get());session->setStudyProfiler(detailed);}
#endif
 std::unique_ptr<NativeNrCommandCache> cache;NativeNrSubmissions submissions(ctx->device.Get());
 if(switchCheck){
  engineFrame.featureFrame=1;engineFrame.reset=true;
  if(!session->recordEngineMotion(ctx->cmd.Get(),engineFrame,[&]{submissions.track(ctx->cmd.Get(),session);}))throw std::runtime_error("Switch fixture capture failed");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Switch event");
  auto pending=std::async(std::launch::async,[&]{return session->waitQuiescent(event,10000);});
  bool heldBeforeSubmit=pending.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
  ComPtr<ID3D12Fence> gate;check(ctx->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"Switch gate");
  check(ctx->queue->Wait(gate.Get(),1),"Switch GPU hold");check(ctx->cmd->Close(),"Switch close");ID3D12CommandList* lists[]={ctx->cmd.Get()};
  submissions.execute(ctx->queue.Get(),1,lists,[&]{ctx->queue->ExecuteCommandLists(1,lists);});
  bool heldBeforeFence=pending.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
  check(gate->Signal(1),"Switch release");bool ready=pending.get();CloseHandle(event);submissions.collect();
  if(!heldBeforeSubmit||!heldBeforeFence||!ready)throw std::runtime_error("Mode switch did not wait for submit and GPU completion");
  session->requestRetire();HANDLE retiredEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);bool retired=session->waitQuiescent(retiredEvent,1000);CloseHandle(retiredEvent);
  if(!retired)throw std::runtime_error("Safely discarded/retired session blocked switch");
  if(!ctx->diagnostics)throw std::runtime_error("Switch check requires D3D12 debug layer");
  for(UINT64 i=0;i<ctx->diagnostics->GetNumStoredMessages();++i){SIZE_T size=0;ctx->diagnostics->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto message=reinterpret_cast<D3D12_MESSAGE*>(data.data());ctx->diagnostics->GetMessage(i,message,&size);if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)throw std::runtime_error(message->pDescription);}
  puts("MODE_SWITCH_WAIT_SUBMIT_FENCE_AND_RETIRED_PASS");return 0;
 }
 std::vector<char> first;
 const UINT runCount=encoderDiagnostic?1:cleanProfile?40:translatedTest?8:sequenceTest?16:4;
 bool oracleDiagnostic=false;
#ifdef NR_COMPACT_GEOMETRY
 wchar_t oracleCapture[32768]{},oracleInput[32768]{};
 bool captureOracle=GetEnvironmentVariableW(L"NR_ORACLE_BOTTLENECK_CAPTURE",oracleCapture,32768)>0;
 bool injectOracle=GetEnvironmentVariableW(L"NR_ORACLE_BOTTLENECK_INPUT",oracleInput,32768)>0;
 oracleDiagnostic=captureOracle||injectOracle;
 ComPtr<ID3D12Resource> oracleReadback,oracleUpload;UINT64 oracleBytes=0;UINT oracleFrame=0;
 struct EncoderSnapshot{std::string name;Tensor* tensor;ComPtr<ID3D12Resource> readback,oracle;UINT h,w,c;};
 std::vector<EncoderSnapshot> encoderSnapshots;
 struct PrefixSnapshot{std::string name;Tensor* tensor;UINT64 elements;ComPtr<ID3D12Resource> readback;};
 std::vector<PrefixSnapshot> prefixSnapshots;
 if(encoderDiagnostic){
  if(!cleanProfile||oracleDiagnostic||compactTimeline)throw std::runtime_error("Encoder diagnostic requires isolated single-reset mode");
  std::filesystem::create_directories(encoderDirectory);std::ofstream layout(std::filesystem::path(encoderDirectory)/"layout.txt");
  for(auto& node:session->captureGraph().publicNodes()){
   Tensor* t=node->window?node->window->skipTensor():node->bottleneck?node->output():forwardCapture&&node->name=="adapter"?node->output():nullptr;if(!t)continue;
   UINT h=0,w=0,c=0;
   if(node->window||node->name=="adapter"){std::ifstream f(nativePath(std::string(argv[1])+"/block0/shape.txt"));if(node->window)f=std::ifstream(nativePath(std::string(argv[1])+"/"+node->name+"/shape.txt"));UINT values[9]{};for(auto& v:values)if(!(f>>v))throw std::runtime_error("Encoder window extent read");h=values[0];w=values[1];c=values[8];}
   else{std::ifstream f(nativePath(std::string(argv[1])+"/down30/shape.txt"));UINT values[9]{};for(auto& v:values)if(!(f>>v))throw std::runtime_error("Encoder bottleneck extent read");h=values[5];w=values[6];c=1024;}
   if(UINT64(h)*w*c!=t->elements)throw std::runtime_error("Encoder snapshot extent mismatch");
   ComPtr<ID3D12Resource> oracle;
   if(encoderOracle){
    UINT64 byteCount=t->elements*4;std::vector<char> data(static_cast<size_t>(byteCount));
    std::ifstream f(std::filesystem::path(encoderOracleDirectory)/(node->name+".bin"),std::ios::binary|std::ios::ate);
    if(!f||f.tellg()!=std::streamoff(byteCount))throw std::runtime_error("Encoder oracle extent mismatch");f.seekg(0);f.read(data.data(),byteCount);if(!f)throw std::runtime_error("Encoder oracle read");
    for(size_t i=0;i<data.size();i+=4){float v;memcpy(&v,data.data()+i,4);if(!std::isfinite(v))throw std::runtime_error("Nonfinite encoder oracle");}
    oracle=buffer(ctx->device.Get(),byteCount,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* dst;check(oracle->Map(0,&empty,&dst),"Encoder oracle setup map");memcpy(dst,data.data(),data.size());oracle->Unmap(0,&empty);
   }
   encoderSnapshots.push_back({node->name,t,buffer(ctx->device.Get(),t->elements*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST),oracle,h,w,c});
   layout<<node->name<<" "<<h<<" "<<w<<" "<<c<<"\n";
  }
  if(!layout||encoderSnapshots.size()!=(forwardCapture?8u:7u))throw std::runtime_error("Encoder snapshot set mismatch");
  if(forwardCapture){for(auto& node:session->captureGraph().publicNodes())if(node->name=="block0"){
   std::ofstream sizes(std::filesystem::path(encoderDirectory)/"block0-prefix.tsv");
   for(auto& view:node->window->firstBlockStudyViews()){
    auto count=std::min<UINT64>(view.second->elements,163840);
    prefixSnapshots.push_back({view.first,view.second,count,buffer(ctx->device.Get(),count*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST)});
    sizes<<view.first<<"\t"<<count<<"\n";
   }if(!sizes)throw std::runtime_error("First-block prefix manifest");
  }}
  session->captureGraph().setStudyObserver([&](GraphNode& node){for(auto& s:encoderSnapshots)if(s.name==node.name){auto before=s.tensor->state;
   if(s.oracle){s.tensor->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_COPY_DEST);ctx->cmd->CopyBufferRegion(s.tensor->resource.Get(),0,s.oracle.Get(),0,s.tensor->elements*4);}
   s.tensor->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);ctx->cmd->CopyBufferRegion(s.readback.Get(),0,s.tensor->resource.Get(),0,s.tensor->elements*4);s.tensor->to(ctx->cmd.Get(),before);}
   if(node.name=="block0")for(auto& s:prefixSnapshots){auto before=s.tensor->state;s.tensor->to(ctx->cmd.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);ctx->cmd->CopyBufferRegion(s.readback.Get(),0,s.tensor->resource.Get(),0,s.elements*4);s.tensor->to(ctx->cmd.Get(),before);}
  });
  puts("ENCODER_DIAGNOSIS_SINGLE_RESET timing_eligible=0 deployable=0");
 }
 if(oracleDiagnostic){
  if(!cleanProfile||captureOracle==injectOracle)throw std::runtime_error("Oracle diagnostic needs one mode and clean40 lifecycle");
  for(auto& node:session->captureGraph().publicNodes())if(node->bottleneck)oracleBytes=node->output()->elements*4;
  if(!oracleBytes||oracleBytes>640ull*1024*4)throw std::runtime_error("Oracle bottleneck extent");
  if(captureOracle){std::filesystem::create_directories(oracleCapture);oracleReadback=buffer(ctx->device.Get(),oracleBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);}
  else{
   std::vector<char> data(size_t(oracleBytes*runCount));
   for(UINT i=0;i<runCount;++i){std::ifstream f(std::filesystem::path(oracleInput)/("frame"+std::to_string(i)+".bin"),std::ios::binary|std::ios::ate);
    if(!f||f.tellg()!=std::streamoff(oracleBytes))throw std::runtime_error("Oracle input extent");f.seekg(0);f.read(data.data()+i*oracleBytes,oracleBytes);if(!f)throw std::runtime_error("Oracle input read");}
   for(size_t i=0;i<data.size();i+=4){float v;memcpy(&v,data.data()+i,4);if(!std::isfinite(v))throw std::runtime_error("Nonfinite oracle input");}
   oracleUpload=buffer(ctx->device.Get(),data.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
   void* dst;check(oracleUpload->Map(0,&empty,&dst),"Oracle setup map");memcpy(dst,data.data(),data.size());oracleUpload->Unmap(0,&empty);
  }
  session->captureGraph().setStudyObserver([&](GraphNode& node){if(!node.bottleneck)return;
   auto t=node.output();auto before=t->state;
   t->to(ctx->cmd.Get(),captureOracle?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_COPY_DEST);
   if(captureOracle)ctx->cmd->CopyBufferRegion(oracleReadback.Get(),0,t->resource.Get(),0,oracleBytes);
   else ctx->cmd->CopyBufferRegion(t->resource.Get(),0,oracleUpload.Get(),oracleFrame*oracleBytes,oracleBytes);
   t->to(ctx->cmd.Get(),before);
  });
  puts("COMPACT_ORACLE_DIAGNOSTIC timing_eligible=0 deployable=0");
 }
#endif
 for(UINT run=0;run<runCount;++run){
#ifdef NR_COMPACT_GEOMETRY
  oracleFrame=run;
#endif
#ifdef NR_CLEAN_PROFILE
  if(pixCapture&&run==8){auto begin=reinterpret_cast<HRESULT(WINAPI*)(const void*)>(GetProcAddress(pixModule,"BeginProgrammaticGpuCapture"));if(!begin)throw std::runtime_error("PIX begin export missing");struct{PCWSTR filename;} params{capturePath};check(begin(&params),"PIX begin frame capture");}
#endif
  if(translatedTest){
   const UINT shift=(run*(width/session->lowWidth()))%width;check(upload->Map(0,&empty,&mapped),"Moving input upload");
   for(UINT y=0;y<height;++y){auto dst=static_cast<char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch;auto src=pixels.data()+size_t(y)*width*4;
    memcpy(dst,src+shift*4,size_t(width-shift)*4);if(shift)memcpy(dst+size_t(width-shift)*4,src,size_t(shift)*4);}
   upload->Unmap(0,&empty);
  }
  // Restore the same unfiltered frame on GPU before each run; replay must not
  // accumulate a residual from the preceding test frame.
  transition(ctx->cmd.Get(),target.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
  ctx->cmd->CopyTextureRegion(&texture,0,0,0,&up,nullptr);transition(ctx->cmd.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);caller.submit();
  if(engineTest){
   if(!cleanProfile&&!sequenceTest&&run==3)session->invalidateTemporalHistory();else{
    engineFrame.featureFrame=run;engineFrame.reset=run==0||(!cleanProfile&&!sequenceTest&&run==2);
    UINT bridgeRange=0;
    if(bridgeProfile){bridgeProfile->reset();bridgeRange=bridgeProfile->begin(ctx->cmd.Get(),"motion_bridge");}
    if(!session->recordEngineMotion(ctx->cmd.Get(),engineFrame,[&]{submissions.track(ctx->cmd.Get(),session);}))throw std::runtime_error("Engine motion record failed");
    if(bridgeProfile){bridgeProfile->end(ctx->cmd.Get(),bridgeRange);bridgeProfile->resolve(ctx->cmd.Get());}
    check(ctx->cmd->Close(),"Engine capture close");ID3D12CommandList* captured[]={ctx->cmd.Get()};submissions.execute(ctx->queue.Get(),1,captured,[&]{ctx->queue->ExecuteCommandLists(1,captured);});
    check(ctx->queue->Signal(ctx->fence.Get(),++ctx->serial),"Engine capture signal");check(ctx->fence->SetEventOnCompletion(ctx->serial,ctx->event),"Engine capture event");if(WaitForSingleObject(ctx->event,60000)!=WAIT_OBJECT_0)throw std::runtime_error("Engine capture timeout");if(!engineTest)submissions.collect();if(!session->ready())throw std::runtime_error("Engine producer fence failed");check(ctx->allocator->Reset(),"Capture allocator reset");check(ctx->cmd->Reset(ctx->allocator.Get(),nullptr),"Capture list reset");
   }
  }
  if(bridgeProfile)printf("ENGINE_BRIDGE_GPU frame=%u ms=%.6f\n",run,bridgeProfile->readAfterFence(ctx->queue.Get()).at("motion_bridge").ms);
  std::unique_ptr<NativeNrPairCapture> pair;
  if(engineTest&&!cleanProfile){pair=std::make_unique<NativeNrPairCapture>(ctx->device.Get(),target.Get(),session->lowWidth(),session->lowHeight());pair->recordBefore(ctx->cmd.Get(),target.Get());}
  if(detailed)detailed->reset();
  ID3D12GraphicsCommandList* list;
  auto commandStart=std::chrono::steady_clock::now();UINT gpuRange=0;
  if(temporalTest){if(!engineTest)session->attestTemporalFrame(run!=3,run==0||run==2);temporalProfile->reset();gpuRange=temporalProfile->begin(ctx->cmd.Get(),"frame");}
  if(run==0||temporalTest){if(!session->record(ctx->cmd.Get(),target.Get(),D3D12_RESOURCE_STATE_PRESENT))throw std::runtime_error("Warm recording failed");if(pair){auto d=session->lastInferenceWasTemporal()?session->motionDiagnosticTensors():std::make_pair(static_cast<Tensor*>(nullptr),static_cast<Tensor*>(nullptr));pair->recordAfter(ctx->cmd.Get(),target.Get(),session->lastInferenceWasTemporal()?session->stabilityDiagnosticTensor():nullptr,d.first,d.second);}if(temporalTest){temporalProfile->end(ctx->cmd.Get(),gpuRange);temporalProfile->resolve(ctx->cmd.Get());}if(detailed)detailed->resolve(ctx->cmd.Get());check(ctx->cmd->Close(),"Warm close");list=ctx->cmd.Get();}
  else{check(ctx->cmd->Close(),"Unused caller close");if(!cache)cache=std::make_unique<NativeNrCommandCache>(session,target.Get());if(!cache->markReadyForExecute())throw std::runtime_error("Replay not ready");list=cache->commandList();}

  auto cpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-commandStart).count();
  submissions.track(list,session);ID3D12CommandList* lists[]={list};submissions.execute(ctx->queue.Get(),1,lists,[&]{ctx->queue->ExecuteCommandLists(1,lists);});
  check(ctx->queue->Signal(ctx->fence.Get(),++ctx->serial),"Caller signal");check(ctx->fence->SetEventOnCompletion(ctx->serial,ctx->event),"Caller event");
  if(WaitForSingleObject(ctx->event,60000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU completion timeout");if(!engineTest)submissions.collect();if(!session->ready())throw std::runtime_error("Session fence lifecycle failed");
#ifdef NR_COMPACT_GEOMETRY
  if(encoderDiagnostic){for(auto& s:encoderSnapshots){void* data;UINT64 byteCount=s.tensor->elements*4;D3D12_RANGE r{0,size_t(byteCount)};check(s.readback->Map(0,&r,&data),"Encoder snapshot map");
   std::ofstream f(std::filesystem::path(encoderDirectory)/(s.name+".bin"),std::ios::binary);f.write(static_cast<char*>(data),byteCount);s.readback->Unmap(0,&empty);if(!f)throw std::runtime_error("Encoder snapshot write");}}
  for(auto& s:prefixSnapshots){void* data;D3D12_RANGE r{0,size_t(s.elements*4)};check(s.readback->Map(0,&r,&data),"First-block prefix map");
   std::ofstream f(std::filesystem::path(encoderDirectory)/("block0-"+s.name+".bin"),std::ios::binary);f.write(static_cast<char*>(data),s.elements*4);s.readback->Unmap(0,&empty);if(!f)throw std::runtime_error("First-block prefix write");}
  if(captureOracle){void* data;D3D12_RANGE r{0,size_t(oracleBytes)};check(oracleReadback->Map(0,&r,&data),"Oracle readback map");
   std::ofstream f(std::filesystem::path(oracleCapture)/("frame"+std::to_string(run)+".bin"),std::ios::binary);f.write(static_cast<char*>(data),oracleBytes);oracleReadback->Unmap(0,&empty);if(!f)throw std::runtime_error("Oracle capture write");}
#endif
#ifdef NR_CLEAN_PROFILE
  validateDenseShadowAudits();
  saveDenseRealCaptures();
  checkDenseInlinePairs();
  if(run==1&&GetEnvironmentVariableA("NR_DENSE_INLINE_COMPARE",nullptr,0))denseInlineCount=0;
  if(run==1&&GetEnvironmentVariableA("NR_STUDY_FP16_DENSE_AUDIT",nullptr,0))denseShadowAuditRecorded=0;
#endif
  if(pair){auto t=session->temporalTrace();pair->save(nativePath(argv[3])/L"pairs"/std::to_string(run+1),session->passesPerFrame(),run+1,t.consumerProducerFrame,session->lastInferenceWasTemporal());}
  if(temporalTest){auto ms=temporalProfile->readAfterFence(ctx->queue.Get()).at("frame").ms;printf("TEMPORAL_NATIVE_GPU frame=%u ms=%.6f command_cpu_ms=%.6f\n",run,ms,cpuMs);}
#ifdef NR_CLEAN_PROFILE
  if(pixCapture&&run==8){auto end=reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(pixModule,"EndProgrammaticGpuCapture"));if(!end)throw std::runtime_error("PIX end export missing");check(end(),"PIX end frame capture");puts("PIX_FRAME_CAPTURE_SAVED");}
#endif
  if(detailed)for(const auto& row:detailed->readAfterFence(ctx->queue.Get()))printf("PROFILE run=%u path=%s ms=%.6f count=%u\n",run,row.first.c_str(),row.second.ms,row.second.count);
  check(ctx->allocator->Reset(),"Caller allocator reset");check(ctx->cmd->Reset(ctx->allocator.Get(),nullptr),"Caller list reset");
  // Explicit diagnostic run only. Sparse history snapshots are not benchmark data.
  const bool timelineSample=compactTimeline&&(run==0||run==1||run==7||run==15||run+1==runCount);
  if(cleanProfile&&run+1<runCount&&!timelineSample)continue;
  transition(ctx->cmd.Get(),target.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);ctx->cmd->CopyTextureRegion(&rb,0,0,0,&texture,nullptr);
  transition(ctx->cmd.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);caller.submit();
  D3D12_RANGE range{0,size_t(bytes)};check(readback->Map(0,&range,&mapped),"Readback map");std::vector<char> result(pixels.size());
  for(UINT y=0;y<height;++y)memcpy(result.data()+size_t(y)*width*4,static_cast<char*>(mapped)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch,size_t(width)*4);readback->Unmap(0,&empty);
  if(run==0||cleanProfile)first=result;else if((!temporalTest||(!sequenceTest&&run==2))&&first!=result)throw std::runtime_error("Spatial replay or temporal reset output mismatch");
  if((temporalTest&&!cleanProfile)||timelineSample){std::filesystem::create_directories(nativePath(argv[3]));saveBytes(std::string(argv[3])+"/frame"+std::to_string(run)+".bin",result.data(),result.size());}
 }
 std::filesystem::create_directories(nativePath(argv[3]));saveBytes(std::string(argv[3])+"/output.bin",first.data(),first.size());
 auto rgb=session->captureRgb();for(float value:rgb)if(!std::isfinite(value))throw std::runtime_error("Nonfinite model output");saveBytes(std::string(argv[3])+"/low-composed.bin",rgb.data(),rgb.size()*4);
 submissions.collect();
#ifdef NR_COMPACT_GEOMETRY
 if(forwardCapture){
  std::filesystem::create_directories(forwardCaptureDirectory);
  auto inputFeatures=session->captureFeatures();
  auto& graph=session->captureGraph();auto nativeHead=graph.publicNodes().back()->runtime()->download(graph.resultTensor());
  saveBytes((std::filesystem::path(forwardCaptureDirectory)/"features.bin").string(),inputFeatures.data(),inputFeatures.size()*4);
  saveBytes((std::filesystem::path(forwardCaptureDirectory)/"head.bin").string(),nativeHead.data(),nativeHead.size()*4);
  puts("NATIVE_FORWARD_CAPTURE_PASS single_reset=1 timing_eligible=0");
 }
#endif
 if(cleanProfile){auto t=session->temporalTrace();if(!edgeTest||t.temporalConsumers!=runCount||t.spatialConsumers)throw std::runtime_error("Clean reference lost temporal/edge route");puts(encoderDiagnostic?"ENCODER_DIAGNOSIS_PASS snapshots=7 timing_eligible=0":oracleDiagnostic?"COMPACT_ORACLE_PASS timing_eligible=0 deployable=0":compactTimeline?"COMPACT_TIMELINE_PASS snapshots=5 timing_eligible=0":"CLEAN_TEMPORAL_EDGE_REFERENCE_PASS diagnostic_readbacks=0");}
 else if(engineTest){auto t=session->temporalTrace();
  if(sequenceTest){if(t.captures!=runCount||t.captureSubmissions!=runCount||t.temporalConsumers!=runCount||t.spatialConsumers!=0)throw std::runtime_error("Sequence producer/consumer accounting mismatch");}
  else if(t.captures!=3||t.captureSubmissions!=3||t.temporalConsumers!=3||t.spatialConsumers!=1||t.consumerHadInput)throw std::runtime_error("Temporal producer/consumer accounting mismatch");
  printf("TEMPORAL_CONSUMPTION_TRACE_PASS captures=%llu submits=%llu temporal=%llu spatial=%llu\n",t.captures,t.captureSubmissions,t.temporalConsumers,t.spatialConsumers);}
 if(session->passesPerFrame()!=1||session->recordedFrames()!=runCount||session->graphPasses()!=runCount)throw std::runtime_error("Global B1 pass count failed");
 check(ctx->device->GetDeviceRemovedReason(),"Device health");
 if(ctx->diagnostics){
  for(UINT64 i=0;i<ctx->diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
   SIZE_T length=0;check(ctx->diagnostics->GetMessage(i,nullptr,&length),"Debug size");std::vector<char> storage(length);
   auto message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(ctx->diagnostics->GetMessage(i,message,&length),"Debug message");
   if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){fprintf(stderr,"D3D12_ERROR %s\n",message->pDescription);throw std::runtime_error("D3D12 debug validation failed");}
  }
 }
 if(cleanProfile)printf("CLEAN_PROFILE_COMPLETED %ux%u frames=%u edge=1 temporal=1 resets=initial_only\n",width,height,runCount);
 else if(sequenceTest)printf("%s %ux%u format=%u frames=%u history_mode=%u\n",translatedTest?"RIGID_CAMERA_TEMPORAL_SEQUENCE_PASS":"STATIC_INPUT_TEMPORAL_SEQUENCE_PASS",width,height,UINT(format),runCount,historyMode);
 else if(temporalTest)printf("TEMPORAL_NATIVE_SESSION_PASS %ux%u format=%u frames=4 reset=BYTEEXACT missing_input=SPATIAL_FALLBACK\n",width,height,UINT(format));
 else printf("GLOBAL_GAME_SESSION_PASS %ux%u format=%u frames=4 passes=4 ordinary_cache_replay=BYTEEXACT\n",width,height,UINT(format));if(engineTest)puts("ENGINE_CAPTURE_SUBMIT_HISTORY_ADVANCE_PASS");return 0;
}catch(const std::exception& e){fprintf(stderr,"GLOBAL_GAME_SESSION_FAILED %s\n",e.what());return 1;}}
