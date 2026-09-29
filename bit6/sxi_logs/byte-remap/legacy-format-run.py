"""Run the unchanged legacy differential, redirecting only its log directory."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[3]
bundle=next((root/'xsa/target/release/build').glob('xsa-*/out/stages-*/bundle'))
p=root/'bit6/sxi_gate.py'
source=p.read_text().replace("logs=root/'bit6/sxi_logs'", "logs=root/'bit6/sxi_logs/byte-remap/legacy-format'")
sys.argv=[str(p),'--work',str(root/'vendor/byte-remap-gates/legacy-format'),'--writer',str(bundle/'sxi_write'),'--dump',str(bundle/'slim_dump')]
exec(compile(source,str(p),'exec'),{'__file__':str(p),'__name__':'__main__'})
