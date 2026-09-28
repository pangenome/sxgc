"""Bounded-memory FASTA/FASTQ extraction into the pinned collection format.

Sequences retain case and orientation, like --text. Names use agc2flat's
revlines sidecar coordinates: total - 1 - stream_offset - sequence_length.
"""
import re
import tempfile

SEPARATOR = b'\x1e'
CONTRACT = 'corpus contract: reserved separator 0x1E must never appear in sequence content'
CHUNK = 1 << 20
SEQUENCE = re.compile(rb'[A-Za-z.*-]+\Z')
QUALITY = re.compile(rb'[!-~]+\Z')


def line_chunks(source):
    """Yield a physical line in bounded chunks, removing only LF or CRLF."""
    pending = b''
    while True:
        part = source.readline(CHUNK)
        if not part:
            if pending:
                yield pending
            return
        part = pending + part
        if part.endswith(b'\n'):
            part = part[:-1]
            if part.endswith(b'\r'):
                part = part[:-1]
            if part:
                yield part
            return
        # Hold one byte so CRLF split at a chunk boundary is handled correctly.
        pending = part[-1:]
        if len(part) > 1:
            yield part[:-1]


def header(source, marker, context, allow_empty=False):
    parts, size = [], 0
    for part in line_chunks(source):
        size += len(part)
        if size > CHUNK:
            raise RuntimeError(f'{context}: header exceeds 1 MiB')
        parts.append(part)
    value = b''.join(parts)
    if not value.startswith(marker):
        raise RuntimeError(f'{context}: expected {marker.decode()} header')
    value = value[1:]
    try:
        value.decode('utf-8')
    except UnicodeDecodeError as error:
        raise RuntimeError(f'{context}: header must be UTF-8') from error
    if any(c < 32 and c != 9 or c == 127 for c in value):
        raise RuntimeError(f'{context}: control character in header')
    if not allow_empty and (not value or value[:1].isspace()):
        raise RuntimeError(f'{context}: missing record identifier')
    return value


def copy_line(source, destination, pattern, context):
    size = 0
    for part in line_chunks(source):
        if destination is not None and SEPARATOR in part:
            raise RuntimeError(f'{context}: {CONTRACT}')
        if not pattern.fullmatch(part):
            raise RuntimeError(f'{context}: invalid characters')
        size += len(part)
        if destination is not None:
            destination.write(part)
    if not size:
        raise RuntimeError(f'{context}: empty or missing line')
    return size


def prepare(kind, source_path, text_path, names_path, progress):
    """Extract without retaining sequences or the collection's names in RAM."""
    count, total = 0, 0
    with source_path.open('rb') as source, text_path.open('wb') as text, \
            tempfile.TemporaryFile(dir=text_path.parent) as rows:
        name, length = None, 0

        def finish():
            nonlocal count, total
            if not length:
                raise RuntimeError(f'{kind} record {count + 1}: empty sequence')
            text.write(SEPARATOR)
            rows.write(name + f'\t{total}\t{length}\n'.encode())
            total += length + 1
            count += 1
            progress(dict(record=count, name=name.decode('utf-8'),
                          sequence_bytes=length, collection_bytes=total))

        while source.peek(1):
            context = f'{kind} record {count + 1}'
            if kind == 'fasta':
                if source.peek(1).startswith(b'>'):
                    if name is not None:
                        finish()
                    value = header(source, b'>', f'fasta record {count + 1}')
                    name, length = value.split()[0], 0
                else:
                    if name is None:
                        raise RuntimeError(f'{context}: sequence before first > header')
                    length += copy_line(source, text, SEQUENCE, context + ' sequence')
            elif kind == 'fastq':
                value = header(source, b'@', context)
                name = value.split()[0]
                length = copy_line(source, text, SEQUENCE, context + ' sequence')
                repeated = header(source, b'+', context, allow_empty=True)
                if repeated and repeated not in (value, name):
                    raise RuntimeError(f'{context}: + identifier does not match @ header')
                quality_length = copy_line(source, None, QUALITY, context + ' quality')
                if quality_length != length:
                    raise RuntimeError(f'{context}: sequence/quality length mismatch')
                finish()
            else:
                raise ValueError('unsupported sequence format: ' + kind)
        if kind == 'fasta' and name is not None:
            finish()
        if not count:
            raise RuntimeError(f'{kind}: no records')
        rows.seek(0)
        with names_path.open('wb') as names:
            for row in rows:
                name, offset, length = row.rstrip(b'\n').split(b'\t')
                start = total - 1 - int(offset) - int(length)
                names.write(name + f'\t{start}\t'.encode() + length + b'\n')
    return count, total
