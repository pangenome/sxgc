"""Bound a DIFFERENT 16-word index on the measured lexical literals. No tape or dictionary reads."""
import json
from pathlib import Path
from overlap_vocab_review import review as vocab

BANK=Path(__file__).resolve().parent/'receipts/vocab-10g'

def wire(k,lit,max_word,max_block):
    if not all(type(x) is int and x>0 for x in (k,lit,max_word,max_block)) or max_word>1<<20:raise ValueError('invalid word/block bound')
    q=(k+15)//16;limit=min(16*max_word,max_block,lit);fields=2*k-q
    if not q<=lit<=min(k*max_word,q*limit):raise ValueError('inconsistent literal bound')
    # All fields (p except at restart, e including it) use >=1 byte. Indexed words
    # use strictly increasing literal addresses in their block, hence p <= total E.
    base=16+8*(q+1)+q+lit+fields
    excess={}
    for t in (256,65536,16777216):
        b=min(q,lit//t) if limit>=t else 0
        excess[t]=min(k,b*16)+(min(k-q,b*15) if max_word>=t else 0)
    return dict(blocks=q,literals=lit,base_min_bytes=base,extra_field_bytes_upper_by_threshold={str(k):v for k,v in excess.items()},wire_upper=base+sum(excess.values()))

def ten():
    v=vocab(BANK);m=json.loads((BANK/'model.json').read_bytes());u=m['lexical_codec_and_navigation']['lexical_blocks']['16'];k=m['k'];lit=u['literal_bytes'];longest=json.loads((BANK/'front.json').read_bytes())['summary']['max_phrase'];maxblock=u['max_block_bytes'];old=v['codec']['16']['exact_wire_bytes_this_tape'];a=wire(k,lit,longest,maxblock);nav=u['tenfold_sensitivity_only'];b=wire(10*k,10*lit,longest,maxblock)
    return dict(scope='bounds from portable 10 GB witnesses only. No direct table built at that size, no product grant or 100 GB timing.',measured_10g_uleb_wire=old,measured_10g_packed=m['packed_bytes'],
        direct_on_measured_10g_dimensions=a,direct_upper_over_uleb=a['wire_upper']/old,
        hypothetical_ten_times_k_and_literals_same_maxima=dict(direct_wire_upper=b['wire_upper'],pre_order_upper=nav['emit_before_order']-nav['lexical_payload_and_directory']+b['wire_upper'],
            room_before_order_in_hypothetical_140gib=(140<<30)-nav['emit_before_order']+nav['lexical_payload_and_directory']-b['wire_upper']))
if __name__=='__main__':print(json.dumps(ten(),indent=2,sort_keys=True))
