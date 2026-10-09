#!/usr/bin/env python3
"""List the LV2 syscalls an ELF makes, and refuse the ones this app must never make.

    python3 -I tools/syscalls.py build/ps3_health.elf          # check the ELF against tools/syscalls.allow
    python3 -I tools/syscalls.py build/ps3_health.elf --write  # rewrite the allowlist from this ELF
    python3 -I tools/syscalls.py --selftest                    # the scanner against an ELF built in memory
    python3 -I tools/syscalls.py --allow <file> <elf> [--write]  # another app's allowlist instead of tools/syscalls.allow

PSL1GHT makes a syscall as `li r11, N` followed by `sc` (ppu-lv2.h loads
`register u64 scn asm("11")` with the number). The scan reads every executable
section of the ELF (big-endian ELF64, 4-byte instructions), finds each `sc`
(0x44000002) and takes the nearest `li r11, N` (0x3960nnnn, addi r11, 0, N) in
the sixteen instructions before it, and stops at an earlier `sc` or a `blr`. A number that reaches r11 another way (a
register move, a load) escapes the scan: an `sc` without a `li r11` before it
is counted as unresolved and reported, never silently dropped. So the allowlist
is a guard against a mistake in the source, not a proof. It audits the ELF,
not the firmware: a listed call can still be refused on another console.

Forbidden, whatever the allowlist says: 604 (the LV1 0x22 drive path that froze
the test console on 2026-10-06), 602 (sys_storage_read, refused on HEN) and 870
(the console id, refused on HEN). See README.md, Safety.

The allowlist holds one number per line, with an optional `# note`. A number
in the allowlist that the ELF no longer makes is reported, not an error.

Exit status: 0 when every syscall of the ELF is in the allowlist and none is
forbidden, 1 with the reasons on stderr otherwise. Every check is an explicit
test, not an assert, so python3 -O cannot skip it.
"""
import os
import struct
import sys

FORBIDDEN = {
    604: 'the LV1 0x22 drive path that froze the console',
    602: 'sys_storage_read, refused on HEN',
    870: 'sys_ss_get_console_id, refused on HEN',
}
SC = 0x44000002          # sc
BLR = 0x4E800020         # blr: the end of a function
LI_R11 = 0x39600000      # addi r11, 0, imm  (li r11, imm)
LOOKBACK = 16            # gcc hoists the li r11 up to about ten instructions before the sc (seen: 10)
SHF_EXECINSTR = 0x4
ALLOW = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'syscalls.allow')


def fail(msg):
    sys.exit(f'syscalls.py: {msg}')


def exec_sections(elf):
    """(name, offset, size, address) of every executable section of a big-endian ELF64."""
    if len(elf) < 0x40 or elf[:4] != b'\x7fELF':
        fail('not an ELF')
    if elf[4] != 2 or elf[5] != 2:
        fail('not a big-endian ELF64')
    shoff = struct.unpack_from('>Q', elf, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('>HHH', elf, 0x3A)
    if shoff == 0 or shnum == 0 or shentsize < 0x40:
        fail('the ELF has no section headers (the scan needs .text)')
    if shoff + shnum * shentsize > len(elf):
        fail('section header table beyond the end of the file')

    def header(i):
        return struct.unpack_from('>IIQQQQ', elf, shoff + i * shentsize)   # name, type, flags, addr, offset, size

    if shstrndx >= shnum:
        fail('section name table index out of range')
    _, _, _, _, str_off, str_size = header(shstrndx)
    names = elf[str_off:str_off + str_size]
    out = []
    for i in range(shnum):
        name_off, stype, flags, addr, off, size = header(i)
        if not flags & SHF_EXECINSTR or stype == 8 or size == 0:   # 8 = SHT_NOBITS
            continue
        if off + size > len(elf):
            fail(f'section {i} beyond the end of the file')
        end = names.find(b'\0', name_off)
        name = names[name_off:end if end >= 0 else None].decode('ascii', 'replace')
        out.append((name, off, size, addr))
    if not out:
        fail('no executable section')
    return out


def scan(elf):
    """{number: count} of the syscalls in the ELF, and the addresses of the `sc` with no `li r11` before them.

    The look back stops at an earlier `sc` or at a `blr`: a `li r11` before them
    belongs to another call or another function."""
    found, unresolved = {}, []
    for _, off, size, addr in exec_sections(elf):
        n = size // 4
        words = struct.unpack_from(f'>{n}I', elf, off)
        for i, w in enumerate(words):
            if w != SC:
                continue
            for j in range(i - 1, max(i - LOOKBACK, 0) - 1, -1):
                if words[j] in (SC, BLR):
                    break
                if words[j] & 0xFFFF0000 == LI_R11:
                    imm = words[j] & 0xFFFF
                    if imm >= 0x8000:
                        imm -= 0x10000
                    found[imm] = found.get(imm, 0) + 1
                    break
            else:
                unresolved.append(addr + i * 4)
                continue
            if words[j] in (SC, BLR):
                unresolved.append(addr + i * 4)
    return found, unresolved


def read_allow(path):
    try:
        lines = open(path, encoding='utf-8').read().splitlines()
    except FileNotFoundError:
        fail(f'{path} not found: run with --write on a known-good ELF to create it')
    allowed = set()
    for k, line in enumerate(lines, 1):
        text = line.split('#', 1)[0].strip()
        if not text:
            continue
        if not text.isdigit():
            fail(f'{path}:{k}: not a syscall number: {line!r}')
        allowed.add(int(text))
    return allowed


def check(found, unresolved, allowed):
    """The error lines for a scan result against an allowlist (empty = ok), and the notes."""
    errors, notes = [], []
    for n in sorted(found):
        if n in FORBIDDEN:
            errors.append(f'syscall {n} ({FORBIDDEN[n]}) is in the ELF, {found[n]} time(s)')
    extra = sorted(n for n in found if n not in allowed and n not in FORBIDDEN)
    if extra:
        errors.append('not in the allowlist: ' + ', '.join(str(n) for n in extra))
    unused = sorted(n for n in allowed if n not in found)
    if unused:
        notes.append('in the allowlist, not in the ELF: ' + ', '.join(str(n) for n in unused))
    if unresolved:
        notes.append(f'{len(unresolved)} sc without a li r11 in the {LOOKBACK} instructions before it (not checked): '
                     + ', '.join(f'0x{a:x}' for a in unresolved))
    return errors, notes


def selftest():
    """A big-endian ELF64 built here: three syscalls, one of them forbidden, one unresolved sc."""
    def li_r11(n):
        return LI_R11 | (n & 0xFFFF)
    text = struct.pack('>12I',
                       li_r11(383), 0x7C631B78, SC,            # li r11,383; mr r3,r3; sc
                       li_r11(604), SC,                        # li r11,604; sc
                       0x7D6B5B78, SC,                         # mr r11,r11; sc   (unresolved)
                       li_r11(383), SC,                        # li r11,383; sc   (383 twice)
                       li_r11(-1 & 0xFFFF), SC,                # li r11,-1; sc    (a negative immediate)
                       0x60000000)                             # nop
    names = b'\0.text\0.shstrtab\0'
    ehdr_size, sh_size = 0x40, 0x40
    text_off = ehdr_size
    names_off = text_off + len(text)
    shoff = names_off + len(names)
    shoff += (-shoff) % 8
    ehdr = b'\x7fELF' + bytes([2, 2, 1, 0]) + bytes(8)
    ehdr += struct.pack('>HHIQQQIHHHHHH', 2, 21, 1, 0, 0, shoff, 0, ehdr_size, 0, 0, sh_size, 3, 2)
    assert len(ehdr) == ehdr_size
    blob = bytearray(ehdr) + text + names
    blob += bytes(shoff - len(blob))
    blob += bytes(sh_size)                                                   # the null section
    blob += struct.pack('>IIQQQQIIQQ', 1, 1, SHF_EXECINSTR | 0x2, 0, text_off, len(text), 0, 0, 4, 0)
    blob += struct.pack('>IIQQQQIIQQ', 7, 3, 0, 0, names_off, len(names), 0, 0, 1, 0)
    found, unresolved = scan(bytes(blob))
    if found != {383: 2, 604: 1, -1: 1} or unresolved != [0x18]:
        fail(f'selftest: scan gave {found}, unresolved {unresolved}')
    errors, notes = check(found, unresolved, {383})
    if len(errors) != 2 or '604' not in errors[0] or errors[1] != 'not in the allowlist: -1':
        fail(f'selftest: check gave {errors}')
    if len(notes) != 1 or '0x18' not in notes[0]:
        fail(f'selftest: notes gave {notes}')
    if check({383: 1}, [], {383, 409})[0]:
        fail('selftest: a clean ELF failed')
    print('syscalls.py selftest: ok')


def main(argv):
    if argv == ['--selftest']:
        selftest()
        return
    write = '--write' in argv
    args = [a for a in argv if a != '--write']
    allow = ALLOW
    if '--allow' in args:
        k = args.index('--allow')
        if k + 1 >= len(args):
            sys.exit(__doc__.strip())
        allow = args[k + 1]
        del args[k:k + 2]
    if len(args) != 1:
        sys.exit(__doc__.strip())
    path = args[0]
    try:
        elf = open(path, 'rb').read()
    except OSError as err:
        fail(str(err))
    found, unresolved = scan(elf)
    if write:
        with open(allow, 'w', encoding='utf-8') as f:
            f.write('# LV2 syscalls the ELF makes (tools/syscalls.py --write); a number per line, # notes allowed.\n')
            for n in sorted(found):
                f.write(f'{n}\n')
        print(f'{allow}: {len(found)} syscalls written from {path}')
    errors, notes = check(found, unresolved, read_allow(allow))
    for line in notes:
        print(f'syscalls.py: note: {line}')
    if errors:
        for line in errors:
            print(f'syscalls.py: {line}', file=sys.stderr)
        sys.exit(1)
    print(f'{path}: {len(found)} syscalls ({sum(found.values())} call sites), all in {os.path.basename(allow)}, none forbidden')


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] in ('-h', '--help'):
        print(__doc__.strip())
        sys.exit(0)
    main(sys.argv[1:])
