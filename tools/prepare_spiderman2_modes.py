"""Create geometry-only B1 packages from validated immutable native weights."""
from pathlib import Path
import json, os, sys, hashlib
import numpy as np
root=Path(__file__).resolve().parents[1]
os.environ['NR_ROOT']=str(root)
sys.path.insert(0,str(root/'work/upstream-baseline/src/ref'))
import nr_frame
source=Path(r"<GAME_LIBRARY>\steamapps\common\Marvel's Spider-Man 2\native_nr_compact512")
bank=root/'work/b580/spiderman2-modes'
bank.mkdir(parents=True,exist_ok=True)
manifest=(source/'graph.txt').read_text()
reports=[]
for width,height in [(320,180),(512,288),(640,360),(960,540),(1280,720)]:
    fw=(width+63)//64*64;fh=(height+63)//64*64
    target=bank/f'assets-{height}'
    if target.exists():raise RuntimeError('Geometry package already exists; preserve it before rebuilding')
    target.mkdir();shapes={};skips={};h,w,c=fh,fw,16
    # Weight files are immutable shared hardlinks; never write to these paths.
    for row in manifest.splitlines():
        kind,name,retain,skip=row.split();out=target/name;out.mkdir()
        if kind!='bottleneck':
            original=list(map(int,(source/name/'shape.txt').read_text().split()))
            if kind=='window':
                assert c==original[8]
                s=original.copy();s[0:2]=[h,w];s[4:6]=[(h+s[2]+7)//8*8,(w+s[3]+7)//8*8]
            elif kind=='transition':
                k,_,_,ic,oc,_,_,pad,publish=original;assert c==ic
                th,tw=((h+pad-1)//pad*pad//2,(w+pad-1)//pad*pad//2) if k!=2 and pad else (h//2,w//2)
                if k==2:th,tw,sc=skips[skip];assert sc==oc
                s=[k,h,w,c,oc,th,tw,pad,publish];h,w,c=th,tw,oc
            else:
                k=original[0];th,tw=h,w
                if k==3:th,tw,sc=skips[skip];assert sc==32
                s=[k,h,w,th,tw];h,w,c=th,tw,32 if k in (1,3) else 4
            (out/'shape.txt').write_text(' '.join(map(str,s)))
            shapes[name]=s
        else:
            assert c==1024 and h*w<=640
            tokens=h*w;token_grid=[w,h]
        if int(retain):skips['skip_'+name]=(h,w,c)
        for f in (source/name).rglob('*.bin'):
            if f.name in ('input.bin','skip.bin'):continue
            dst=out/f.relative_to(source/name);dst.parent.mkdir(parents=True,exist_ok=True)
            os.link(f,dst)
    assert (h,w,c)==(fh,fw,4)
    for sub,shape in [('frame_input',[height,width,fh,fw]),('frame_output',[fh,fw,height,width])]:
        (target/sub).mkdir();(target/sub/'shape.txt').write_text(' '.join(map(str,shape)))
    reference=Path(r'<GAME_LIBRARY>\steamapps\common\RESIDENT EVIL requiem BIOHAZARD requiem\native_nr_global360')
    noise=np.fromfile(reference/'frame_input/noise.bin',np.float32).reshape(384,640,3)
    xs=np.floor((np.arange(fw)+.5)*640/width).astype(int);ys=np.floor((np.arange(fh)+.5)*360/height).astype(int)
    xs=np.where(xs<640,xs,1278-xs);ys=np.where(ys<384,ys,766-ys)
    assert xs.min()>=0 and xs.max()<640 and ys.min()>=0 and ys.max()<384
    noise[ys[:,None],xs[None,:],:].astype('<f4').tofile(target/'frame_input/noise.bin')
    (target/'graph.txt').write_text(manifest)
    report={'width':width,'height':height,'network_width':fw,'network_height':fh,'tokens':tokens,'token_grid':token_grid,
        'alignment_policy':'Experimental ceil-to64, same native graph rules; not a claim of vendor-exact extent selection',
        'noise_policy':'logical viewport nearest sample of fixed360 noise, reflected padded tail, same policy as accepted512', 'noise_frame_index':0,'temporal_history':'current frame (static fixture)','weights':'immutable hardlinks to accepted native assets',
        'shapes':shapes,'retained_skips':skips}
    (target/'geometry.json').write_text(json.dumps(report,indent=2));reports.append(report)
    for shader in (source/'shaders').rglob('*'):
        if shader.is_file():
            dst=target/'shaders'/shader.relative_to(source/'shaders');dst.parent.mkdir(parents=True,exist_ok=True);os.link(shader,dst)
    entries=[]
    for f in sorted(target.rglob('*')):
        if f.is_file() and f.name not in ('assets.txt','geometry.json'):
            entries.append(f'{f.stat().st_size} {hashlib.sha256(f.read_bytes()).hexdigest()} {f.relative_to(target).as_posix()}')
    (target/'assets.txt').write_text('NR_NATIVE_ASSETS 1\ngeometry '+f'{height} {width} {fh} {fw}\nfiles {len(entries)}\n'+'\n'.join(entries)+'\n')

(bank/'geometry-preflight.json').write_text(json.dumps({'passed':True,'cases':reports},indent=2))
print(json.dumps([{'height':r['height'],'network':[r['network_width'],r['network_height']],'tokens':r['tokens']} for r in reports]))

