"""Package resident-model assets, excluding captured input/output/reference frames."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--fixture', type=Path, default=root/'work/b580/graph/gradient')
parser.add_argument('--output', type=Path, default=root/'work/b580/native_nr_assets')
args = parser.parse_args()
source = args.fixture.resolve(strict=True)
output = args.output.resolve()
if output == source or source in output.parents or output in source.parents:
    parser.error('Package and fixture directories must be separate.')
if output.exists():
    parser.error('Output already exists; choose a new package directory.')
files = {'graph.txt', 'frame_input/shape.txt', 'frame_input/noise.bin', 'frame_output/shape.txt'}
nodes = source.joinpath('graph.txt').read_text(encoding='ascii').splitlines()
if len(nodes) != 77:
    raise ValueError('Expected the complete 77-node graph')
for row in nodes:
    kind, name, retain, skip = row.split()
    if not re.fullmatch('[a-z0-9_]+', name) or kind not in {'endpoint','window','transition','bottleneck'}:
        raise ValueError('Invalid graph node')
    folder = source / name
    if kind != 'bottleneck':
        files.add(f'{name}/shape.txt')
    for path in folder.rglob('*.bin'):
        if path.stem in {'input','skip'} or path.stem.startswith(('native','reference')):
            continue
        relative = path.relative_to(source).as_posix()
        if not re.fullmatch(r'[a-z0-9_/]+\.bin', relative):
            raise ValueError(f'Invalid asset path: {relative}')
        files.add(relative)
geometry = source.joinpath('frame_input/shape.txt').read_text(encoding='ascii').split()
if len(geometry) != 4:
    raise ValueError('Invalid package geometry')
for name in files:
    if not source.joinpath(name).is_file():
        raise FileNotFoundError(name)
output.mkdir(parents=True)
entries = []
for name in sorted(files):
    target = output / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / name, target)
    data = target.read_bytes()
    entries.append((len(data), hashlib.sha256(data).hexdigest(), name))
manifest = ['NR_NATIVE_ASSETS 1', 'geometry '+' '.join(geometry), f'files {len(entries)}']
manifest += [f'{size} {digest} {name}' for size,digest,name in entries]
output.joinpath('assets.txt').write_text('\n'.join(manifest)+'\n', encoding='ascii')
print(json.dumps({'package':str(output),'files':len(entries), 'bytes':sum(e[0] for e in entries),
                  'geometry':list(map(int,geometry)), 'dtype':'float32',
                  'shaders':'embedded in binary; compiled during initialization',
                  'limitations':'Static SDR fixture geometry/noise. Not ONNX or INT8.'}))
