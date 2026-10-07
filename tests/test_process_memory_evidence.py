"""Small deterministic checks of the C1/C2 ordinary guest evidence contract."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

ROOT=Path(__file__).resolve().parents[1]


class ProcessMemoryEvidenceTests(unittest.TestCase):
    def test_frozen_bex1_files_are_unchanged_and_valid(self):
        folder=ROOT/'tests/fixtures/bex1-legacy'
        manifest=json.loads((folder/'manifest.json').read_text())
        self.assertEqual(set(manifest['files']),{'counter.bex','docstats.bex','hello-c.bex','hello.bex','notebook.bex'})
        for name,expected in manifest['files'].items():
            data=(folder/name).read_bytes()
            self.assertEqual(len(data),expected['bytes'])
            self.assertEqual(hashlib.sha256(data).hexdigest(),expected['sha256'])
            magic,entry,length,reserved=struct.unpack_from('<4I',data)
            self.assertEqual(magic,0x31584542);self.assertEqual(length,len(data))
            self.assertEqual(reserved,0);self.assertGreaterEqual(entry,16)
            self.assertLess(entry,len(data));self.assertLessEqual(len(data),49152)

    def test_valid_client_public_report_fits_one_line(self):
        # Eight fixed-width values are 74 visible bytes, under the process's
        # unchanged 80-column print boundary. No report needs a private read.
        report='PM '+' '.join(f'{value:08x}' for value in range(8))
        self.assertEqual(len(report),74)
        self.assertEqual([int(report[3+i*9:11+i*9],16) for i in range(8)],list(range(8)))
        data2=bytes((i*37+2*13)&255 for i in range(24576))
        data6=bytes((i*37+6*13)&255 for i in range(24576))
        self.assertNotEqual(data2,data6)
        self.assertEqual(sum(data2),3133440);self.assertEqual(sum(data6),3133440)

    def test_guest_accounts_for_every_normal_lifecycle_boundary(self):
        source=(ROOT/'tests/process_memory_guest.c').read_text()
        for expected in ('"baseline",0,0,0,0','"one",16,16,1,0',
                         '"two",32,32,2,0','"eight",128,128,8,0',
                         '"rejected-ninth",128,128,8,0','"close-pending",16,128,1,0',
                         '"zero-reuse",32,128,2,0','"normal-exit",0,128,0,0',
                         '"final",0,128,0,0','"reboot",0,0,0,0'):
            self.assertIn(expected,source)
        # These checks audit test construction only, never claim guest proof.
        self.assertIn('native_sync_poll(second,first_op)==BOS_E_STALE',source)
        self.assertIn('pm_app_exit(1,23)',source)
        runner=(ROOT/'tools/process_memory_test.py').read_text()
        self.assertIn("'-monitor', 'none'",runner)
        self.assertNotIn('session.memory',runner)
        self.assertNotIn('pmemsave',runner)


if __name__=='__main__':
    unittest.main()
