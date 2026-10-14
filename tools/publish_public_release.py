"""Publish weight-free installers with existing authorized GitHub credentials.

Never prints credentials or requests them through chat. Uses GH_TOKEN/GITHUB_TOKEN
or Git Credential Manager non-interactively; exits if neither is authorized.
"""
from pathlib import Path
import argparse,json,subprocess,os,urllib.request,urllib.error,http.client
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--check-auth',action='store_true');args=ap.parse_args()
token=os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN')
if not token:
 env=dict(os.environ,GIT_TERMINAL_PROMPT='0',GCM_INTERACTIVE='Never')
 p=subprocess.run(['git','-c','credential.interactive=never','credential','fill'],input='protocol=https\nhost=github.com\n\n',capture_output=True,text=True,env=env,timeout=30)
 if p.returncode==0:token=dict(line.split('=',1) for line in p.stdout.splitlines() if '=' in line).get('password')
if not token:raise SystemExit('AUTH_REQUIRED: GitHub connector does not expose release-upload credentials. Authorize Git Credential Manager or GitHub CLI locally; do not paste tokens in chat.')
headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json','User-Agent':'OptiScaler-NR-public-release','X-GitHub-Api-Version':'2022-11-28'}
def api(path,data=None):
 req=urllib.request.Request('https://api.github.com'+path,data=json.dumps(data).encode() if data is not None else None,headers=headers)
 with urllib.request.urlopen(req,timeout=60) as response:return json.load(response)
profile=api('/user');assert profile['login'].lower()=='byroncalle23','Authenticated GitHub account differs from the authorized repository owner'
if args.check_auth:print('GITHUB_RELEASE_AUTH_READY byroncalle23');raise SystemExit(0)
builds=json.loads((root/'work/b580/public-installers/build-result.json').read_text())
assert len(builds)==2 and all(not x['weights_included'] and not x['experimental_sdk_included'] for x in builds)
repo='/repos/byroncalle23/optiscaler-nr-intel-arc';tag='v1.2.0-public-no-weights'
try:release=api(repo+'/releases/tags/'+tag)
except urllib.error.HTTPError as e:
 if e.code!=404:raise
 release=api(repo+'/releases',dict(tag_name=tag,target_commitish='main',name='OptiScaler NR · instaladores públicos sin pesos',draft=False,prerelease=True,body='Dos instaladores: B580 y B570/B580 experimental. Sin pesos ni SDK experimental Microsoft 721. OptiScaler base puede usarse con NR apagado; NR requiere modelo/runtime aportados localmente y registrados con el instalador. Leer docs/INSTALADORES-PUBLICOS.md. No hay compatibilidad universal ni promesa de FPS.'))
for p in [root/'dist'/x['file'] for x in builds]+[root/'dist/PUBLIC-INSTALLERS-SHA256.txt']:
 old=next((a for a in release.get('assets',[]) if a['name']==p.name),None)
 if old:raise RuntimeError('Release asset already exists; verify it before deciding to replace: '+p.name)
 url=release['upload_url'].split('{',1)[0]+'?name='+urllib.parse.quote(p.name)
 parsed=urllib.parse.urlsplit(url);assert parsed.hostname=='uploads.github.com'
 conn=http.client.HTTPSConnection(parsed.hostname,timeout=300)
 upload_headers=dict(headers,**{'Content-Type':'application/octet-stream','Content-Length':str(p.stat().st_size)})
 with p.open('rb') as body:conn.request('POST',parsed.path+'?'+parsed.query,body=body,headers=upload_headers);response=conn.getresponse();result=json.loads(response.read())
 assert response.status==201,(response.status,result.get('message'));print('UPLOADED',result['browser_download_url'],flush=True);conn.close()
print('PUBLIC_RELEASE_READY',release['html_url'])
