#!/usr/bin/env python3
"""The optional cleaned corpus must have exactly one terminal; never overwrite."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

tool=Path(__file__).resolve().parents[1]/'tools/prepare_external_measurement.py'
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp);source=root/'source';source.write_bytes(bytes(range(256)))
    cmd=[sys.executable,str(tool),'--source',str(source),'--cleaned',str(root/'clean'),
         '--raw-copy',str(root/'raw'),'--manifest',str(root/'manifest')]
    subprocess.run(cmd,check=True)
    cleaned=(root/'clean').read_bytes();manifest=json.loads((root/'manifest').read_text())
    assert (root/'raw').read_bytes()==source.read_bytes()
    assert len(cleaned)==257 and cleaned[-1]==30 and cleaned.count(30)==1
    assert all(6<=c<128 for c in cleaned)
    assert manifest['low_bytes_changed']==6 and manifest['high_bytes_changed']==128
    assert manifest['reserved_terminal_bytes_changed']==1
    assert manifest['cleaned_sha256']==hashlib.sha256(cleaned).hexdigest()
    refused=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    assert refused.returncode and b'refusing existing output' in refused.stdout
    assert (root/'clean').read_bytes()==cleaned
print('EXTERNAL_MEASUREMENT_PASS terminal_unique=1 all_bytes=256 no_clobber=1')
