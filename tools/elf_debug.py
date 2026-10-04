"""Read bounded production data layouts from compiler-emitted DWARF.

Used by normal desktop input tests for observation only. It never evaluates
expressions, calls guest functions, or writes guest memory.
"""
import re
import subprocess


class DebugInfo:
    def __init__(self, elf):
        text = subprocess.check_output(['readelf', '--debug-dump=info', str(elf)], text=True)
        self.entries = {}
        unit = None
        pattern = r'<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)(.*?)(?=\n\s*<\d+><|\Z)'
        for match in re.finditer(pattern, text, re.S):
            depth, address, tag, body = match.groups()
            if tag == 'DW_TAG_compile_unit':
                unit = self.attribute(body, 'name')
            self.entries[int(address, 16)] = dict(depth=int(depth), tag=tag, body=body, unit=unit)

    @staticmethod
    def attribute(body, name):
        match = re.search(r'DW_AT_' + re.escape(name) + r'\s*:(.*)', body)
        if not match:
            return None
        value = match[1].strip()
        return value.rsplit(':', 1)[-1].strip() if value.startswith('(') else value

    def named(self, name, tags, unit=None):
        found = [(address, entry) for address, entry in self.entries.items()
                 if entry['tag'] in tags and self.attribute(entry['body'], 'name') == name
                 and (unit is None or entry['unit'].endswith(unit))]
        if len(found) != 1:
            raise ValueError(f'Expected one {name!r} in {unit!r}, found {len(found)}')
        return found[0]

    def type_reference(self, entry):
        match = re.search(r'DW_AT_type\s*: <0x([0-9a-f]+)>', entry['body'])
        if not match:
            raise ValueError('Missing concrete DWARF type')
        return int(match[1], 16)

    def structure_at(self, address):
        entry = self.entries[address]
        while entry['tag'] in ('DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type'):
            address = self.type_reference(entry)
            entry = self.entries[address]
        if entry['tag'] != 'DW_TAG_structure_type':
            raise ValueError('Expected a structure')
        size = int(self.attribute(entry['body'], 'byte_size'), 0)
        fields = {}
        started = False
        for child_address, child in self.entries.items():
            if child_address == address:
                started = True
                continue
            if not started:
                continue
            if child['depth'] <= entry['depth']:
                break
            if child['depth'] == entry['depth'] + 1 and child['tag'] == 'DW_TAG_member':
                fields[self.attribute(child['body'], 'name')] = int(
                    self.attribute(child['body'], 'data_member_location'), 0)
        return size, fields

    def structure(self, name, unit=None):
        address, _ = self.named(name, ('DW_TAG_typedef', 'DW_TAG_structure_type'), unit)
        return self.structure_at(address)

    def variable(self, name, unit=None):
        _, entry = self.named(name, ('DW_TAG_variable',), unit)
        location = re.search(r'DW_OP_addr: ([0-9a-f]+)', entry['body'])
        if not location:
            raise ValueError('Variable has no fixed production data address')
        return int(location[1], 16), self.structure_at(self.type_reference(entry))
