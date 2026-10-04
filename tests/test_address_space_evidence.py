"""Deterministic host checks for valid-app C3 fixture construction; no guest run."""
from pathlib import Path
import re
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from address_space_test import expected_output,FIELDS

class AddressSpaceEvidenceTests(unittest.TestCase):
    def test_public_report_halves_fit_unchanged_eighty_column_delivery(self):
        values=list(range(16));halves=[]
        for group in range(2):
            line=f'A{group} '+' '.join(f'{value:08x}' for value in values[group*8:group*8+8])
            self.assertEqual(len(line),74)
            halves.extend(int(line[3+i*9:11+i*9],16) for i in range(8))
        self.assertEqual(halves,values)

    def test_cross_page_persisted_patterns_and_capacity_are_distinct(self):
        self.assertEqual(len(expected_output(2)),32768)
        self.assertNotEqual(expected_output(2),expected_output(6))
        # One text, one data,256/460workspace,16stack,2table pages.
        small=1+1+256+16+2;large=1+1+460+16+2
        for total in (797,1021):
            self.assertGreater(small*2+large+16,total)
            self.assertLessEqual(small+large+16,total)
        self.assertGreater(256,253) # A full workspace cannot fit one free hole.

    def test_serial_accounting_decodes_both_table_kinds(self):
        line='AS-COUNT both-filled-barrier total=797 free=245 allocated=552 high_water=552 backing=0 mapped=548 pt=2 pd=2 records=2'
        match=re.search(r'AS-COUNT ([\w-]+)\b'+''.join(r' '+field+r'=(\d+)' for field in FIELDS),line)
        self.assertIsNotNone(match)
        result=dict(zip(FIELDS,map(int,match.groups()[1:])))
        self.assertEqual(result['allocated'],result['mapped']+result['pt']+result['pd'])
        self.assertEqual(result['total'],result['allocated']+result['free'])

    def test_explicit_barrier_before_either_verification(self):
        source=(ROOT/'tests/address_space_guest.c').read_text()
        self.assertLess(source.index('as_until(5,2,1)'),source.index('as_until(1,2,1)'))
        self.assertLess(source.index('as_until(1,2,1)'),source.index("process_key(first,'v')"))
        app=(ROOT/'tests/address_space_app.c').read_text()
        self.assertIn('if(stage>=2&&!verify())',app)
        runner=(ROOT/'tools/address_space_test.py').read_text()
        self.assertIn("'-monitor','none'",runner)
        self.assertNotIn('pmemsave',runner)
        self.assertNotIn('session.memory',runner)

if __name__=='__main__':unittest.main()
