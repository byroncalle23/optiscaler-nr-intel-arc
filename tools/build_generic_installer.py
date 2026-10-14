from pathlib import Path
import json,hashlib,subprocess,zipfile,struct,shutil,sys
root=Path.cwd();bank=root/'work/b580/generic-installer';stage=bank/'generated';dist=root/'dist';dist.mkdir(exist_ok=True)
bseries=True  # The current generic package always pairs both-card checks with the both-card DLL.
game=Path(r'<GAME_LIBRARY>\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem')
dll=root/('work/b580/optiscaler-build/global-b1-modes-bseries/bin/OptiScaler.dll' if bseries else 'work/b580/optiscaler-build/global-b1-modes/bin/OptiScaler.dll')
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(dll)==('249b08d43612737aba66ed462ca4f5c5f98d9764512ebf35836d322c6092e16f' if bseries else 'e36e447df8ce4134dfb1d75ac8a6537a98d0fa78cb8338a9f69749bc6ce0887d')
paths={};items=[];blobs={}
def add(name,path,component,mode='replace'):
 assert name not in paths and path.is_file(),name
 digest=sha(path);paths[name]=path;items.append(dict(path=name,sha256=digest,blob=digest,bytes=path.stat().st_size,mode=mode,component=component));blobs.setdefault(digest,path)
def generated(name,text,component,mode='replace'):
 p=stage/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text,encoding='utf-8',newline='');add(name,p,component,mode)
add('dxgi.dll',dll,'OptiScaler + NR cinco modos')
for name in ('amd_fidelityfx_dx12.dll','amd_fidelityfx_upscaler_dx12.dll','amd_fidelityfx_framegeneration_dx12.dll','amd_fidelityfx_loader_dx12.dll','libxess.dll','libxess_fg.dll','libxell.dll'):
 add(name,game/name,'Reescalado y generacion de fotogramas')
add('libxess_dx11.dll',root/'work/optiscaler-source/external/xess/bin/libxess_dx11.dll','XeSS DX11 (NR solo DX12)')
for folder in ('native_nr_modes','native_nr_runtime'):
 for p in sorted((game/folder).rglob('*')):
  if p.is_file():add(p.relative_to(game).as_posix(),p,'Modelo completo cinco modos' if folder.endswith('modes') else 'Runtime privado NR')
add('OptiScaler.ini',root/'work/optiscaler-source/OptiScaler.ini','Ajustes OptiScaler','preserve')
add('native_nr.ini',game/'native_nr.ini','Ajustes NR')
add('nr-launcher/Start.ps1',stage/'Start.ps1','Lanzador general')
add('Launch-OptiScaler-NR.bat',stage/'Launch-OptiScaler-NR.bat','Acceso al juego')
generated('Desinstalar-OptiScaler-NR.bat','@echo off\r\nstart "" "%~dp0nr-general-installer\\Uninstall.exe"\r\n','Restauracion')
license_text=subprocess.run(['git','-C',str(root/'work/optiscaler-source'),'show','HEAD:LICENSE'],capture_output=True,text=True,check=True).stdout
generated('nr-docs/OptiScaler-LICENSE.txt',license_text,'Licencias')
for label,p in [('XeSS',root/'work/optiscaler-source/external/xess/LICENSE.txt'),('FidelityFX',root/'work/optiscaler-source/external/FidelityFX-SDK/LICENSE.txt'),('Agility',root/'work/optiscaler-source/external/directx_agility_sdk/LICENSE.txt'),('Recovered-model-code',root/'work/mlx-dlss/LICENSE')]:
 add('nr-docs/'+label+'-LICENSE.txt',p,'Licencias')
readme='''OptiScaler + Neural Rendering — paquete general local / Intel Arc B580

Selecciona el ejecutable real de un juego DX12. DXGI es la carga predeterminada;
version.dll y winmm.dll son alternativas si el juego lo requiere.
Referencia funcional: Spider-Man 2, con entrada FSR 3.1.5. Otros juegos no validados.
NR: DirectX 12, SDR y Arc B580. OptiScaler base conserva sus demas rutas.
No se garantiza compatibilidad universal. El temporal requiere recursos/estados
verificados del juego; una entrada desconocida no habilita reproyeccion a ciegas.
Requiem requiere el paquete/lanzador especifico ya preparado, por REFramework.

Iniciar Launch-OptiScaler-NR.bat como administrador, dejando su ventana abierta.
El modo de desarrollador temporal se restaura al cerrar el juego.
Insert > Custom > activar reconstruccion neuronal > Modo neuronal.
320x180 / 512x288 / 640x360 / 960x540 / 1280x720. NR empieza desactivado.
Modelo completo y sincrono, historial temporal y EdgeGuidedResidual.
Modos 540p/720p son lentos y pueden superar el presupuesto VRAM durante el juego.
FSR4, XeSS, FSR3.1 y generacion de fotogramas segun soporte del juego/driver.
No copia DLL NVIDIA del juego, REFramework, SDK original ni ejecutables del juego.

Respaldo automatico y desinstalacion con el juego cerrado. Conserva ajustes y otros
mods; Spider-Man adapta Dxgi=false respaldando el INI anterior. Archivos modificados
despues se conservan y se notifican. Pesos compartidos por enlaces NTFS; no editar.
El instalador tiene su propio registro nr-general-installer, separado del paquete
antiguo para Requiem. Si ya hay instalacion general, desinstalar antes de reinstalar.

Paquete personalizado para uso local, no oficial. No se han validado derechos de
redistribucion publica de los pesos. Licencias incluidas corresponden al codigo
 y bibliotecas; no otorgan por si mismas derechos sobre los pesos originales.
'''
if bseries:
 readme=readme.replace('Intel Arc B580','Intel Arc B570 / B580').replace('SDR y Arc B580','SDR y Arc B570 / B580')
 readme+='\nB580: referencia probada. B570 (PCI 8086:E20C): soporte experimental; no hay prueba fisica ni tiempos medidos. Comenzar en 320x180. El driver debe exponer SM6.10 y las capacidades matriciales requeridas; se mantienen las protecciones de VRAM.\n'
generated('nr-docs/LEEME-General.txt',readme,'Instrucciones')
manifest=dict(version='1.1.0-general-bseries-local' if bseries else '1.0.0-general-local',scope='DX12 SDR Arc B580 tested / B570 hardware-unvalidated; other games experimental' if bseries else 'DX12 SDR Arc B580; Spider-Man reference; other games experimental',files=items)
(bank/'package-manifest.json').write_text(json.dumps(manifest,indent=2))
exe_manifest=stage/'app.manifest';exe_manifest.write_text('<?xml version="1.0" encoding="UTF-8"?><assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"><trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges><requestedExecutionLevel level="requireAdministrator" uiAccess="false"/></requestedPrivileges></security></trustInfo></assembly>')
csc=Path('C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe');refs=['System.Windows.Forms.dll','System.Drawing.dll','System.Web.Extensions.dll','System.IO.Compression.dll','System.IO.Compression.FileSystem.dll','System.Management.dll'];args=[str(csc),'/nologo','/optimize+','/platform:x64']+['/reference:'+r for r in refs]
for kind,name,admin in [('winexe','bootstrap.exe',True),('exe','installer-test.exe',False)]:
 subprocess.run(args+['/target:'+kind,'/out:'+str(bank/name)]+(['/win32manifest:'+str(exe_manifest)] if admin else [])+[str(root/'installer/GenericInstaller.cs')],check=True)
archive=bank/('payload-bseries.zip' if bseries else 'payload.zip')
if '--reuse-payload' in sys.argv:
 with zipfile.ZipFile(archive) as z:
  assert json.loads(z.read('package.json'))==manifest,'Cached payload differs; rebuild without --reuse-payload'
else:
 with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=True) as z:
  z.writestr('package.json',json.dumps(manifest))
  for i,(digest,p) in enumerate(blobs.items()):
   z.write(p,'blobs/'+digest)
   if i%100==0:print('PACK_UNIQUE_BLOBS',i,'/',len(blobs),flush=True)
output=dist/('OptiScaler-NR-General-B570-B580-Setup.exe' if bseries else 'OptiScaler-NR-General-B580-Setup.exe');shutil.copyfile(bank/'bootstrap.exe',output);offset=output.stat().st_size
with output.open('ab') as out,archive.open('rb') as source:shutil.copyfileobj(source,out,1024*1024);out.write(b'NRSETUPGENV1PACK'+struct.pack('<qq',offset,archive.stat().st_size))
report=dict(installer=str(output),sha256=sha(output),dll_sha256=sha(dll),b570_hardware_validated=False,bytes=output.stat().st_size,files=len(items),unique_blobs=len(blobs),logical_bytes=sum(i['bytes'] for i in items),unique_bytes=sum(p.stat().st_size for p in blobs.values()),live_games_changed=False,public_distribution_validated=False)
(bank/'build-result.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
