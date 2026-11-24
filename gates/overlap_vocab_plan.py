"""Arithmetic on measured complete-word census cells; never opens corpus or starts a producer.
Each non-END row belongs to one 2-symbol lexical prefix of its finite tail.
Distinct suffix *descriptors*, text rows and equal-key cursors have different costs.
"""
import hashlib
import struct

CELLS=256*257

def counts(sparse):
    x=[0]*CELLS;last=-1
    for i,n in sparse:
        if not(type(i) is type(n) is int and last<i<CELLS and 0<n<2**64):raise ValueError('invalid/noncanonical sparse counts')
        last=i;x[i]=n
    return x

def digest(x):return hashlib.sha256(struct.pack('<'+str(CELLS)+'Q',*x)).hexdigest()

def greedy(cells,cap,detail=False):
    """Minimum number of contiguous unsplit-cell segments at a descriptor cap.
    Moving the first cut earlier cannot put fewer cells in the remaining segments.
    Iteration proves greedy optimal for this fixed cell family, not all finite keys.
    """
    ranges=[];first=last=None;d=r=peak=nr=0
    def close():
        nonlocal peak,nr
        peak=max(peak,r);nr+=1
        if detail:ranges.append([first,last,d,r])
    for i,dc,rc in cells:
        if dc>cap:return None
        if d and d+dc>cap:close();d=r=0
        if not d:first=i
        last=i;d+=dc;r+=rc
    if d:close()
    return dict(range_count=nr,max_text_rows=peak,**({'ranges_first_last_descriptors_rows':ranges} if detail else {}))

def minimum(cells,max_ranges):
    if not cells or max_ranges<1:raise ValueError('empty/cap')
    lo=max(max(d for _,d,_ in cells),(sum(d for _,d,_ in cells)+max_ranges-1)//max_ranges);hi=sum(d for _,d,_ in cells)
    while lo<hi:
        mid=(lo+hi)//2;v=greedy(cells,mid)
        if v is not None and v['range_count']<=max_ranges:hi=mid
        else:lo=mid+1
    return dict(descriptor_slots=lo,payload_bytes=8*lo,**greedy(cells,lo))

def ef_capacity(n,maximum):
    """Requested block-64 payload capacity of the fixture Monotone column."""
    if min(n,maximum)<0:raise ValueError('negative monotone extent')
    if not n:return 0
    L=((maximum+n-1)//n).bit_length()
    return 48*((n+63)//64)+8*((n*L+63)//64)

def lexical_block_model(n,m,k,u,longest,reference):
    """Measured lexical-block wire counts on THIS tape. Arithmetic at 10× is sensitivity only."""
    if not (1<=k<=m<=n<2**43 and k<=u<=n and 1<=longest<=1<<20):raise ValueError('invalid measured census')
    out={};ceil=lambda a,b:(a+b-1)//b
    for span in (4,8,16):
        payload,directory,lit,block=(int(reference[f'lex{span}_{x}']) for x in ('payload','directory','literals','maxblock'))
        q=ceil(k,span);headers=2*k-q
        if not (0<lit<=u+8*(k-1) and lit+headers<=payload<=lit+4*headers and directory==8*(1+q) and
                0<block<=min(payload,span*(longest+8)) and block*q>=payload):raise ValueError('lexical block ledger')
        r=dict(payload=payload,directory=directory,literal_bytes=lit,max_block_bytes=block);out[str(span)]=r
        for scale,label in ((1,'measured_dimensions_model'),(10,'tenfold_sensitivity_only')):
            N,M,K=n*scale,m*scale,k*scale
            # Linear payload density for sensitivity ONLY, not equality even for a concatenated text.
            lex=scale*payload+8*(1+ceil(K,span));heads=ef_capacity(K+1,M-K);starts=ef_capacity(M,N);adjusted=ef_capacity(M,N+7*(M-1))
            psi=4*M;samples=12*ceil(M,64)+4*ceil(M,8);tree=16*ceil(M,16);pred=M
            core=lex+heads+starts+adjusted+psi+samples+pred
            r[label]=dict(lexical_payload_and_directory=lex,heads=heads,starts=starts,adjusted_byte_plcp=adjusted,psi=psi,sample_b8=samples,
                predecessor_bytes=pred,rank_block_tree=tree,old_packed_T=scale*u+12*K-8,
                parse_4k_no_dict=20*M+32*K,parse_6k_no_dict=20*M+48*K,
                build_psi_before_dictionary=12*M+psi+samples+heads,alignment_with_ids_no_SA=core+4*M,
                emit_before_order=core+tree,arrays_compare_gib=140,preorder_room=(140<<30)-core-tree,
                rank_sample_extent_ok=M<2**32-1)
    return dict(lexical_blocks=out,scope='Only wire sizes at original dimensions are exact. Modeled payloads exclude orderer, heaps, buffers, stacks, allocator and consumers. No large compressed loader/producer, no time or launch claim.')

def study(n,m,k,u,d,desc,occ):
    """desc / occ are full dense arrays with END-second=0, raw-second+1.
    No assumption about random disk IO, distribution at another N or full product fit.
    """
    if not (0<k<=m<=n<2**43 and k<=u<=n and d==u+9*k-7 and len(desc)==len(occ)==CELLS and
            all(type(x) is int and type(y) is int and 0<=x<=y<=n and bool(x)==bool(y) for x,y in zip(desc,occ)) and
            sum(desc)==u and sum(occ)==n and sum(desc[::257])==sum(occ[::257])==1):raise ValueError('inconsistent exact census')
    cells=[(i,x,y) for i,(x,y) in enumerate(zip(desc,occ)) if x];t=d+3*k-1
    # Existing fixture-only bounded producer loads T after wide sort. These are
    # distinct phase peaks, not simultaneously charged or measured RSS.
    base=t+8*(k+1)+8*k;tree=16*((m+15)//16);fixed=base+21*m+tree+4*(k+1)
    phases=dict(parse_wide_six_k=20*m+48*k,parse_wide_four_k_alternative=20*m+32*k,
                aux_alignment=base+28*m+8,lists=base+37*m+8+tree+4*(k+1),emit_before_arena=fixed)
    narrow=m+6*k<2**31;cap140=140<<30;avail=cap140-fixed
    return dict(n=n,m=m,k=k,descriptors=u,D=d,packed_bytes=t,parse_u32_bytes=4*m,
                prefix_cell_encoding='raw_first*257+second: END=0, raw+1; global END separate',
                distinct_cells_sha256=digest(desc),row_cells_sha256=digest(occ),largest_descriptor_cell=max(cells,key=lambda p:p[1]),
                largest_row_cell=max(cells,key=lambda p:p[2]),
                hypothetical_8byte_descriptor_arenas={str(a):greedy(cells,a//8,True) for a in [256<<20,1<<30,2<<30,4<<30,16<<30]},
                minimum_whole_cell_capacity_by_range_limit={str(b):minimum(cells,b) for b in (8,16,31,63)},
                heap_bytes_upper_without_windows=20*min(k,max(desc)),
                packed_widths=dict(parse_sort_extent=m+6*k,old_parse_extent_fits_i32=narrow,bounded_m_fits=m<2**32-1),
                native_dictionary_sa_alone=8*d,old_bounded_phase_array_model=phases,
                comparison_to_140_gib_only=dict(six_k_and_pre_emit_phases_fit=max(phases[s] for s in ('parse_wide_six_k','aux_alignment','lists'))<=cap140,
                        emit_room_for_whole_arena=max(0,avail),emit_over_budget=max(0,-avail)),
                note='Measured census inputs, modeled ranges/space. Payload excludes heap, stack, classification passes, allocator/consumers. Range caps are not actual equal-key or rank-window work. 140 GiB is the 10 GB array comparison, not a new grant. No product dispatch.')
