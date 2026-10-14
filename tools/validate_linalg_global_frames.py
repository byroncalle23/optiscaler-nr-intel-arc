"""Global B1 GPU pipeline: integral reduction, one graph, upsampled residual."""
from pathlib import Path
import os, subprocess, json, re, hashlib
import numpy as np
from nr_acceptance import RGB_RMSE_LIMIT
root=Path(__file__).resolve().parents[1];parent=root/'work/b580/cooperative-vectors/linalg7213'
bank=parent/'global-b1';fixture=root/'work/b580/requiem-live/menu-detail-confirmed/before.bin'
env=os.environ.copy()
for key in list(env):
    if key.startswith('NR_GRAPH_LINALG') or key in ('NR_FRAME_PROFILE','NR_BATCH_ISOLATE','NR_BATCH_PACK_ONLY'):env.pop(key)
env.update(NR_D3D12_DEBUG='1',NR_FRAME_RUNS='8',NR_FRAME_REPLAY='1')
for key in ['NR_DML_ALLOW_HALF','NR_DML_TENSOR_HALF','NR_DML_PREPACK_HALF','NR_DML_CONV_HALF','NR_DML_DIRECT_COMPUTE','NR_ATTENTION_SCALAR','NR_WINDOW_NATIVE_HALF','NR_SPLIT_COMPENSATED_PROJECTION','NR_ADAPTER_COMPENSATED_PROJECTION']:env[key]='0'
for key in ['NR_WINDOW_PARALLEL_NORM','NR_WINDOW_FUSED_GATE','NR_WINDOW_FUSED_FEED_PACK']:env[key]='1'
report={'passed':False,'production_deployed':False,'visual_validated':False,'temporal_validated':False,
    'scope':'Static SDR fixture; one global B1 graph, low-res composed residual added to1920x1080 base; not live gameplay',
    'rgb_relative_rmse_limit':RGB_RMSE_LIMIT,'cases':[],'comparisons':[],
    'artifact_hashes':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in bank.iterdir() if p.suffix in ('.exe','.dxil','.hlsl')},
    'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest()}
def save(): (bank/'validation.json').write_text(json.dumps(report,indent=2))
def guard():
    p=subprocess.run(['powershell','-NoProfile','-Command',"Get-Process -ErrorAction Stop | Where-Object { $_.ProcessName -in @('re9','GTA5_Enhanced','linalg_global_frame','linalg_batch_frame','linalg_batch_graph','linalg_attention_unit','linalg_graph_diagnostic') } | Select-Object -ExpandProperty ProcessName"],capture_output=True,text=True,check=True)
    if p.stdout.strip():raise RuntimeError('Another GPU workload is active')
def decode(path,h,w):
    p=np.fromfile(path,np.uint32).reshape(h,w)
    return np.stack((p&1023,(p>>10)&1023,(p>>20)&1023),axis=-1).astype(np.float32)/np.float32(1023)
def resize(value,h,w):
    def axis(out,n):
        d=2*out;u=np.clip((2*np.arange(out,dtype=np.int32)+1)*n-out,0,(n-1)*d)
        a=u//d;return a,np.minimum(a+1,n-1),(u%d).astype(np.float32)/np.float32(d)
    y,yb,fy=axis(h,value.shape[0]);x,xb,fx=axis(w,value.shape[1]);fx=fx[None,:,None];fy=fy[:,None,None]
    a=value[y[:,None],x[None,:]];b=value[y[:,None],xb[None,:]]
    c=value[yb[:,None],x[None,:]];d=value[yb[:,None],xb[None,:]]
    top=a+(b-a)*fx;bottom=c+(d-c)*fx
    return top+(bottom-top)*fy
def rmse(a,b):
    a=a.astype(np.float64);b=b.astype(np.float64)
    return float(np.sqrt(np.mean((a-b)**2))/max(np.sqrt(np.mean(a*a)),1e-12))
def run(exe,folder,args,local):
    guard()
    if folder.exists():raise RuntimeError('Existing result case; preserve it before rerunning')
    folder.mkdir();print('RUNNING '+folder.name,flush=True)
    p=subprocess.run([str(exe),*map(str,args)],env=local,capture_output=True,text=True,timeout=600)
    (folder/'stdout.log').write_text(p.stdout);(folder/'stderr.log').write_text(p.stderr)
    if p.returncode:raise RuntimeError(folder.name+': '+p.stderr[-3000:])
    guard();hashes=[hashlib.sha256((folder/f'frame{i}.bin').read_bytes()).hexdigest() for i in range(8)]
    if len(set(hashes))!=1:raise RuntimeError('Ordinary/cached/replay outputs differ')
    return p.stdout
try:
    # Same-session existing tiled route for latency comparison; its output is1440p
    # because the legacy fixture adapter keeps physical textureextent unchanged.
    tileFolder=bank/'tiled-1080-b4';local=env.copy();local.update(NR_GRAPH_LINALG='1',NR_GRAPH_LINALG_ATTENTION='1',NR_GRAPH_LINALG_WINDOW_FUSED='1')
    log=run(parent/'window-fused/full-frames/linalg_batch_frame.exe',tileFolder,[root/'work/b580/normal-startup/native_nr_assets',fixture,tileFolder,4,1920,1080],local)
    tiledWarm=[float(ms) for r,ms in re.findall(r'RUN (\d+) B 4 WIDTH 1920 HEIGHT 1080 TILES 45 BATCHES 12 CPU [\d.]+ GPU ([\d.]+)',log) if int(r)>=2]
    if len(tiledWarm)!=6:raise RuntimeError('Tiled baseline grid/sample mismatch')
    tileMedian=float(np.median(tiledWarm));tileRGB=resize(decode(tileFolder/'frame0.bin',1440,2560),1080,1920)
    report['tiled_baseline']={'median_gpu_ms':tileMedian,'warm_gpu_ms':tiledWarm,'same_session':True,'tiles':45,'batches':12,'repeat_exact':True};save()
    original=decode(fixture,1440,2560);baseOracle=resize(original,1080,1920)
    for width,height in [(640,360),(960,540)]:
        geometry=json.loads((bank/f'assets-{height}/geometry.json').read_text());outputs={};medians={}
        for native in (False,True):
            mode='native' if native else 'dml';folder=bank/f'{height}-{mode}'
            log=run(bank/'linalg_global_frame.exe',folder,[bank/f'assets-{height}',fixture,folder,width,height,int(native)],env)
            if f'GLOBAL B 1 GRAPH_PASSES 1 LOW {width} {height}' not in log:raise RuntimeError('Not a single global B1 pass')
            if f'GLOBAL_TOKENS {geometry["tokens"]}' not in log:raise RuntimeError('Actual bottleneck token count differs')
            if native and 'FUSED_WINDOWS 62' not in log:raise RuntimeError('Native window path missing')
            samples={}
            for r,name,ms in re.findall(r'TIME run=(\d+) name=(\S+) ms=([\d.]+)',log):
                if int(r)>=2:samples.setdefault(name,[]).append(float(ms))
            if any(len(v)!=6 for v in samples.values()) or 'frame' not in samples:raise RuntimeError('Missing global timing samples')
            medians[native]=float(np.median(samples['frame']))
            base=np.fromfile(folder/'base.bin',np.float32).reshape(1080,1920,3)
            low=np.fromfile(folder/'low.bin',np.float32).reshape(height,width,3)
            composed=np.fromfile(folder/'composed.bin',np.float32).reshape(height,width,3)
            if not all(np.isfinite(v).all() for v in (base,low,composed)):raise RuntimeError('Nonfinite global activations')
            lowOracle=baseOracle.reshape(height,1080//height,width,1920//width,3).mean(axis=(1,3),dtype=np.float32)
            baseError=float(np.max(np.abs(base-baseOracle)));lowError=float(np.max(np.abs(low-lowOracle)))
            if max(baseError,lowError)>3e-6:raise RuntimeError('GPU resample/reduction oracle failed')
            result=decode(folder/'frame0.bin',1080,1920);outputs[native]=result
            finalOracle=np.clip(base+resize(composed-low,1080,1920),0,1)
            quantized=np.rint(finalOracle*np.float32(1023))/np.float32(1023)
            composeError=float(np.max(np.abs(result-quantized)))
            if composeError>1/1023+2e-6:raise RuntimeError('GPU residual upsample/composition oracle failed')
            report['cases'].append({'height':height,'mode':mode,'tokens':geometry['tokens'],'network_width':geometry['network_width'],'network_height':geometry['network_height'],
                'median_gpu_ms':medians[native],'warm_gpu_ms':samples['frame'],'stage_median_ms':{k:float(np.median(v)) for k,v in samples.items()},
                'repeat_replay_exact':True,'base_oracle_max_abs':baseError,'reduction_oracle_max_abs':lowError,'composition_oracle_max_abs':composeError,
                'tracked_buffer_bytes':int(re.findall(r'BUFFER_BYTES (\d+)',log)[-1])});save()
        error=rmse(outputs[False],outputs[True])
        report['comparisons'].append({'height':height,'rgb_relative_rmse_native_vs_same_geometry_dml':error,
            'rgb_relative_rmse_vs_tiled_diagnostic':rmse(tileRGB,outputs[True]),
            'saved_ms_vs_tiled':tileMedian-medians[True],'speedup_vs_tiled':tileMedian/medians[True],
            'gpu_delta_percent_vs_tiled':100*(medians[True]/tileMedian-1)})
        save()
        if error>RGB_RMSE_LIMIT:raise RuntimeError('Same-geometry native/DML RGB exceeds acceptance')
    report['passed']=True;save();print(json.dumps(report,indent=2),flush=True)
except Exception as e:report['error']=str(e);save();raise
