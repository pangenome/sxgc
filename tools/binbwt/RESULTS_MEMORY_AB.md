# chi costs 0.11 n RAM, not 1.1 n (measured A/B, 10 GB corpus, main=104ea3e)

Two builds, identical flags (--threads 96 --memory-gb 96), only chi on/off. Per-phase peak RSS from
each process's own /usr/bin/time -v record.

| phase | chi off | chi on | delta |
|---|---:|---:|---:|
| order-tape (sorter) | 20,030 MB | 20,116 MB | +86 MB (noise) |
| emit-tape (merge)   |  1,510 MB |  1,519 MB |  +9 MB |
| finish-v2           |  3,351 MB |  4,469 MB | **+1,118 MB** |
| container bytes     | 5,710,290,472 | 7,004,523,984 | +1.29 GB |

chi = 2,676,929,445 in the on arm (exact). So chi's RAM term is ~0.11 n and its published size ~0.13 n.

Pile projection: chi RAM ~145 GB, chi disc ~170 GB, finish total ~0.45 n (~590 GB) - all inside a
450 GB / 770 GB-free box. **The run-stream chi rewrite is NOT required**; the earlier 0.93 n/11 GB
figures were sampler artefacts, not the bitmap.
