from pathlib import Path
import subprocess,json,hashlib,os,zipfile,uuid,sys
bseries=True  # Current generic release; the archived B580-only EXE remains unchanged.
root=Path.cwd();bank=root/'work/b580/generic-installer';helper=bank/'installer-test.exe';setup=root/'dist'/('OptiScaler-NR-General-B570-B580-Setup.exe' if bseries else 'OptiScaler-NR-General-B580-Setup.exe');payload=bank/('payload-bseries.zip' if bseries else 'payload.zip');base=bank/'validation'/uuid.uuid4().hex[:8];base.mkdir(parents=True)
live=[Path(r"<GAME_LIBRARY>\steamapps\common\Marvel's Spider-Man 2\dxgi.dll"),Path(r'<GAME_LIBRARY>\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem\dxgi.dll')]
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
original={str(p):sha(p) for p in live};results=[]
def run(label,args,expected=0):
 p=subprocess.run([str(helper)]+list(map(str,args)),capture_output=True,text=True,timeout=360)
 (bank/(label+'.log')).write_text(p.stdout+p.stderr)
 assert p.returncode==expected,(label,p.stdout[-1800:]+p.stderr)
 results.append(label);print('GENERIC_INSTALLER_PASS',label,flush=True)
run('embedded-verify',['--verify',setup])
if bseries:
 for label,id,expected in [('b580','PCI\\VEN_8086&DEV_E20B&SUBSYS_0000',0),('b570','PCI\\VEN_8086&DEV_E20C&SUBSYS_0000',0),('wrong-vendor','PCI\\VEN_10DE&DEV_E20C',1),('other-intel','PCI\\VEN_8086&DEV_56A0',1),('prefix-not-device','PCI\\VEN_8086&DEV_E20C0',1)]:
  run('hardware-'+label,['--hardware-test',id],expected)
folder=base/'juego de prueba ñ';folder.mkdir();exe=folder/'Spider-Man2.exe';os.link(helper,exe)
(folder/'dxgi.dll').write_bytes(b'original proxy DLL');(folder/'OptiScaler.ini').write_text('[Spoofing]\nDxgi=auto\n[User]\nKeep=1\n')
(folder/'native_nr.ini').write_text('[NativeNR]\nOld=keep\n');(folder/'other-mod.dll').write_bytes(b'untouched')
before={name:sha(folder/name) for name in ['dxgi.dll','OptiScaler.ini','native_nr.ini','other-mod.dll','Spider-Man2.exe']}
run('full-install',['--install-test',payload,exe,helper])
expected_dll=json.loads((bank/'build-result.json').read_text())['dll_sha256'] if bseries else 'e36e447df8ce4134dfb1d75ac8a6537a98d0fa78cb8338a9f69749bc6ce0887d'
assert sha(folder/'dxgi.dll')==expected_dll
assert 'Dxgi = false' in (folder/'OptiScaler.ini').read_text() and 'Keep=1' in (folder/'OptiScaler.ini').read_text()
profile=json.loads((folder/'nr-launcher/profile.json').read_text());assert profile['executable']=='Spider-Man2.exe' and len(profile['asset_manifests'])==5
p=subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(folder/'nr-launcher/Start.ps1'),'-VerifyOnly'],capture_output=True,text=True,timeout=45)
(bank/'launcher-verify.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stdout+p.stderr;results.append('launcher-verify-without-os-change')
# The common model weights are physically shared between the five folders.
a=folder/'native_nr_modes/assets-180/block0/conv_weight.bin'
for candidate in (folder/'native_nr_modes/assets-180/block0').glob('*.bin'):
 other=folder/'native_nr_modes/assets-720/block0'/candidate.name
 if other.is_file() and sha(candidate)==sha(other):
  assert candidate.stat().st_ino==other.stat().st_ino;results.append('ntfs-shared-weights');break
else:raise AssertionError('No shared weight fixture found')
run('duplicate-install-blocked',['--install-test',payload,exe,helper],1)
(folder/'nr-docs/LEEME-General.txt').write_text('user changed this file')
run('retain-user-change',['--uninstall-test',folder])
assert (folder/'nr-docs/LEEME-General.txt').read_text()=='user changed this file'
assert not json.loads((folder/'nr-general-installer/installation.json').read_text())['removed']
assert all(sha(folder/n)==h for n,h in before.items())
with zipfile.ZipFile(payload) as z:
 m=json.loads(z.read('package.json'));item=next(i for i in m['files'] if i['path']=='nr-docs/LEEME-General.txt');(folder/'nr-docs/LEEME-General.txt').write_bytes(z.read('blobs/'+item['blob']))
run('complete-uninstall',['--uninstall-test',folder]);assert json.loads((folder/'nr-general-installer/installation.json').read_text())['removed']
def small(name,paths,bad=False):
 out=base/(name+'.zip');content=b'generic test content';digest=hashlib.sha256(content).hexdigest();items=[]
 with zipfile.ZipFile(out,'w') as z:
  for path in paths:items.append(dict(path=path,sha256='0'*64 if bad else digest,blob='0'*64 if bad else digest,bytes=len(content),mode='replace',component='Test'))
  z.writestr('blobs/'+('0'*64 if bad else digest),content);z.writestr('package.json',json.dumps(dict(version='test',scope='test',files=items)))
 return out
for label,paths,bad in [('path-traversal',['../outside.dll'],False),('tampered-blob',['dxgi.dll'],True),('wrapper-collision',['dxgi.dll','version.dll'],False)]:
 f=base/label;f.mkdir();e=f/'NRTestGame64.exe';os.link(helper,e)
 run(label,['--install-test',small(label,paths,bad),e,helper]+(['version.dll'] if label=='wrapper-collision' else []),1)
 assert not (f/'nr-general-installer').exists()
f=base/'rollback';f.mkdir();e=f/'NRTestGame64.exe';os.link(helper,e);(f/'dxgi.dll').write_bytes(b'old DLL');(f/'conflict.bin').mkdir()
run('rollback',['--install-test',small('rollback',['dxgi.dll','conflict.bin']),e,helper],1);assert (f/'dxgi.dll').read_bytes()==b'old DLL'
f=base/'alternative';f.mkdir();e=f/'NRTestGame64.exe';os.link(helper,e);(f/'OptiScaler.ini').write_text('user config')
run('alternative-version',['--install-test',small('alternative',['dxgi.dll']),e,helper,'version.dll']);assert (f/'version.dll').exists() and not (f/'dxgi.dll').exists()
assert json.loads((f/'nr-launcher/profile.json').read_text())['dll']=='version.dll'
run('alternative-restore',['--uninstall-test',f]);assert not (f/'version.dll').exists() and (f/'OptiScaler.ini').read_text()=='user config'
f=base/'requiem-protected';f.mkdir();e=f/'re9.exe';os.link(helper,e)
run('requiem-specific-profile-required',['--install-test',small('requiem',['dxgi.dll']),e,helper],1);assert not (f/'nr-general-installer').exists()
f=base/'invalid-exe';f.mkdir();e=f/'NRInvalid.exe';e.write_bytes(b'not an executable')
run('invalid-executable',['--install-test',small('invalid',['dxgi.dll']),e,helper],1)
assert all(sha(Path(p))==h for p,h in original.items())
report=dict(passed=True,tests=results,fixture=str(base),live_games_unchanged=True,real_game_started=False,installer_sha256=sha(setup))
(bank/'validation.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
