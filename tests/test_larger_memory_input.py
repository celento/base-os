"""Host/build/preparation checks for the opt-in gate. Never starts QEMU."""
from contextlib import redirect_stderr
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import larger_memory_input_test as gate
import init_data
import volume


class LargerMemoryInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='larger-memory-host-')
        cls.directory = Path(cls.temp.name)
        cls.font = gate.desktop.VisibleText(ROOT / 'src/font.h')
        cls.binaries = {}
        for variant in (1, 2):
            wrapper = cls.directory / f'variant-{variant}.c'
            wrapper.write_text(f'#define LARGER_MEMORY_VARIANT {variant}\n#include ' +
                               json.dumps(str(ROOT / 'tests/larger_memory_app.c')) + '\n')
            output = wrapper.with_suffix('.bex')
            subprocess.run([sys.executable, str(ROOT / 'tools/build_app.py'), str(wrapper), str(output),
                '--format', 'bex2', '--workspace-bytes', str(gate.WORKSPACE), '--stack-bytes', '65536',
                '--required-abi-minor', '1', '--elf-output', str(wrapper.with_suffix('.elf'))], check=True,
                capture_output=True, text=True)
            cls.binaries[variant] = output.read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def args(self, directory, **changes):
        values = dict(build=directory / 'build', work=directory / 'work', expected_revision='fixture',
                      build_log=directory / 'build.log', memory_mib=256, profile='both',
                      prepare_only=True, run_prepared=False, qemu_slot_held=False, timeout=3)
        values.update(changes)
        return SimpleNamespace(**values)

    def modules(self):
        return dict(init_data=init_data, volume=volume,
                    layout=SimpleNamespace(constants=lambda: {'DISK_SECTORS': 4}),
                    update_image=SimpleNamespace(install_kernel=lambda *args: None),
                    platform_evidence=SimpleNamespace(PlatformSession=lambda *args, **kwargs:
                        self.fail('A host/preparation test tried to start QEMU')))

    def pixels(self, lines):
        pixels = np.full((650, 900, 3), 32, dtype=np.uint8)
        for index, line in enumerate(lines):
            glyphs = self.font.template(line)
            region = pixels[15 + index * 20:33 + index * 20, 17:17 + glyphs.shape[1]]
            region[glyphs] = 230
        return pixels

    def test_two_valid_separately_declared_sdk_clients(self):
        self.assertNotEqual(self.binaries[1], self.binaries[2])
        for data in self.binaries.values():
            plan = gate.executable_plan(data)
            self.assertEqual(plan['workspace_bytes'], 3145728)
            self.assertEqual(plan['stack_bytes'], 65536)
            self.assertEqual(plan['mapped_pages'], 785)
            self.assertEqual(plan['owned_pages'], 787)
            self.assertEqual(plan['data_bytes'], 0)

    def test_full_byte_oracles_are_distinct_and_pinned(self):
        self.assertEqual(gate.checksum(1), 3738106053)
        self.assertEqual(gate.checksum(2), 3564015813)
        with self.assertRaises(ValueError):
            gate.checksum(3)

    def test_every_page_has_a_distinct_cross_variant_namespaced_header(self):
        # Independent serialized record oracle, including all three MiB.
        headers = []
        for variant in (1, 2):
            tags = [(page ^ ((variant * 2654435769) % (1 << 32))).to_bytes(4, 'little')
                    for page in range(gate.WORKSPACE // 4096)]
            self.assertEqual(len(tags), 768)
            self.assertEqual(len(set(tags)), 768)
            self.assertNotEqual(tags[:256], tags[256:512])
            self.assertNotEqual(tags[256:512], tags[512:])
            headers.append(set(tags))
        self.assertFalse(headers[0] & headers[1])

    def test_real_concurrent_client_algorithms_and_fresh_zero_reuse(self):
        compiler = shutil.which('clang') or 'cc'
        flags = [compiler, '-std=c11', '-D_XOPEN_SOURCE=700', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-pthread', '-I', str(ROOT / 'sdk')]
        objects = []
        for variant in (1, 2):
            obj = self.directory / f'host-{variant}.o';objects.append(str(obj))
            subprocess.run(flags + ['-include', str(ROOT / 'tests/larger_memory_app_shim.h'),
                f'-DLARGER_MEMORY_VARIANT={variant}', f'-Dmain=larger_memory_{variant}_main',
                '-c', str(ROOT / 'tests/larger_memory_app.c'), '-o', str(obj)], check=True)
        executable = self.directory / 'larger-memory-host'
        subprocess.run(flags + [str(ROOT / 'tests/larger_memory_app_host.c'), *objects, '-o', str(executable)], check=True)
        result = subprocess.run([str(executable), str(self.directory)], check=True, capture_output=True, text=True,
                                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        self.assertIn('fill barrier', result.stdout)
        counts = dict(zip(gate.SDK_FIELDS, (785, 787, 2, 1024, 4096, 2522, 3)))
        for variant, generation in ((1, 1), (2, 1), (2, 2)):
            for round_number in (1, 2):
                client = dict(variant=variant, generation=generation, task=variant + 1, round=round_number)
                actual = (self.directory / f'v{variant}-g{generation}-r{round_number}.txt').read_bytes()
                self.assertEqual(actual, gate.expected_report(client, counts))

    def test_exact_numeric_glyph_decoding_without_pool_guess(self):
        # Deliberately synthetic host values; no expected guest E820 total.
        counts = dict(zip(gate.SDK_FIELDS, (785, 787, 2, 1024, 8192, 6602, 3)))
        pixels = self.pixels([label + str(counts[key]) for key, label in zip(gate.SDK_FIELDS, gate.SDK_LABELS)])
        for key, label in zip(gate.SDK_FIELDS, gate.SDK_LABELS):
            self.assertEqual(gate.read_decimal(self.font, pixels, label), counts[key])
        with self.assertRaises(AssertionError):
            gate.read_decimal(self.font, pixels, 'Unknown: ')
        ambiguous = self.pixels(['Pool free: 123', 'Pool free: 124'])
        with self.assertRaises(AssertionError):
            gate.read_decimal(self.font, ambiguous, 'Pool free: ')
        # A damaged final digit must not be accepted as the shorter prefix 12.
        damaged = self.pixels(['Pool free: 123'])
        template = self.font.template('3');y, x = np.argwhere(template)[0]
        damaged[15 + y, 17 + len('Pool free: 12') * 8 + x] = 32
        with self.assertRaises(AssertionError):
            gate.read_decimal(self.font, damaged, 'Pool free: ')

    def test_actual_serial_baseline_is_recorded_without_synthetic_total(self):
        self.assertEqual(gate.serial_baseline('Owned pages total=8192 allocated=0 high_water=0\n'),
                         dict(total_pages=8192, allocated_pages=0, high_water_pages=0))
        for text in ('', 'Owned pages total=4096 allocated=1 high_water=1\n',
                     'Owned pages total=1 allocated=0 high_water=0\n' * 2):
            with self.assertRaises(AssertionError):
                gate.serial_baseline(text)

    def test_explicit_optional_256_mode_and_existing_disk_formats(self):
        base = ['build', '--expected-revision', 'abc', '--build-log', 'build.log', '--work', 'out', '--prepare-only']
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            gate.parse_args(base)
        for profile in ('default', 'large', 'both'):
            args = gate.parse_args(base + ['--memory-mib', '256', '--disk-profile', profile])
            self.assertEqual(args.profile, profile)
            self.assertEqual(args.memory_mib, 256)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            gate.parse_args(base + ['--memory-mib', '128'])

    def test_prepare_only_main_cannot_enter_guest_execution(self):
        args = self.args(self.directory)
        with patch.object(gate.desktop, 'runtime_tools', return_value=(ROOT, self.modules())), \
             patch.object(gate, 'prepare') as prepare, \
             patch.object(gate, 'run_profile', side_effect=AssertionError('No QEMU')):
            gate.main(args)
            prepare.assert_called_once()

    def test_missing_slot_is_rejected_before_loading_any_runtime(self):
        args = self.args(self.directory, prepare_only=False, run_prepared=True)
        with patch.object(gate.desktop, 'runtime_tools', side_effect=AssertionError('Not admitted')):
            with self.assertRaisesRegex(ValueError, 'qemu-slot-held'):
                gate.main(args)

    def test_real_prepare_builds_media_without_qemu_and_preserves_frozen_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.args(Path(temporary));args.build.mkdir()
            (args.build / 'boot.bin').write_bytes(bytes(512));(args.build / 'kernel.bin').write_bytes(b'fixture')
            original_popen = subprocess.Popen
            def allowed_builds_only(command, *rest, **kwargs):
                self.assertNotIn('qemu', str(command[0]).lower())
                return original_popen(command, *rest, **kwargs)
            with patch.object(gate.desktop, 'source_provenance', return_value={'host_fixture': True}), \
                 patch('subprocess.Popen', side_effect=allowed_builds_only):
                report = gate.prepare(args, ROOT, self.modules())
            self.assertEqual(report['status'], gate.PREPARED)
            self.assertFalse(report['passed'])
            self.assertEqual(set(report['profiles']), {'default', 'large'})
            for profile, item in report['profiles'].items():
                self.assertEqual(item['ram_mib'], 256)
                self.assertEqual(item['disk_format'], profile)
                result = gate.desktop.validate_stopped_files(args.work / (profile + '.img'), report, {}, volume)
                self.assertEqual(len(result), 4)
                nodes = volume.load((args.work / (profile + '.img')).read_bytes())[2]
                for variant in (1, 2):
                    name = report['plans'][str(variant)]['name']
                    self.assertEqual(nodes[volume.resolve(nodes, '/Programs/' + name)]['data'], self.binaries[variant])
            self.assertNotIn('report.json', report['prepared_files'])
            with self.assertRaisesRegex(ValueError, 'fresh empty'):
                gate.prepare(args, ROOT, self.modules())

    def test_failed_preparation_keeps_error_and_compiler_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.args(Path(temporary))
            failed = subprocess.CompletedProcess(['compiler'], 19, 'compiler stdout\n', 'compiler stderr\n')
            with patch.object(gate.desktop, 'source_provenance', return_value={}), \
                 patch.object(gate.subprocess, 'run', return_value=failed):
                with self.assertRaises(subprocess.CalledProcessError):
                    gate.prepare(args, ROOT, self.modules())
            report = json.loads((args.work / 'report.json').read_text())
            self.assertEqual(report['status'], 'preparation failed')
            self.assertIn('19', report['error'])
            self.assertEqual((args.work / 'larger-a.log').read_text(), 'compiler stdout\ncompiler stderr\n')

    def test_admission_refusal_preserves_prepared_report_and_records_sidecar(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.args(Path(temporary), prepare_only=False, run_prepared=True, qemu_slot_held=True)
            args.work.mkdir();report_path = args.work / 'report.json'
            report_path.write_text(json.dumps(dict(status=gate.PREPARED, gate_sources={}, profiles={'default': {}, 'large': {}})))
            original = report_path.read_bytes()
            with patch.object(gate.desktop, 'runtime_tools', return_value=(ROOT, self.modules())), \
                 patch.object(gate, 'source_identity', return_value={'changed': {}}), \
                 patch.object(gate, 'run_profile', side_effect=AssertionError('No QEMU')):
                for index in (1, 2):
                    with self.assertRaisesRegex(ValueError, 'Gate sources changed'):
                        gate.main(args)
                    self.assertEqual(report_path.read_bytes(), original)
                    sidecar = json.loads((args.work / f'admission-refused-{index:03d}.json').read_text())
                    self.assertEqual(sidecar['status'], 'admission refused; no guest started')
                    self.assertIn('Gate sources changed', sidecar['error'])

    def test_disk_profile_admission_rejects_narrowing_broadening_and_substitution(self):
        for prepared, requested in ((('default', 'large'), 'default'),
                                    (('default', 'large'), 'large'), (('default',), 'both'),
                                    (('large',), 'both'), (('default',), 'large'), (('large',), 'default')):
            with self.subTest(prepared=prepared, requested=requested), tempfile.TemporaryDirectory() as temporary:
                args = self.args(Path(temporary), prepare_only=False, run_prepared=True,
                                 qemu_slot_held=True, profile=requested)
                args.work.mkdir();path = args.work / 'report.json'
                path.write_text(json.dumps(dict(status=gate.PREPARED, profiles={name: {} for name in prepared})))
                original = path.read_bytes()
                with patch.object(gate.desktop, 'runtime_tools', return_value=(ROOT, self.modules())), \
                     patch.object(gate, 'run_profile', side_effect=AssertionError('No QEMU')):
                    with self.assertRaisesRegex(ValueError, 'disk-profile set differs'):
                        gate.main(args)
                self.assertEqual(path.read_bytes(), original)
                sidecar = json.loads((args.work / 'admission-refused-001.json').read_text())
                self.assertIn('disk-profile set differs', sidecar['error'])

    def test_sdk_counts_follow_actual_baseline_and_current_live_owners(self):
        plans = {'1': gate.executable_plan(self.binaries[1]), '2': gate.executable_plan(self.binaries[2])}
        clients = [dict(variant=1, generation=1, task=2, round=0),
                   dict(variant=2, generation=1, task=3, round=0)]
        # Synthetic baseline tests arithmetic without predicting firmware RAM.
        item = {};baseline = dict(total_pages=8192, allocated_pages=0, high_water_pages=0)
        counts = dict(zip(gate.SDK_FIELDS, (785, 787, 2, 1024, 8192, 6602, 3)))
        gate.record_live_clients(item, clients, plans, 'coexist')
        self.assertEqual(item['live_owned_pages'], 1590)
        gate.check_memory_counts(counts, plans['1'], baseline, item['live_owned_pages'])
        for bad in (dict(counts, pool_total_pages=4096), dict(counts, pool_free_pages=6603),
                    dict(counts, mapped_pages=786), dict(counts, policy_pages=786), dict(counts, policy_pages=2048)):
            with self.assertRaises(AssertionError):
                gate.check_memory_counts(bad, plans['1'], baseline, item['live_owned_pages'])
        gate.record_live_clients(item, clients[:1], plans, 'closed-second')
        self.assertEqual(item['live_owned_pages'], 803)
        gate.check_memory_counts(dict(counts, pool_free_pages=7389), plans['1'], baseline, 803)

    def test_started_report_rerun_refusal_preserves_original_acceptance(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.args(Path(temporary), prepare_only=False, run_prepared=True, qemu_slot_held=True)
            args.work.mkdir();path = args.work / 'report.json'
            path.write_text('{"status":"passed","passed":true}\n');original = path.read_bytes()
            with patch.object(gate.desktop, 'runtime_tools', return_value=(ROOT, self.modules())):
                with self.assertRaisesRegex(ValueError, 'Never reuse'):
                    gate.main(args)
            self.assertEqual(path.read_bytes(), original)
            sidecar = json.loads((args.work / 'admission-refused-001.json').read_text())
            self.assertIn('Never reuse', sidecar['error'])

    def test_counter_focus_misses_keep_unique_screenshots_and_errors(self):
        class Session:
            def __init__(self, directory):
                self.events = [];self.captures = [];self.keys = []
                self.process = SimpleNamespace(poll=lambda: None)
                self.log = directory / 'serial.log';self.log.write_text('DESKTOP-READY\n')
            def frame(self, label): self.captures.append(label)
            def key(self, key, delay): self.keys.append(key)
        with tempfile.TemporaryDirectory() as temporary:
            session = Session(Path(temporary));item = {}
            with patch.object(gate.desktop, 'wait_counter', side_effect=[AssertionError('not foreground'), {'value': 4}]):
                self.assertEqual(gate.focus_counter(session, None, item, 10), {'value': 4})
            with patch.object(gate.desktop, 'wait_counter', side_effect=[AssertionError('later miss'), {'value': 5}]):
                gate.focus_counter(session, None, item, 10)
            self.assertEqual(session.captures, ['counter-focus-1-timeout', 'counter-focus-3-timeout'])
            self.assertEqual([event['error'] for event in session.events],
                             ["AssertionError('not foreground')", "AssertionError('later miss')"])
            self.assertEqual(session.keys, ['ctrl-tab', 'ctrl-tab'])

    def test_generations_keep_relaunch_reports_distinct_even_with_reused_slot(self):
        first = dict(variant=2, generation=1, task=3, round=1)
        replacement = dict(first, generation=2)
        self.assertNotEqual(gate.report_path(first), gate.report_path(replacement))
        self.assertEqual(gate.identity(replacement), 'variant 2 generation 2 task 3')


if __name__ == '__main__':
    unittest.main()
