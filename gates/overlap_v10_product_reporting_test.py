"""Regress the two reporting failures without starting a sorter or touching any index."""
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from overlap_v10_product_compare import Arm, optional_io_stat

class Worker:
    pid=334455
    returncode=None
    def poll(self):return self.returncode

def main():
    with TemporaryDirectory() as td:
        d=Path(td);assert optional_io_stat(d) is None
        (d/'io.stat').touch();assert optional_io_stat(d) is None
        (d/'io.stat').write_text('7:0 rbytes=512 wbytes=0\n');assert optional_io_stat(d)=='7:0 rbytes=512 wbytes=0'
        a=Arm.__new__(Arm);w=Worker();a.root=d;a.work={'child':w};a.excluded=set();a.stop=d/'STOP';a.peak=a.alloc=a.fd_retries=0
        # No substitution of an empty fd set for a denied live child. Once
        # it is reaped it cannot hold any scratch, but its exit is still scored.
        with patch.object(Path,'iterdir',side_effect=PermissionError('exec transition')),patch('time.sleep'):
            try:a.measure()
            except PermissionError:pass
            else:raise AssertionError('silently skipped a live fd table')
        assert a.fd_retries==12
        def reap():w.returncode=0;raise PermissionError('exiting worker')
        with patch.object(Path,'iterdir',side_effect=reap):assert a.measure()==(d/'io.stat').stat().st_size and w.returncode==0
        assert a.fd_retries==13
    print('PASS absent/empty/available optional io.stat; persistent live permission failure refuses, actual reaped worker accepted')

if __name__=='__main__':main()
