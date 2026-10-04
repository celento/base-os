"""Host-only construction/evidence checks. No QEMU or native guest is run."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import larger_memory_test as gate
import init_data
import volume
from build_app import build


class LargerMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='baseos-larger-memory-host-')
        cls.work = Path(cls.temporary.name)
        cls.inspector = cls.work / 'inspector'
        subprocess.run([shutil.which('cc') or 'gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'src'), str(ROOT / 'tests/executable_inspect_host.c'),
                        str(ROOT / 'src/executable.c'), '-o', str(cls.inspector)], check=True)
        cls.plans = {}
        for label in ('memory-a', 'memory-b'):
            path = cls.work / (label + '.bex')
            build(ROOT / 'tests/larger_memory_fixture_app.c', path, format='bex2', workspace_bytes=gate.WORKSPACE,
                  stack_bytes=65536, required_abi_minor=1, elf_output=path.with_suffix('.elf'))
            cls.plans[label] = json.loads(subprocess.check_output([str(cls.inspector), str(path)], text=True))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def serial(self, total=4096, reboot=False):
        # Synthetic evidence exercises the collector only, never a guest claim.
        text = f'Owned pages total={total} allocated=0 high_water=0\n'
        populations = [(0, 0, 0)] * 2 if reboot else [
            (0, 0, 0), (0, 0, 1), (1, 0, 1), (1, 1, 1), (1, 1, 1), (1, 0, 1),
            (1, 1, 1), (0, 1, 1), (1, 1, 1), (0, 1, 1), (0, 0, 1), (0, 0, 0), (0, 0, 0)]
        labels = ['baseline', 'reboot'] if reboot else gate.CHECKPOINTS
        high = 0
        for label, (a, b, legacy) in zip(labels, populations):
            mapped = a * self.plans['memory-a']['mapped_pages'] + b * self.plans['memory-b']['mapped_pages']
            owned = mapped + (a + b) * 2 + legacy * 16
            high = max(high, owned)
            values = (total, total - owned, owned, high, legacy * 16, mapped, a + b, a + b, a + b + legacy)
            text += 'AS-COUNT ' + label + ''.join(f' {key}={value}' for key, value in zip(gate.FIELDS, values)) + '\n'
        if not reboot:
            for label in ('a-filled', 'b-filled', 'a-retained', 'b-retained', 'a-after-peer-close',
                          'b-zero-relaunch', 'b-after-peer-close', 'a-zero-relaunch'):
                a = label.startswith('a-')
                plan = self.plans['memory-a' if a else 'memory-b']
                owner = (101 if a else 102) + (2 if ('zero-relaunch' in label or label == 'b-after-peer-close') else 0)
                first = label.endswith('filled')
                values = (owner, 6 if a else 2, 1 if first else 2, gate.expected_checksum(6 if a else 2), 1 if first else 20,
                          0 if first else 17, 0, 4294967290, plan['mapped_pages'], plan['owned_pages'], 2,
                          gate.WORKSPACE, plan['workspace'][0], plan['stack'][0], 0 if first else 20, 0)
                text += 'LM-APP ' + label + ''.join(f' {key}={value}' for key, value in zip(gate.APP_FIELDS, values)) + '\n'
        return text

    def test_valid_parser_plans_retain_existing_policy(self):
        for plan in self.plans.values():
            self.assertEqual(plan['workspace'][1:], [3145728, 768])
            self.assertEqual(plan['stack'][1:], [65536, 16])
            self.assertEqual(plan['owned_pages'], plan['mapped_pages'] + 2)
            self.assertEqual(plan['table_pages'], 2)
            self.assertLessEqual(plan['owned_pages'], 1024)
        needed = sum(plan['owned_pages'] for plan in self.plans.values()) + 16
        self.assertGreater(needed, 1021)

    def test_full_workspace_checksum_and_every_page_identity(self):
        identities = []
        for display, checksum in ((2, 401102976), (6, 401151104)):
            data = bytes(gate.expected_byte(i, display) for i in range(gate.WORKSPACE))
            self.assertEqual(sum(data), checksum)
            self.assertEqual(sum(data), gate.expected_checksum(display))
            headers = [data[page * 4096:page * 4096 + 4] for page in range(768)]
            self.assertEqual(len(set(headers)), 768)
            self.assertEqual(len(set(data[i * 4096:(i + 1) * 4096] for i in range(768))), 768)
            identities.append(set(headers))
        self.assertTrue(identities[0].isdisjoint(identities[1]))
        self.assertNotEqual(gate.expected_output(2), gate.expected_output(6))
        self.assertEqual(len(gate.expected_output(2)), 32768)

    def test_actual_pool_not_guessed_from_nominal_ram(self):
        for total in (4096, 8192):
            result = gate.decode_counts(self.serial(total), self.plans)
            self.assertEqual(result['actual_e820_filtered_total'], total)
            final = result['checkpoints'][-1]
            self.assertEqual(final['free'], total)
            self.assertEqual(final['allocated'], 0)
            self.assertEqual(final['high_water'], sum(p['owned_pages'] for p in self.plans.values()) + 16)
            self.assertEqual(final['records'], 0)
            self.assertEqual(len(result['application_reports']), 8)
            self.assertEqual(gate.decode_counts(self.serial(total, True), self.plans, True)['actual_e820_filtered_total'], total)

    def test_accounting_and_barrier_evidence_are_required(self):
        text = self.serial()
        for changed in (text.replace('pt=2', 'pt=1', 1), text.replace('allocated=0 high_water=0', 'allocated=1 high_water=0', 1),
                        text.replace(' checksum=' + str(gate.expected_checksum(6)), ' checksum=0', 1),
                        text.replace('AS-COUNT both-filled-barrier', 'AS-COUNT skipped'),
                        text.replace('LM-APP b-retained', 'LM-APP skipped')):
            with self.assertRaises(AssertionError):
                gate.decode_counts(changed, self.plans)

    def test_stopped_native_files_and_frozen_programs_are_exact(self):
        with tempfile.TemporaryDirectory() as tmp:
            disk = Path(tmp) / 'default.img'
            apps = {'counter.bex': b'frozen bytes'}
            documents = {name.rsplit('/', 1)[1]: data for name, data in gate.expected_files().items()}
            gate.desktop.seed_disk(disk, 'default', apps, documents, dict(init_data=init_data, volume=volume))
            report = dict(original_files={'/Programs/counter.bex': dict(bytes=12, sha256=hashlib.sha256(b'frozen bytes').hexdigest())})
            output = gate.validate_stopped(disk, report, volume)
            self.assertEqual(output['/Documents/counter-4.txt']['bytes'], 3)
            report['original_files']['/Programs/counter.bex']['bytes'] = 13
            with self.assertRaisesRegex(AssertionError, 'Original frozen'):
                gate.validate_stopped(disk, report, volume)

    def test_build_errors_keep_exact_command_and_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            failed = subprocess.CompletedProcess(['compiler', 'source.c'], 7, 'stdout\n', 'stderr\n')
            with patch.object(gate.subprocess, 'run', return_value=failed):
                with self.assertRaises(subprocess.CalledProcessError):
                    gate.command(work, 'failed', ['compiler', 'source.c'])
            self.assertEqual(json.loads((work / 'failed.command.json').read_text()), ['compiler', 'source.c'])
            self.assertEqual((work / 'failed.log').read_text(), 'stdout\nstderr\n')
            with patch.object(gate.subprocess, 'run', side_effect=FileNotFoundError('missing compiler')):
                with self.assertRaises(FileNotFoundError):
                    gate.command(work, 'missing', ['compiler', 'source.c'])
            self.assertEqual(json.loads((work / 'missing.command.json').read_text()), ['compiler', 'source.c'])
            self.assertIn('missing compiler', (work / 'missing.log').read_text())

    def test_prepare_only_does_not_enter_execution(self):
        args = SimpleNamespace(build=ROOT / 'build', work=ROOT / 'unused-preparation',
                               memory_mib=256, prepare_only=True)
        with patch.object(gate.desktop, 'runtime_tools', return_value=(ROOT, {})), \
             patch.object(gate, 'prepare', return_value={}) as prepared, \
             patch.object(gate, 'run', side_effect=AssertionError('No guest allowed')):
            gate.main(args)
            prepared.assert_called_once()

    def test_explicit_optional_ram_and_coordinator_grant(self):
        args = SimpleNamespace(build=ROOT / 'build', work=ROOT / 'unused-preparation',
                               memory_mib=256, prepare_only=False, qemu_slot_held=False)
        with patch.object(gate, 'run', side_effect=AssertionError('No guest allowed')):
            with self.assertRaisesRegex(ValueError, 'qemu-slot-held'):
                gate.main(args)
            args.memory_mib = 128
            with self.assertRaisesRegex(ValueError, 'optional 256'):
                gate.main(args)

    def test_rejected_rerun_preserves_original_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'report.json'
            original = b'{"passed": true, "status": "passed"}\n'
            path.write_bytes(original)
            args = SimpleNamespace(build=ROOT / 'build', work=Path(tmp), memory_mib=256,
                                   prepare_only=False, qemu_slot_held=True)
            with patch.object(gate.desktop, 'runtime_tools', return_value=(ROOT, {})), \
                 patch.object(gate, 'run', side_effect=AssertionError('No guest allowed')):
                with self.assertRaisesRegex(ValueError, 'Never reuse'):
                    gate.main(args)
            self.assertEqual(path.read_bytes(), original)
            refusal = json.loads(next(Path(tmp).glob('admission-refusal-*.json')).read_text())
            self.assertFalse(refusal['guest_started'])
            self.assertIn('Never reuse', refusal['error'])

    def test_run_profile_selection_matches_prepared_scope(self):
        for prepared, requested in ((('default',), 'both'), (('default', 'large'), 'default'), (('large',), 'default')):
            with self.subTest(prepared=prepared, requested=requested), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'report.json'
                original = json.dumps(dict(status='prepared; no guest started', profiles=dict.fromkeys(prepared, {}))).encode()
                path.write_bytes(original)
                args = SimpleNamespace(build=ROOT / 'build', work=Path(tmp), memory_mib=256,
                                       prepare_only=False, qemu_slot_held=True, disk_profile=requested)
                with patch.object(gate.desktop, 'runtime_tools', return_value=(ROOT, {})), \
                     patch.object(gate, 'run', side_effect=AssertionError('No guest allowed')):
                    with self.assertRaisesRegex(ValueError, 'Requested disk profiles'):
                        gate.main(args)
                self.assertEqual(path.read_bytes(), original)
                refusal = json.loads(next(Path(tmp).glob('admission-refusal-*.json')).read_text())
                self.assertFalse(refusal['guest_started'])

    def test_fixture_barrier_and_existing_gates_unchanged(self):
        guest = (ROOT / 'tests/larger_memory_guest.c').read_text()
        self.assertLess(guest.index('as_until(1,2,1)'), guest.index("process_key(first,'v')"))
        self.assertIn('as_baseline=initial.total;', guest)
        self.assertNotIn('as_baseline==', guest)
        runner = (ROOT / 'tools/larger_memory_test.py').read_text()
        self.assertIn("'-monitor', 'none'", runner)
        for forbidden in ('pmemsave', 'session.memory(', "'-qmp'", 'socket.socket'):
            self.assertNotIn(forbidden, runner)
        make = (ROOT / 'Makefile').read_text()
        self.assertIn('QEMU_MEMORY ?= 64M', make)
        self.assertIn('DATA_PROFILE=large QEMU_MEMORY=128M', make)


if __name__ == '__main__':
    unittest.main()
