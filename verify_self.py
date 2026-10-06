#!/usr/bin/env python3
"""Check an EBOOT.BIN from make_self_npdrm, against its ELF when given.

    python3 -I verify_self.py build/pkg/USRDIR/EBOOT.BIN [build/hdd_ssd_health.elf]

The macOS ARM64 make_self crashed with a bus error after "self written in
memory", so build.sh runs this before any pkg leaves the Mac. make_self writes
the plaintext metadata key npdrm_keypair_d as the encrypted blob npdrm_keypair_e
(PSL1GHT tools/geohot/oddkeys.h); with the plaintext, the rest of the metadata
decrypts (AES-CTR). Then it checks what the console's loader checks: each
segment's HMAC-SHA1 (random 64-byte key in the metadata, over the compressed
plaintext) and the ECDSA signature over the plaintext header (fixed r, k, Da, n
from PSL1GHT keys.h, so s = (z + r*Da) / k mod n is reproducible). With the
ELF, every segment must also inflate to the ELF's own bytes, and the plaintext
copy of the section headers must match.

NOTICE: the key material (KEYPAIR_E, ERK, RIV, SIG_R, SIG_N, SIG_K, SIG_DA)
belongs to PSL1GHT's tools (tools/geohot) and is not part of this repository
or of its MIT grant. The script reads it from the TOML file named by
$PS3DEV_KEYS (default ~/.local/share/ps3dev/keys.toml), one hex string per
name, and never ships it.

Exit status: 0 when every check passes, 1 with one line on stderr otherwise.
Every check is an explicit test, not an assert, so python3 -O cannot skip it.
"""
import hashlib, hmac, os, struct, sys, tomllib, zlib

KEY_FILE = os.environ.get('PS3DEV_KEYS') or os.path.expanduser('~/.local/share/ps3dev/keys.toml')
KEY_NAMES = ('KEYPAIR_E', 'ERK', 'RIV', 'SIG_R', 'SIG_N', 'SIG_K', 'SIG_DA')


def fail(msg):
    sys.exit(f'verify_self.py: {msg}')


def load_keys(key_file=KEY_FILE):
    """The seven constants from the TOML file: bytes for the AES/blob values, ints for ECDSA."""
    try:
        with open(key_file, 'rb') as f:
            raw = tomllib.load(f)
    except FileNotFoundError:
        fail(f'key file {key_file} not found: copy the make_self_npdrm constants from PSL1GHT '
             f'(tools/geohot keys.h and oddkeys.h) into it as hex strings, one per name '
             f'({", ".join(KEY_NAMES)}), or set PS3DEV_KEYS')
    except tomllib.TOMLDecodeError as e:
        fail(f'key file {key_file} is not valid TOML: {e}')
    missing = [n for n in KEY_NAMES if not isinstance(raw.get(n), str)]
    if missing:
        fail(f'key file {key_file} lacks {", ".join(missing)} (hex strings)')
    try:
        k = {n: bytes.fromhex(raw[n]) for n in ('KEYPAIR_E', 'ERK', 'RIV')}
        k.update({n: int(raw[n], 16) for n in ('SIG_R', 'SIG_N', 'SIG_K', 'SIG_DA')})
    except ValueError as e:
        fail(f'key file {key_file}: a value is not hex: {e}')
    if len(k['KEYPAIR_E']) != 0x40 or len(k['ERK']) != 16 or len(k['RIV']) != 16:
        fail(f'key file {key_file}: KEYPAIR_E must be 64 bytes, ERK and RIV 16 bytes')
    return k


def ctr(key, iv, data):
    from Crypto.Cipher import AES
    from Crypto.Util import Counter
    return AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=int.from_bytes(iv, 'big'))).decrypt(data)


def main(self_path, elf_path=None):
    k = load_keys()
    try:
        s = open(self_path, 'rb').read()
        e = open(elf_path, 'rb').read() if elf_path else None
    except OSError as err:
        fail(str(err))
    if len(s) < 0x20:
        fail(f'{self_path}: too short for a SELF header')
    magic, _, _, htype, meta_off, hdr_len = struct.unpack_from('>IIHHIQ', s, 0)
    if magic != 0x53434500 or htype != 1:
        fail(f'{self_path}: not a SELF')
    mi = 0x20 + meta_off
    if s[mi:mi + 0x40] != k['KEYPAIR_E']:
        fail('metadata key blob is not npdrm_keypair_e')
    rest = ctr(k['ERK'], k['RIV'], s[mi + 0x40:hdr_len])
    sig_off, _, nsec, nkey = struct.unpack_from('>QIII', rest, 0)
    keys = rest[0x20 + nsec * 0x30:0x20 + nsec * 0x30 + nkey * 0x10]
    checked = 0
    for i in range(nsec):
        doff, dsize, stype, idx, hashed, sidx, enc, kidx, vidx, comp = struct.unpack_from('>QQIIIIIIII', rest, 0x20 + i * 0x30)
        if stype != 2:                      # 2 = program segment
            continue
        data = s[doff:doff + dsize]
        if enc == 3:
            data = ctr(keys[kidx * 16:kidx * 16 + 16], keys[vidx * 16:vidx * 16 + 16], data)
        if hashed == 2:                     # HMAC-SHA1: the digest (0x20 slot), then the 0x40 key
            digest, hkey = keys[sidx * 16:sidx * 16 + 0x14], keys[sidx * 16 + 0x20:sidx * 16 + 0x60]
            if hmac.new(hkey, data, hashlib.sha1).digest() != digest:
                fail(f'segment {idx}: HMAC-SHA1 wrong')
        if comp == 2:
            data = zlib.decompress(data)
        if e:
            phoff, phentsize = struct.unpack_from('>Q', e, 0x20)[0], struct.unpack_from('>H', e, 0x36)[0]
            p_offset = struct.unpack_from('>Q', e, phoff + idx * phentsize + 8)[0]
            filesz = struct.unpack_from('>Q', e, phoff + idx * phentsize + 0x20)[0]
            want = e[p_offset:p_offset + filesz]
            if data != want:
                fail(f'segment {idx}: {len(data)} bytes decrypted != {len(want)} ELF bytes')
        checked += 1
    plain = bytearray(s[:hdr_len])          # the header as signed: before metadata encryption
    plain[mi:mi + 0x40] = k['ERK'] + bytes(16) + k['RIV'] + bytes(16)
    plain[mi + 0x40:hdr_len] = rest
    z = int.from_bytes(hashlib.sha1(plain[:sig_off]).digest(), 'big')
    want_s = (z + k['SIG_R'] * k['SIG_DA']) * pow(k['SIG_K'], -1, k['SIG_N']) % k['SIG_N']
    got_r = int.from_bytes(plain[sig_off + 1:sig_off + 0x15], 'big')
    got_s = int.from_bytes(plain[sig_off + 0x16:sig_off + 0x2a], 'big')
    if got_r != k['SIG_R'] or got_s != want_s:
        fail(f'signature wrong (r ok: {got_r == k["SIG_R"]})')
    if e:
        phnum = struct.unpack_from('>H', e, 0x38)[0]
        if checked != phnum:
            fail(f'{checked} of {phnum} program segments in the metadata')
        shoff = struct.unpack_from('>Q', e, 0x28)[0]
        shentsize, shnum = struct.unpack_from('>HH', e, 0x3A)
        self_shoff = struct.unpack_from('>Q', s, 0x40)[0]   # Self_Ehdr e_shoff
        if s[self_shoff:self_shoff + shentsize * shnum] != e[shoff:shoff + shentsize * shnum]:
            fail('section headers differ')
        print(f'{self_path}: {checked} segments, HMACs, signature and {shnum} section headers match the ELF')
    else:
        print(f'{self_path}: {checked} segment HMACs and the signature are valid')


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] in ('-h', '--help'):
        print(__doc__.strip())
        sys.exit(0)
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip())
    main(*sys.argv[1:])
