"""Build two public packages, excluding all model assets and experimental SDK binaries."""
from pathlib import Path
import hashlib,json,struct,subprocess,zipfile,shutil,io,argparse
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--local-dist',type=Path,default=root/'dist');args=ap.parse_args()
bank=root/'work/b580/public-installers';bank.mkdir(parents=True,exist_ok=True)
dist=args.local_dist.resolve();dist.mkdir(parents=True,exist_ok=True)
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def local_zip(path):
 with path.open('rb') as f:
  f.seek(-32,2);marker,offset,length=struct.unpack('<16sqq',f.read(32));assert marker==b'NRSETUPGENV1PACK';f.seek(offset);data=f.read(length)
 return zipfile.ZipFile(io.BytesIO(data))
src=root/'work/optiscaler-source'
libraries={
 'amd_fidelityfx_dx12.dll':src/'external/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll',
 **{n:src/'external/FidelityFX-SDK-v2/Kits/FidelityFX/signedbin'/n for n in ['amd_fidelityfx_upscaler_dx12.dll','amd_fidelityfx_framegeneration_dx12.dll','amd_fidelityfx_loader_dx12.dll']},
 **{n:src/'external/xess/bin'/n for n in ['libxess.dll','libxess_fg.dll','libxell.dll','libxess_dx11.dll']}}
license_roots={'FidelityFX':src/'external/FidelityFX-SDK','FidelityFX-v2':src/'external/FidelityFX-SDK-v2','XeSS':src/'external/xess'}
variants=[('B580','OptiScaler-NR-General-B580-Setup.exe','c57e6016c3221fb88feec8e83506bc301ea0dccba848b810fb1f99a8734fb8b4'),('B570-B580','OptiScaler-NR-General-B570-B580-Setup.exe','73154d0fa4dd3be2aeb09c262994cc7faee1288271fa94c5e4f85d2d09194334')]
results=[]
for variant,filename,pin in variants:
 source=args.local_dist/filename;assert sha(source)==pin,'Original local package changed'
 with local_zip(source) as z:
  original=json.loads(z.read('package.json'));byname={x['path']:x for x in original['files']};files={}
  for name in ['dxgi.dll','OptiScaler.ini','nr-docs/OptiScaler-LICENSE.txt','Desinstalar-OptiScaler-NR.bat']:
   item=byname[name];data=z.read('blobs/'+item['blob']);assert hashlib.sha256(data).hexdigest()==item['sha256'];files[name]=data
  for name,p in libraries.items():files[name]=p.read_bytes()
  ini=z.read('blobs/'+byname['native_nr.ini']['blob']).decode('utf-8-sig');assert 'Enabled=0' in ini and 'GlobalB1=1' in ini
  files['native_nr.ini']=ini.replace('GlobalB1=1','GlobalB1=0').encode()
  contract=[dict(path=x['path'],bytes=x['bytes'],sha256=x['sha256']) for x in original['files'] if x['path'].startswith('native_nr_runtime/')]
  files['nr-launcher/runtime-contract.json']=json.dumps(contract,indent=2).encode()
 launcher=(root/'work/b580/generic-installer/generated/Start.ps1').read_text(encoding='utf-8-sig')
 needle="if($VerifyOnly){Write-Output"
 guard="if(-not $VerifyOnly -and $nrProfile.model_required){throw 'Falta el modelo/runtime local. Consulta LEEME-Publico.txt; OptiScaler base puede usarse iniciando el juego normalmente.'}\n"
 assert needle in launcher;launcher=launcher.replace(needle,guard+needle)
 files['nr-launcher/Start.ps1']=launcher.encode()
 files['Launch-OptiScaler-NR.bat']=(root/'work/b580/generic-installer/generated/Launch-OptiScaler-NR.bat').read_bytes()
 readme=f'''OptiScaler + NR / {variant} / PAQUETE PUBLICO SIN PESOS

Incluye DLL de integracion y bibliotecas OptiScaler procedentes de sus SDKs.
NO incluye pesos ni el SDK experimental Microsoft 721 (su licencia no permite
redistribuirlo). NR no esta listo despues de instalar este paquete solo.
OptiScaler base puede utilizarse iniciando el juego normalmente, con NR apagado.

Para NR: aporta legalmente tu paquete local compatible en native_nr_modes,
con assets-180/288/360/540/720 y sus manifiestos completos, y el runtime 721.3
obtenido de su proveedor en native_nr_runtime. No se descarga ningun peso.
Con el juego cerrado, vuelve a abrir este instalador, selecciona el mismo
ejecutable y pulsa Modelo local. Verifica hashes/geometrias/runtime y registra
el perfil; NR sigue apagado hasta activarlo desde Insert > Custom.
Despues usa Launch-OptiScaler-NR.bat como administrador y deja la ventana abierta.
Para comprobar sin registrar: nr-general-installer/Uninstall.exe --check-model GAME.exe

Los archivos de modelo/runtime aportados por ti no se borran al desinstalar;
el registro restaura solo los archivos que administra. Conserva tu respaldo.
B570: experimental, sin prueba fisica. DX12/SDR; otros juegos no validados.
Requiem requiere su integracion especifica. No hay garantia de FPS.
Codigo y documentacion: https://github.com/byroncalle23/optiscaler-nr-intel-arc
'''
 files['LEEME-Publico.txt']=readme.encode()
 files['nr-docs/SOURCE.txt']=b'Corresponding source: https://github.com/byroncalle23/optiscaler-nr-intel-arc\nOptiScaler base commit: 7651154215172af82d9d39ef5efb3798ff1e3703\n'
 for label,folder in license_roots.items():
  for p in sorted(folder.rglob('*')):
   if p.is_file() and p.suffix.lower() in ['.txt','.md'] and any(s in p.name.lower() for s in ['license','notice','third-party','thirdparty']):
    files['nr-docs/licenses/'+label+'/'+p.relative_to(folder).as_posix()]=p.read_bytes()
 items=[];blobs={}
 for name,data in files.items():
  assert not name.startswith(('native_nr_modes/','native_nr_runtime/'))
  digest=hashlib.sha256(data).hexdigest();blobs[digest]=data
  items.append(dict(path=name,sha256=digest,blob=digest,bytes=len(data),mode='preserve' if name=='OptiScaler.ini' else 'replace',component='Paquete publico sin pesos'))
 manifest=dict(version='1.2.0-public-no-model-'+variant,scope='B580 tested; B570 experimental if selected; model/runtime supplied locally',weights_included=False,experimental_sdk_included=False,files=items)
 stage=bank/variant;stage.mkdir(exist_ok=True)
 (stage/'package.json').write_text(json.dumps(manifest,indent=2))
 archive=stage/'payload.zip'
 with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
  z.writestr('package.json',json.dumps(manifest))
  for digest,data in blobs.items():z.writestr('blobs/'+digest,data)
 csc=Path('C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe');refs=['System.Windows.Forms.dll','System.Drawing.dll','System.Web.Extensions.dll','System.IO.Compression.dll','System.IO.Compression.FileSystem.dll','System.Management.dll']
 compiler=[str(csc),'/nologo','/optimize+','/platform:x64','/define:PUBLIC_PACKAGE'+(';B580_ONLY' if variant=='B580' else '')]+['/reference:'+r for r in refs]
 for target,name in [('winexe','bootstrap.exe'),('exe','installer-test.exe')]:
  subprocess.run(compiler+['/target:'+target,'/out:'+str(stage/name)]+(['/win32manifest:'+str(root/'work/b580/generic-installer/generated/app.manifest')] if target=='winexe' else [])+[str(root/'installer/GenericInstaller.cs')],check=True)
 output=dist/('OptiScaler-NR-Publico-'+variant+'-SinPesos-Setup.exe');shutil.copyfile(stage/'bootstrap.exe',output);offset=output.stat().st_size
 with output.open('ab') as f,archive.open('rb') as inp:shutil.copyfileobj(inp,f);f.write(b'NRSETUPGENV1PACK'+struct.pack('<qq',offset,archive.stat().st_size))
 result=dict(variant=variant,file=output.name,sha256=sha(output),bytes=output.stat().st_size,files=len(items),weights_included=False,experimental_sdk_included=False,ready_for_nr_without_local_model=False)
 results.append(result);print(json.dumps(result),flush=True)
(bank/'build-result.json').write_text(json.dumps(results,indent=2))
(dist/'PUBLIC-INSTALLERS-SHA256.txt').write_text(''.join(x['sha256']+'  '+x['file']+'\n' for x in results))
