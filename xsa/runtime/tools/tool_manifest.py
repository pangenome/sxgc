#!/usr/bin/env python3
"""SXI distribution manifest: SHA256  artifact # role (paths are logical)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
BINARIES = ('xsa', 'rpfbwt_endpoints', 'slim_dump', 'sxi_write',
            'sxi_text_audit', 'agc2flat', 'pfp++', 'rpfbwt')
ROLES = ('CLI, sweep and validation', 'endpoint conversion', 'slim aggregates',
         'container writer', 'source audit', 'AGC adapter',
         'patched native AGC parser', 'patched endpoint tap')


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def source_paths(root=ROOT):
    patterns = ('tools/build*.sh', 'tools/tool_manifest.py', 'tools/upstream.lock.json',
                'bit6/patches/*.patch',
                'bit6/*.hpp', 'bit6/*.cpp', 'bit6/sxi_*.py',
                'bit6/pfp_ds_vendor/**/*.hpp', 'xsa/src/*.rs', 'agc2flat/src/*.rs',
                'xsa/Cargo.*', 'agc2flat/Cargo.*')
    return sorted({p.relative_to(root).as_posix() for pattern in patterns
                   for p in root.glob(pattern) if p.is_file()})


def create(prefix, destination, root=ROOT):
    lines = ['# SXI toolset v1; generated once after a complete isolated build.',
             '# bin/ resolves in the prefix; repo/ resolves in the source checkout.',
             '# pin/ hashes the UTF-8 upstream commit plus one newline.']
    for name, role in zip(BINARIES, ROLES):
        lines.append(f'{sha256(prefix / name)}  bin/{name} # {role}')
    for name, pin in json.loads((root / 'tools/upstream.lock.json').read_text()).items():
        digest = hashlib.sha256((pin['commit'] + '\n').encode()).hexdigest()
        lines.append(f"{digest}  pin/{name} # upstream {pin['commit']} {pin['url']}")
    for name in source_paths(root):
        role = 'in-repo patch' if name.endswith('.patch') else 'build/runtime source'
        lines.append(f'{sha256(root / name)}  repo/{name} # {role}')
    destination.write_text('\n'.join(lines) + '\n')


def verify_install(manifest, prefix, overrides, raw, allow_drift):
    """Installed manifests are authenticated by the running Cargo-built binary."""
    actual_manifest = hashlib.sha256(raw).hexdigest()
    expected_manifest = os.environ.get('XSA_INSTALL_MANIFEST_SHA256')
    if actual_manifest != expected_manifest:
        raise ValueError('tool manifest verification failed: install manifest SHA256 mismatch')
    data = json.loads(raw)
    if data.get('format') != 'xsa-install-v1' or data.get('package') != {
            'name': 'xsa', 'version': os.environ.get('XSA_PACKAGE_VERSION')}:
        raise ValueError('tool manifest verification failed: Cargo package identity mismatch')
    required = (set(BINARIES) - {'xsa'}) | {'phi_inverse_heads'}
    if not required <= set(data['files']):
        raise ValueError('tool manifest verification failed: incomplete installed toolset')
    artifacts, errors = [], []
    for name, expected in data['files'].items():
        if Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('invalid installation manifest path: ' + name)
        path = Path((overrides or {}).get(name, prefix / name))
        try:
            actual = sha256(path)
            if actual != expected:
                errors.append(f'{manifest}: SHA256 mismatch: {name}; expected={expected}; actual={actual}; path={path}')
            if name in required and not os.access(path, os.X_OK):
                errors.append(f'missing executable: {path}')
            artifacts.append(dict(artifact=('bin/' if '/' not in name else 'repo/') + name,
                                  path=str(path.resolve()), expected_sha256=expected,
                                  sha256=actual, role='Cargo-bundled stage/runtime'))
        except OSError as e:
            kind = 'missing executable' if name in required else 'missing/unreadable installed artifact'
            errors.append(f'{kind}: {path}; {e}')
    # Self-hashing cannot be embedded in the executable. Record its observed
    # digest separately from the build-bound native-stage hashes.
    xsa = Path((overrides or {}).get('xsa', prefix / 'xsa'))
    actual_xsa = sha256(xsa)
    artifacts.append(dict(artifact='bin/xsa', path=str(xsa.resolve()),
                          sha256=actual_xsa, role='executing Cargo package (observed self hash)'))
    if errors and not allow_drift:
        raise ValueError('tool manifest verification failed:\n' + '\n'.join(errors))
    return dict(stage='tool-provenance', manifest_path=str(manifest.resolve()),
                manifest_sha256=actual_manifest, manifest=raw.decode(),
                package=data['package'], source_sha256=data['source_sha256'],
                upstreams=data['upstreams'], artifacts=artifacts,
                allow_drift=allow_drift, verified=not errors, drift=errors)


def verify(manifest, prefix, overrides=None, allow_drift=False, root=ROOT):
    """Return full provenance; reject incomplete, malformed or mismatching sets."""
    raw = manifest.read_bytes()
    if raw.lstrip().startswith(b'{'):
        return verify_install(manifest, prefix, overrides, raw, allow_drift)
    text = raw.decode('utf-8')
    pins = json.loads((root / 'tools/upstream.lock.json').read_text())
    expected = {'bin/' + n for n in BINARIES} | {'pin/' + n for n in pins}
    expected |= {'repo/' + n for n in source_paths(root)}
    seen, artifacts, errors = set(), [], []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        match = re.fullmatch(r'([0-9a-f]{64})  (\S+) # (.+)', line)
        if not match:
            errors.append(f'{manifest}:{number}: malformed manifest line: {line}')
            continue
        digest, name, role = match.groups()
        if name in seen or name not in expected:
            errors.append(f'{manifest}:{number}: duplicate/unknown artifact: {line}')
            continue
        seen.add(name)
        path = None
        try:
            if name.startswith('pin/'):
                actual = hashlib.sha256((pins[name[4:]]['commit'] + '\n').encode()).hexdigest()
            else:
                path = (Path((overrides or {}).get(name[4:], prefix / name[4:]))
                        if name.startswith('bin/') else root / name[5:])
                actual = sha256(path)
                if name.startswith('bin/') and not os.access(path, os.X_OK):
                    errors.append(f'{manifest}:{number}: missing executable: {path}; line: {line}')
            artifacts.append(dict(artifact=name, path=str(path.resolve()) if path else None,
                                  expected_sha256=digest, sha256=actual, role=role))
            if actual != digest:
                errors.append(f'{manifest}:{number}: SHA256 mismatch: {line}; actual={actual}; path={path}')
        except OSError as error:
            kind = 'missing executable' if name.startswith('bin/') else 'missing/unreadable artifact'
            errors.append(f'{manifest}:{number}: {kind}: {line}; {error}')
    for name in sorted(expected - seen):
        errors.append(f'{manifest}: missing manifest line for {name}')
    if errors and not allow_drift:
        raise ValueError('tool manifest verification failed:\n' + '\n'.join(errors))
    return dict(stage='tool-provenance', manifest_path=str(manifest.resolve()),
                manifest_sha256=hashlib.sha256(raw).hexdigest(), manifest=text,
                artifacts=artifacts, allow_drift=allow_drift,
                verified=not errors, drift=errors)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['create', 'verify'])
    p.add_argument('--prefix', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    a = p.parse_args()
    try:
        if a.action == 'create':
            create(a.prefix, a.manifest)
        else:
            result = verify(a.manifest, a.prefix)
            print(f"PASS manifest verified: {len(result['artifacts'])} artifacts; {result['manifest_sha256']}")
    except (OSError, ValueError) as error:
        p.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
