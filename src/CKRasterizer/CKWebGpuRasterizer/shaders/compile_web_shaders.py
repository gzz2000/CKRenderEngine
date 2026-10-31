#!/usr/bin/env python3
"""Translate the checked native FFP artifacts, preserving their resource ABI.

Requires Tint built with TINT_BUILD_SPV_READER=ON. The ordinary macOS Dawn
binary release does not enable that reader. Qualified source revision:
97ae1b10ac9d579f7e6b790ce7fc4faa1041f820 (Dawn v20261030.184100).
Compilation is only the first gate: run the browser shader/pixel tests too.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent.parent / 'CKSdlGpuRasterizer' / 'shaders'
sys.path.insert(0, str(NATIVE))
from shader_pack import decode
from lower_lod import lower


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tint', required=True, type=Path)
    parser.add_argument('--spirv-grammar', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    tint = args.tint.resolve()
    manifest = json.loads((NATIVE / 'generated/manifest.json').read_text())
    entries = [s for s in manifest['shaders'] if s['format'] == 'spirv']
    header = (NATIVE / 'generated/spirv_pack.h').read_text()
    packed = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', header))
    artifacts = decode(packed)
    if len(entries) != len(artifacts):
        raise ValueError('SPIR-V manifest/pack length mismatch')
    grammar = json.loads(args.spirv_grammar.read_text())
    result = {'abi_version': manifest['abi_version'],
              'interface_hash': manifest['interface_hash'],
              'tint_sha256': sha(tint.read_bytes()),
              'lowering_sha256': sha(HERE.joinpath('lower_lod.py').read_bytes()),
              'shaders': []}
    shaders = {}
    with tempfile.TemporaryDirectory(prefix='ballance-wgsl-') as directory:
        work = Path(directory)
        for entry, code in zip(entries, artifacts):
            if sha(code) != entry['sha256']:
                raise ValueError(f"SPIR-V hash mismatch: {entry['name']}")
            lowered, queries = lower(code, grammar)
            spv = work / f"{entry['name']}.spv"
            wgsl = spv.with_suffix('.wgsl')
            spv.write_bytes(lowered)
            subprocess.run([str(tint), str(spv), '--format', 'wgsl',
                            '--allow-non-uniform-derivatives', 'true', '-o', str(wgsl)], check=True)
            # FFP branches are runtime state-dependent, as in the native shader.
            # Tint's diagnostic directive permits that existing derivative use;
            # it does not bypass syntax/resource/pipeline validation.
            text = wgsl.read_text()
            if not text.strip():
                raise ValueError(f"Empty WGSL: {entry['name']}")
            shaders[entry['name']] = text
            result['shaders'].append({'name': entry['name'], 'spirv_sha256': entry['sha256'],
                                      'wgsl_sha256': sha(text.encode()), 'lowered_lod_queries': queries})
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'shaders.json').write_text(json.dumps(shaders, separators=(',', ':')) + '\n')
    (args.output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"Converted {len(shaders)} shader modules; browser validation still required")


if __name__ == '__main__':
    main()
