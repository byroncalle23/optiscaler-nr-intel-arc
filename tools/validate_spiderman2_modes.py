from pathlib import Path
import subprocess,os,json,re,statistics,numpy as np
root=Path.cwd();bank=root/'work/b580/spiderman2-modes';exe=root/'work/b580/pix-profile/runner/compact_game_session_probe.exe';fixture=root/'work/b580/temporal/edge-real-edge-stability/pairs/1/before.bin'
cases=[]
for w,h in [(320,180),(512,288),(640,360),(960,540),(1280,720)]:
 if subprocess.run(['powershell','-NoProfile','-Command','if(Get-Process Spider-Man2,re9 -ErrorAction SilentlyContinue){exit 1}'],capture_output=True).returncode:raise RuntimeError('Close games before benchmark')
 out=bank/f'gpu-{w}x{h}';out.mkdir(exist_ok=False);env={k:v for k,v in os.environ.items() if not k.startswith('NR_')};env['NR_CLEAN_PROFILE']='1'
 print('MODE_RUNNING',w,h,flush=True)
 p=subprocess.run([str(exe),str(bank/f'assets-{h}'),str(fixture),str(out),'2560','1440','--engine-edge-stability','unused'],cwd=exe.parent,env=env,capture_output=True,text=True,timeout=240)
 (out/'stdout.log').write_text(p.stdout);(out/'stderr.log').write_text(p.stderr)
 assert p.returncode==0,p.stderr+p.stdout[-1600:]
 assert 'CLEAN_TEMPORAL_EDGE_REFERENCE_PASS diagnostic_readbacks=0' in p.stdout and 'ENGINE_CAPTURE_SUBMIT_HISTORY_ADVANCE_PASS' in p.stdout
 low=np.fromfile(out/'low-composed.bin',np.float32);assert len(low)==w*h*3 and np.isfinite(low).all()
 original=np.fromfile(fixture,np.uint32);result=np.fromfile(out/'output.bin',np.uint32);assert original.shape==result.shape and np.array_equal(original>>30,result>>30)
 a={int(f):float(ms) for f,ms in re.findall(r'TEMPORAL_NATIVE_GPU frame=(\d+) ms=([\d.]+)',p.stdout)}
 b={int(f):float(ms) for f,ms in re.findall(r'ENGINE_BRIDGE_GPU frame=(\d+) ms=([\d.]+)',p.stdout)}
 warm=[a[f]+b[f] for f in a if f>=8];assert len(warm)==32
 case=dict(mode=[w,h],gpu_combined_median_ms=statistics.median(warm),temporal_edge=True,finite=True,alpha_preserved=True);cases.append(case);print('MODE_PASS',case,flush=True)
 (bank/'preflight-progress.json').write_text(json.dumps(cases,indent=2))
(bank/'preflight.json').write_text(json.dumps(dict(passed=True,cases=cases,live_switch_validated=False,quality_accepted=False,fixture='historical Requiem1440 for offline comparison; no Spider-Man visual claim'),indent=2))
