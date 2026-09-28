"""Own one ephemeral FUSE mount and its parse process group; never write text."""
import os
import pathlib
import signal
import subprocess
import time


def ghost_parse(mount, producer, consumer, env, logs, label, record, verbose=False):
    mount.mkdir(mode=0o700)
    children = []
    handles = []
    old_term = signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    size = None
    try:
        def start(stage, command):
            log = logs / (label + '.' + stage + '.log')
            timing = logs / (label + '.' + stage + '.time')
            handle = log.open('wb'); handles.append(handle)
            command = list(map(str, command))
            record(dict(stage=stage, command=command, work=str(mount.parent), start=time.time()))
            proc = subprocess.Popen(['/usr/bin/time', '-v', '-o', str(timing), *command],
                                    env=env, stdout=handle, stderr=handle, start_new_session=True)
            children.append((stage, proc, time.monotonic(), log, timing))
            return proc
        source = start('ghost', producer)
        text = mount / 'collection'
        while not text.exists():
            if source.poll() is not None:
                raise RuntimeError(f'ghost mount failed ({source.returncode}); see {children[0][3]}')
            time.sleep(.05)
        size = text.stat().st_size
        reader = start('parse', consumer)
        while reader.poll() is None:
            if source.poll() is not None:
                raise RuntimeError('ghost service exited during parse')
            time.sleep(.05)
        if reader.returncode:
            raise RuntimeError(f'parse failed ({reader.returncode}); see {children[1][3]}')
    finally:
        # Stop/reap the consumer before unmounting. Only our own sessions receive signals.
        for _, proc, _, _, _ in children[1:]:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL); proc.wait()
        if os.path.ismount(mount):
            subprocess.run(['fusermount3', '-u', str(mount)], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if os.path.ismount(mount):
                subprocess.run(['fusermount3', '-uz', str(mount)], check=True)
        for _, proc, _, _, _ in children:
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGTERM)
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL); proc.wait()
        for handle in handles: handle.close()
        signal.signal(signal.SIGTERM, old_term)
        mount.rmdir()
        for stage, proc, begin, log, timing in children:
            record(dict(stage=stage, returncode=proc.returncode,
                        wall_seconds=time.monotonic()-begin, log=str(log), timing=str(timing)))
    return size
