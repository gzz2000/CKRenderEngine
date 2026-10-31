#!/usr/bin/env python3
"""Generate WebGL2 FFP from the same checked SPIR-V as native/WebGPU.

Requires spirv-cross (qualified 1.4.363.0) and the SPIR-V JSON grammar.
Use desktop GLSL 330 as an intermediate: ES lacks textureQueryLevels, so
replace that operation with the texture's actual allocation level count,
provided by the renderer. No shading equations are maintained separately.
"""
import argparse, hashlib, json, re, subprocess, sys, tempfile
from pathlib import Path
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent.parent / 'CKSdlGpuRasterizer/shaders'
sys.path.insert(0, str(NATIVE))
from shader_pack import decode
from lower_lod import lower


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--spirv-cross', default='spirv-cross')
    p.add_argument('--spirv-grammar', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    manifest = json.loads((NATIVE / 'generated/manifest.json').read_text())
    entries = [s for s in manifest['shaders'] if s['format'] == 'spirv']
    packed = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', (NATIVE / 'generated/spirv_pack.h').read_text()))
    artifacts = decode(packed)
    assert len(entries) == len(artifacts)
    names = {'vs_ff_3d', 'vs_ff_positiont', 'fs_ff_stage', 'fs_ff_stage_cube', 'fs_ff_stage_volume'}
    grammar = json.loads(a.spirv_grammar.read_text())
    shaders = {}
    with tempfile.TemporaryDirectory(prefix='ballance-glsl-') as tmp:
        for entry, binary in zip(entries, artifacts):
            name = entry['name']
            if name not in names: continue
            assert hashlib.sha256(binary).hexdigest() == entry['sha256']
            spv = Path(tmp) / 'shader.spv'
            spv.write_bytes(lower(binary, grammar)[0])
            source = subprocess.check_output([a.spirv_cross, str(spv), '--version', '330', '--fixup-clipspace'], text=True)
            source = source.replace('#version 330', '#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp samplerCube;\nprecision highp sampler3D;')
            slots = {name: int(slot) for slot, name in re.findall(r'layout\(binding = (\d+)\) uniform sampler\w+ (\w+);', source)}
            source = re.sub(r'^#extension GL_ARB_.*\n', '', source, flags=re.M)
            source = re.sub(r'layout\(binding = \d+, std140\)', 'layout(std140)', source)
            source = re.sub(r'layout\(binding = \d+\) ', '', source)
            # ES 3.00 links varyings by name, not SPIR-V location.
            source = source.replace('out_var_TEXCOORD' if name.startswith('vs_') else 'in_var_TEXCOORD', 'ff_varying')
            if 'textureQueryLevels' in source:
                source = source.replace('void main()', 'uniform int ckMipLevels[16];\n\nvoid main()')
                source = re.sub(r'textureQueryLevels\((\w+)\)', lambda m: f'ckMipLevels[{slots[m[1]]}]', source)
            for sampler, slot in slots.items():
                source = re.sub(r'\b' + sampler + r'\b', 'ff_texture' + str(slot), source)
            if name.startswith('vs_'):
                source = source.replace('void main()\n{', 'void main()\n{\n    gl_PointSize = 1.0;')
            shaders[name] = source
    assert set(shaders) == names
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(shaders, separators=(',', ':')) + '\n')
    print(f'Generated {len(shaders)} WebGL2 shaders: {a.output}')


if __name__ == '__main__': main()
