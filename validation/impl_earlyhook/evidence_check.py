"""Read-only evidence checks. Never loads a driver or modifies another process."""
import argparse, ctypes, hashlib, json, pathlib, struct, time, unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

def identity(path):
    p = pathlib.Path(path)
    data = p.read_bytes()
    return {'path': str(p.resolve()), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}

def same_sha256(actual, expected):
    return str(actual).lower() == str(expected).lower()

def codeview(path):
    data = pathlib.Path(path).read_bytes()
    if data[:2] != b'MZ': raise ValueError('Not a PE image')
    nt = struct.unpack_from('<I', data, 60)[0]
    if data[nt:nt+4] != b'PE\0\0': raise ValueError('Invalid PE signature')
    count, opts = struct.unpack_from('<H', data, nt+6)[0], struct.unpack_from('<H', data, nt+20)[0]
    opt = nt+24
    directory = opt + (112 if struct.unpack_from('<H',data,opt)[0] == 0x20b else 96)
    rva, size = struct.unpack_from('<II',data,directory+6*8)
    def raw(addr):
        for i in range(count):
            s = opt+opts+i*40
            vs, va, rs, off = struct.unpack_from('<IIII',data,s+8)
            if va <= addr < va+max(vs,rs): return off+addr-va
        raise ValueError('Unmapped RVA')
    if not rva: raise ValueError('No debug directory')
    debug = raw(rva)
    for off in range(debug,debug+size,28):
        kind, length, ptr = struct.unpack_from('<I',data,off+12)[0], struct.unpack_from('<I',data,off+16)[0], struct.unpack_from('<I',data,off+24)[0]
        if kind == 2 and data[ptr:ptr+4] == b'RSDS':
            return {'guid_hex':data[ptr+4:ptr+20].hex(), 'age':struct.unpack_from('<I',data,ptr+20)[0], 'pdb_path':data[ptr+24:ptr+length].split(b'\0')[0].decode(errors='replace')}
    raise ValueError('No RSDS CodeView record')

def pdb_identity(path):
    d = pathlib.Path(path).read_bytes()
    if not d.startswith(b'Microsoft C/C++ MSF 7.00'): raise ValueError('Unsupported PDB format')
    block, _, _, nbytes, _, blockmap = struct.unpack_from('<6I',d,32)
    if block not in (512,1024,2048,4096,8192): raise ValueError('Invalid MSF block size')
    nblocks = (nbytes+block-1)//block
    ids = struct.unpack_from('<'+'I'*nblocks,d,blockmap*block)
    directory = b''.join(d[i*block:(i+1)*block] for i in ids)[:nbytes]
    streams = struct.unpack_from('<I',directory)[0]
    sizes = struct.unpack_from('<'+'I'*streams,directory,4)
    pos, info = 4+4*streams, None
    for index, size in enumerate(sizes):
        nb = 0 if size == 0xffffffff else (size+block-1)//block
        pages = struct.unpack_from('<'+'I'*nb,directory,pos) if nb else ()
        pos += nb*4
        if index == 1: info = b''.join(d[i*block:(i+1)*block] for i in pages)[:size]
    if not info or len(info)<28: raise ValueError('Missing PDB info stream')
    return {'age':struct.unpack_from('<I',info,8)[0], 'guid_hex':info[12:28].hex()}

def status_check(status, arm, status_mtime, arm_mtime, now, max_age=10, now_ms=None):
    errors = []
    if len(status) not in (792, 792+36): raise ValueError('Unexpected legacy/v21 protocol size')
    if len(arm)!=3752: raise ValueError('Unexpected legacy protocol size')
    magic, ack, armed, count, selftest = struct.unpack_from('<5I', status)
    amagic, seq = struct.unpack_from('<2I',arm)
    pid, requested = struct.unpack_from('<QI',arm,24)
    if magic!=0x31534452 or amagic!=0x31314452: raise ValueError('Bad protocol magic')
    if not 0<count<=8 or requested!=count: errors.append('ENTRY_COUNT_MISMATCH')
    if ack!=seq: errors.append('ACK_MISMATCH')
    if not armed: errors.append('NOT_ARMED')
    if status_mtime<arm_mtime: errors.append('STATUS_PREDATES_REQUEST')
    if now-status_mtime>max_age: errors.append('STALE_SNAPSHOT')
    entries=[]
    for i in range(min(count,8)):
        real, orig, state, _, hits = struct.unpack_from('<QQIIq',status,24+i*32)
        if state&0x10 and (state&0xf)!=0xf: errors.append(f'ENTRY_{i}_IMPOSSIBLE_DETOUR_STATE')
        entries.append({'index':i,'real':hex(real),'orig':hex(orig),'state':hex(state),'rescan_hits':hits})
    r={'pid':pid,'ack':ack,'request_seq':seq,'errors':errors,'entries':entries,
       'limitations':['Legacy selftest field is not populated reliably; zero is not proof of success.',
                     'Legacy DETOUR is historical; it provides neither current byte verification nor execution counts.',
                     'ACK proves request consumption only, not completion of a functioning hook or capture.']}
    tail=None
    if len(status)==792+36:
        m,ver,nonce,lsb,lsh,hb_lo,hb_hi,writes,chk = struct.unpack_from('<9I',status,792)
        load=(lsh<<32)|lsb; hb=(hb_hi<<32)|hb_lo
        tail={'magic':hex(m),'version':ver,'nonce':hex(nonce),'loadStampMs':load//10000,
              'heartbeatMs':hb//10000,'statusWrites':writes}
        if m!=0x32544452: errors.append('TAIL_BAD_MAGIC')
        if ver!=21: errors.append('TAIL_BAD_VERSION')
        if nonce==0: errors.append('TAIL_NONCE_ZERO')
        if chk!=sum((m,ver,nonce,lsb,lsh,hb_lo,hb_hi,writes))&0xffffffff: errors.append('TAIL_CHECKSUM_MISMATCH')
        if hb<load: errors.append('TAIL_HEARTBEAT_BEFORE_LOAD')
        if now_ms is not None and hb//10000>now_ms+1000: errors.append('TAIL_HEARTBEAT_IN_FUTURE')
        r['tail']=tail
        r['limitations'].append('v21 tail proves liveness only at write time; take two snapshots and '
                                'confirm the same nonce with advancing statusWrites/heartbeat.')
    else:
        r['limitations'].append('Legacy 792-byte file: no build/instance nonce or heartbeat (v21 tail absent).')
    return r

class Tests(unittest.TestCase):
    def test_sha256_comparison_is_case_insensitive(self):
        self.assertTrue(same_sha256('a1b2c3', 'A1B2C3'))
        self.assertFalse(same_sha256('a1b2c3', 'A1B2C4'))

    def fixture(self,state=0x5f,ack=7):
        s,a=bytearray(792),bytearray(3752)
        struct.pack_into('<5I',s,0,0x31534452,ack,1,1,0)
        struct.pack_into('<QQIIq',s,24,0x10000,0x20000,state,0,0)
        struct.pack_into('<2I',a,0,0x31314452,7)
        struct.pack_into('<QI',a,24,1234,1)
        return s,a
    def test_valid_snapshot_is_not_execution_proof(self):
        s,a=self.fixture(); r=status_check(s,a,100,99,101)
        self.assertEqual(r['errors'],[]); self.assertEqual(len(r['limitations']),4)
    def test_corrupt_state_detected(self):
        for value in (0x5d,0x5e):
            s,a=self.fixture(value)
            self.assertIn('ENTRY_0_IMPOSSIBLE_DETOUR_STATE',status_check(s,a,100,99,101)['errors'])
    def test_handshake_and_staleness(self):
        s,a=self.fixture(ack=6); errors=status_check(s,a,90,99,120)['errors']
        for e in ('ACK_MISMATCH','STATUS_PREDATES_REQUEST','STALE_SNAPSHOT'): self.assertIn(e,errors)
    def test_truncated_protocol(self):
        with self.assertRaises(ValueError): status_check(b'',b'',0,0,0)
    def test_trampoline_layout_regression(self):
        # Model the actual field boundaries without performing an out-of-bounds C write.
        for stolen in range(12,17):
            copied=stolen+14
            self.assertGreater(copied,16)
            self.assertLessEqual(copied,32)
            old=bytearray(32); old[16:30]=b'X'*14; old[:copied]=b'T'*copied
            self.assertNotEqual(old[24:26],b'XX')
            fixed=bytearray(48); fixed[32:]=b'X'*16; fixed[:copied]=b'T'*copied
            self.assertEqual(fixed[32:],b'X'*16)

    @staticmethod
    def v21_tail(nonce=0xA1B2C3D4,load_ms=1_000,hb_ms=60_000,writes=7,bad_checksum=False,zero_nonce=False):
        load=load_ms*10000; hb=hb_ms*10000
        n=0 if zero_nonce else nonce
        head=[0x32544452,21,n,load&0xffffffff,load>>32,hb&0xffffffff,hb>>32,writes]
        chk=sum(head)&0xffffffff
        if bad_checksum: chk^=0xDEAD
        return struct.pack('<9I',*head,chk)

    def test_v21_tail_valid(self):
        s,a=self.fixture()
        s=s+bytearray(36)
        s[792:828]=self.v21_tail()
        r=status_check(bytes(s),bytes(a),100,99,101,now_ms=120_000)
        self.assertEqual(r['errors'],[])
        self.assertEqual(r['tail']['version'],21)
        self.assertEqual(r['tail']['statusWrites'],7)
        self.assertTrue(any('v21 tail' in x for x in r['limitations']))

    def test_v21_tail_corruption_detected(self):
        s,a=self.fixture()
        s=s+bytearray(36)
        s[792:828]=self.v21_tail(bad_checksum=True)
        self.assertIn('TAIL_CHECKSUM_MISMATCH',status_check(bytes(s),bytes(a),100,99,101)['errors'])
        s2,_=self.fixture()
        s2=s2+bytearray(36)
        s2[792:828]=self.v21_tail(zero_nonce=True)
        self.assertIn('TAIL_NONCE_ZERO',status_check(bytes(s2),bytes(a),100,99,101)['errors'])

    def test_v21_tail_time_semantics(self):
        s,a=self.fixture()
        s=s+bytearray(36)
        s[792:828]=self.v21_tail(load_ms=60_000,hb_ms=1_000)   # heartbeat before load
        self.assertIn('TAIL_HEARTBEAT_BEFORE_LOAD',status_check(bytes(s),bytes(a),100,99,101)['errors'])
        s3,_=self.fixture()
        s3=s3+bytearray(36)
        s3[792:828]=self.v21_tail(load_ms=1_000,hb_ms=500_000) # heartbeat beyond uptime
        self.assertIn('TAIL_HEARTBEAT_IN_FUTURE',status_check(bytes(s3),bytes(a),100,99,101,now_ms=100_000)['errors'])

def main():
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('mode',choices=['manifest','verify','status','test'])
    p.add_argument('--manifest',type=pathlib.Path)
    p.add_argument('--status',type=pathlib.Path,default=pathlib.Path('C:/rdoc_status.bin'))
    p.add_argument('--arm',type=pathlib.Path,default=pathlib.Path('C:/rdoc_arm.bin'))
    args=p.parse_args()
    if args.mode=='test':
        return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests)).wasSuccessful() else 1
    if args.mode=='status':
        import ctypes
        now_ms=ctypes.windll.kernel32.GetTickCount64() if hasattr(ctypes,'windll') else None
        r=status_check(args.status.read_bytes(),args.arm.read_bytes(),args.status.stat().st_mtime,args.arm.stat().st_mtime,time.time(),now_ms=now_ms)
        print(json.dumps(r,indent=2)); return 2 if r['errors'] else 0
    if args.mode=='verify':
        if not args.manifest: p.error('--manifest is required')
        manifest=json.loads(args.manifest.read_text(encoding='utf-8-sig'))
        errors=[item['path'] for item in manifest['files'] if not pathlib.Path(item['path']).is_file() or not same_sha256(identity(item['path'])['sha256'],item['sha256'])]
        print(json.dumps({'changed_or_missing':errors},indent=2)); return 2 if errors else 0
    dll=ROOT/'renderdoc/x64/Release/rendertest.dll'; pdb=dll.with_suffix('.pdb')
    cv,pi=codeview(dll),pdb_identity(pdb)
    match=all(cv[k]==pi[k] for k in ('guid_hex','age'))
    paths=[dll,pdb,ROOT/'RDocKit/scripts/chain_kernel.ps1',ROOT/'driver/build3/rdoc_earlyhook.sys',ROOT/'RDocKit/bin/x64/rendertest.dll']
    r={'files':[identity(f) for f in paths], 'dll_codeview':cv,'pdb_identity':pi,'dll_pdb_match':match,
       'note':'Manifest pins the script and images together; it does not validate hard-coded RVAs or prove runtime execution.'}
    print(json.dumps(r,indent=2)); return 0 if match else 2

if __name__=='__main__':
    try: raise SystemExit(main())
    except (OSError,ValueError,struct.error) as exc:
        print(json.dumps({'error':str(exc)})); raise SystemExit(2)
