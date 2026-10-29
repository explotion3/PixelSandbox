"""Read-only PE32/RTTI inspection of a local Noita installation.

Uses only the Python standard library. Does not launch, patch, or inject into
the game. Vtable entries are candidates, not recovered function names/bodies.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


class PE32:
    def __init__(self, path):
        self.data = path.read_bytes()
        if self.data[:2] != b'MZ':
            raise ValueError(f'Not a PE file: {path}')
        pe = self.u32(0x3c)
        if self.data[pe:pe + 4] != b'PE\0\0':
            raise ValueError('Invalid PE signature')
        self.machine, count, _, _, self.symbols, optional_size, _ = struct.unpack_from('<HHIIIHH', self.data, pe + 4)
        optional = pe + 24
        if self.machine != 0x14c or self.u16(optional) != 0x10b:
            raise ValueError('This inspector supports x86 PE32 only; do not reuse its RTTI offsets for x64')
        self.base = self.u32(optional + 28)
        self.entry = self.u32(optional + 16)
        self.directories = optional + 96
        self.sections = []
        for i in range(count):
            at = optional + optional_size + i * 40
            name = self.data[at:at + 8].split(b'\0')[0].decode('ascii')
            virtual_size, rva, size, offset = struct.unpack_from('<IIII', self.data, at + 8)
            if offset + size > len(self.data):
                raise ValueError('Section extends beyond file')
            self.sections.append(dict(name=name, rva=rva, virtual_size=virtual_size,
                                      size=size, offset=offset, flags=self.u32(at + 36)))

    def u16(self, at):
        return struct.unpack_from('<H', self.data, at)[0]

    def u32(self, at):
        return struct.unpack_from('<I', self.data, at)[0]

    def raw(self, rva):
        for section in self.sections:
            if section['rva'] <= rva < section['rva'] + section['size']:
                return section['offset'] + rva - section['rva']
        raise ValueError(f'RVA has no file-backed section: {rva:#x}')

    def rva(self, offset):
        for section in self.sections:
            if section['offset'] <= offset < section['offset'] + section['size']:
                return section['rva'] + offset - section['offset']
        raise ValueError(f'Offset has no section: {offset:#x}')

    def string(self, offset):
        end = self.data.index(b'\0', offset, min(offset + 4096, len(self.data)))
        return self.data[offset:end].decode('ascii')

    def references(self, value):
        return (m.start() for m in re.finditer(re.escape(struct.pack('<I', value)), self.data)
                if m.start() % 4 == 0)

    def directory(self, index):
        return struct.unpack_from('<II', self.data, self.directories + index * 8)

    def imports(self):
        rva, _ = self.directory(1)
        result = []
        if not rva:
            return result
        at = self.raw(rva)
        while any(struct.unpack_from('<IIIII', self.data, at)):
            original, _, _, name, first = struct.unpack_from('<IIIII', self.data, at)
            functions = []
            cursor = self.raw(original or first)
            while self.u32(cursor):
                thunk = self.u32(cursor)
                functions.append('#' + str(thunk & 0xffff) if thunk & 0x80000000
                                 else self.string(self.raw(thunk) + 2))
                cursor += 4
            result.append(dict(dll=self.string(self.raw(name)), functions=functions))
            at += 20
        return result

    def debug(self):
        rva, size = self.directory(6)
        result = []
        if not rva:
            return result
        for at in range(self.raw(rva), self.raw(rva) + size, 28):
            _, _, _, _, kind, length, _, offset = struct.unpack_from('<IIHHIIII', self.data, at)
            item = dict(kind=kind, size=length)
            if kind == 2 and self.data[offset:offset + 4] == b'RSDS':
                # The PDB path is a link-time marker, not proof that symbols are available.
                item['pdb_basename'] = self.string(offset + 24).replace('\\', '/').split('/')[-1]
            result.append(item)
        return result

    def type_name(self, descriptor_va):
        name = self.string(self.raw(descriptor_va - self.base) + 8)
        if not name.startswith(('.?AV', '.?AU')) or not name.endswith('@@'):
            raise ValueError('Not a supported type descriptor')
        return name

    def executable_pointer(self, va):
        return any(s['flags'] & 0x20000000 and s['rva'] <= va - self.base < s['rva'] + s['size']
                   for s in self.sections)

    def hierarchy(self, name):
        # Accept a complete RTTI name for systems outside the grid namespace.
        descriptor_name = name if name.startswith(('.?AV', '.?AU')) else f'.?AV{name}@grid@@'
        needle = descriptor_name.encode('ascii') + b'\0'
        name_offset = self.data.find(needle)
        if name_offset < 0:
            return []
        descriptor_va = self.base + self.rva(name_offset - 8)
        result = []
        for ref in self.references(descriptor_va):
            col = ref - 12
            if col < 0:
                continue
            signature, offset, cd_offset, _, hierarchy_va = struct.unpack_from('<IIIII', self.data, col)
            if signature != 0 or offset > 4096 or cd_offset > 4096:
                continue
            try:
                at = self.raw(hierarchy_va - self.base)
                signature, attributes, count, array_va = struct.unpack_from('<IIII', self.data, at)
                if signature != 0 or not 1 <= count <= 32:
                    continue
                bases = []
                for i in range(count):
                    base_va = self.u32(self.raw(array_va - self.base) + i * 4)
                    td, contained, mdisp, pdisp, vdisp, flags = struct.unpack_from(
                        '<IIiiiI', self.data, self.raw(base_va - self.base))
                    bases.append(dict(name=self.type_name(td), contained_bases=contained,
                                      mdisp=mdisp, pdisp=pdisp, vdisp=vdisp, attributes=flags))
                if bases[0]['name'] != needle[:-1].decode('ascii'):
                    continue
                col_va = self.base + self.rva(col)
                tables = []
                for vref in self.references(col_va):
                    start = vref + 4
                    entries = []
                    for slot in range(128):
                        pointer = self.u32(start + slot * 4)
                        if not self.executable_pointer(pointer):
                            break
                        entries.append(hex(pointer - self.base))
                    if entries:
                        tables.append(dict(vtable_rva=hex(self.rva(start)),
                                           candidate_function_rvas=entries))
                if tables:
                    result.append(dict(col_rva=hex(col_va - self.base), base_offset=offset,
                                       hierarchy_attributes=attributes, bases=bases, vtables=tables))
            except (ValueError, UnicodeDecodeError, struct.error):
                continue
        return result

    def report(self):
        types = [dict(name=m.group()[:-1].decode('ascii'), file_offset=hex(m.start()),
                      rva=hex(self.rva(m.start())))
                 for m in re.finditer(rb'\.\?A[UV][A-Za-z0-9_?$@]+@@\x00', self.data)]
        names = ['ICell', 'ICellBurnable', 'CLiquidCell', 'CGasCell', 'CFireCell',
                 'CSolidCell', 'IGridWorld', 'GridWorld', 'GridWorldThreaded']
        return dict(sha256=hashlib.sha256(self.data).hexdigest(), machine=hex(self.machine),
                    image_base=hex(self.base), entry_rva=hex(self.entry), coff_symbols=self.symbols,
                    sections=self.sections, imports=self.imports(), debug=self.debug(),
                    rtti_type_strings=types, hierarchies={name: self.hierarchy(name) for name in names})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('E:/SteamLibrary/steamapps/common/Noita'))
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'build/evidence/noita-static/audit.json')
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    if output.is_relative_to(source):
        raise ValueError('Output must be outside the read-only source installation')
    result = {name: PE32(source / name).report() for name in ['noita.exe', 'noita_dev.exe']}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    for name, report in result.items():
        print(f"{name}: x86 PE32; SHA-256 {report['sha256']}; COFF symbols={report['coff_symbols']}")
        for cls, items in report['hierarchies'].items():
            primary = next((item for item in items if item['base_offset'] == 0), None)
            if primary:
                print(f"  {cls}: " + ' -> '.join(b['name'] for b in primary['bases']))
    print(f'Report: {output}')


if __name__ == '__main__':
    main()
