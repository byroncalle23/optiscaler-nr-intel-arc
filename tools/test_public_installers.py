"""Offline tests of public packages and local-only model registration; never launch games."""
from pathlib import Path
import json,subprocess,os,hashlib,uuid,shutil,zipfile,sys,urllib.request
guided='--guided' in sys.argv
root=Path(__file__).resolve().parents[1];bank=root/('work/b580/public-guided-installers' if guided else 'work/b580/public-installers');dist=root/('dist/guided' if guided else 'dist')
source=Path(r"<GAME_LIBRARY>\steamapps\common\Marvel's Spider-Man 2")
live=[source/'dxgi.dll',Path(r'<GAME_LIBRARY>\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem/dxgi.dll')]
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
before={str(p):sha(p) for p in live};results=[]
for build in json.loads((bank/'build-result.json').read_text()):
 variant=build['variant'];stage=bank/variant;helper=stage/'installer-test.exe';case=stage/('test-'+uuid.uuid4().hex[:8]);case.mkdir()
 def run(label,args,code=0):
  p=subprocess.run([str(helper)]+list(map(str,args)),capture_output=True,text=True,timeout=360)
  (case/(label+'.log')).write_text(p.stdout+p.stderr);assert p.returncode==code,(label,p.stdout[-1000:]+p.stderr)
  results.append(variant+':'+label);print('PUBLIC_INSTALLER_PASS',variant,label,flush=True)
 run('verify',['--verify',dist/build['file']])
 run('b580',['--hardware-test','PCI\\VEN_8086&DEV_E20B'])
 run('b570-policy',['--hardware-test','PCI\\VEN_8086&DEV_E20C'],1 if variant=='B580' else 0)
 if guided:
  for name in ['Spider-Man2.exe','re9.exe','re2.exe','re3.exe','re4.exe','Unknown.exe']:run('profile-'+name,['--profile-test',name])
 with zipfile.ZipFile(stage/'payload.zip') as z:
  m=json.loads(z.read('package.json'));assert not m['weights_included'] and not m['experimental_sdk_included']
  assert not any(x['path'].startswith(('native_nr_modes/','native_nr_runtime/')) for x in m['files'])
  assert not any(x['path'].endswith(('.bin','.onnx','.safetensors','.dxil')) for x in m['files'])
  assert all(hashlib.sha256(z.read('blobs/'+x['blob'])).hexdigest()==x['sha256'] for x in m['files'])
 game=case/'juego de prueba ñ';game.mkdir();exe=game/'NRPublicTest64.exe';os.link(helper,exe)
 (game/'dxgi.dll').write_bytes(b'original DLL');(game/'OptiScaler.ini').write_text('[User]\nKeep=1\n')
 run('install',['--install-test',stage/'payload.zip',exe,helper])
 assert 'GlobalB1=0' in (game/'native_nr.ini').read_text() and 'Enabled=0' in (game/'native_nr.ini').read_text()
 assert json.loads((game/'nr-launcher/profile.json').read_text())['model_required']
 run('missing-model',['--check-model',exe],1)
 if guided and variant=='B570-B580':
  sdk=bank/'official-1.721.3-preview.nupkg'
  if not sdk.exists():urllib.request.urlretrieve('https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.721.3-preview/microsoft.direct3d.d3d12.1.721.3-preview.nupkg',sdk)
  output=case/'official-sdk';output.mkdir()
  run('official-sdk-contract',['--extract-sdk-test',sdk,output,game/'nr-launcher/runtime-contract.json'])
  run('automatic-local-import',['--import-local',exe,source,output])
 p=subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(game/'nr-launcher/Start.ps1'),'-VerifyOnly'],capture_output=True,text=True,timeout=30)
 assert p.returncode==0,p.stdout+p.stderr;results.append(variant+':launcher-verify')
 for p in (source/'native_nr_modes').rglob('*'):
  if p.is_file():
   dest=game/p.relative_to(source);dest.parent.mkdir(parents=True,exist_ok=True)
   if dest.exists():continue
   if p.name=='assets.txt':shutil.copyfile(p,dest)
   else:os.link(p,dest)
 for item in json.loads((game/'nr-launcher/runtime-contract.json').read_text()):
  dst=game/item['path'];dst.parent.mkdir(parents=True,exist_ok=True)
  if not dst.exists():shutil.copyfile(source/item['path'],dst)
 manifest=game/'native_nr_modes/assets-180/assets.txt';saved=manifest.read_bytes();manifest.write_text('NR_NATIVE_ASSETS 1\ngeometry wrong\nfiles 0\n')
 run('reject-wrong-geometry',['--check-model',exe],1);manifest.write_bytes(saved)
 run('verify-local-model',['--check-model',exe]);assert ('GlobalB1=1' if guided and variant=='B570-B580' else 'GlobalB1=0') in (game/'native_nr.ini').read_text()
 run('register-local-model',['--register-model',exe]);profile=json.loads((game/'nr-launcher/profile.json').read_text())
 assert not profile['model_required'] and len(profile['asset_manifests'])==5
 assert 'GlobalB1=1' in (game/'native_nr.ini').read_text() and 'Enabled=0' in (game/'native_nr.ini').read_text()
 run('uninstall',['--uninstall-test',game]);assert (game/'dxgi.dll').read_bytes()==b'original DLL'
 assert 'Keep=1' in (game/'OptiScaler.ini').read_text()
 assert manifest.exists()==(not (guided and variant=='B570-B580'))
 # User-supplied model remains by design; remove only the isolated test links/copies.
 resolved=game.resolve();assert resolved.is_relative_to(bank.resolve()) and case.name.startswith('test-')
 for folder in [game/'native_nr_modes',game/'native_nr_runtime']:
  if folder.exists():shutil.rmtree(folder)
assert all(sha(Path(p))==h for p,h in before.items())
report=dict(passed=True,tests=results,real_game_started=False,live_games_unchanged=True,windows_settings_changed=False,public_packages_have_model=False)
(bank/'validation.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
