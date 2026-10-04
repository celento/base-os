import importlib.util
import hashlib
import json
import pathlib
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('build_info', pathlib.Path(__file__).resolve().parents[1] / 'tools/build_info.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BuildInfoTests(unittest.TestCase):
    def test_exported_identity_is_stable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            info = {'revision': 'a' * 40, 'commit_epoch': 1791060000, 'dirty': False}
            (root / 'source-info.json').write_text(json.dumps(info))
            actual = MODULE.generate(root, root / 'out')
            self.assertEqual(actual['revision'], info['revision'])
            header = root / 'out/build_info.h'
            before = header.stat().st_mtime_ns
            MODULE.generate(root, root / 'out')
            self.assertEqual(header.stat().st_mtime_ns, before)
            self.assertIn('#define BASEOS_BUILD_DIRTY 0', header.read_text())

    def test_exported_source_edits_and_deletions_are_dirty(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / 'kernel.c'
            source.write_bytes(b'original source')
            info = {'revision': 'b' * 40, 'commit_epoch': 1791060000, 'dirty': False,
                    'source_sha256': {'kernel.c': hashlib.sha256(source.read_bytes()).hexdigest()}}
            (root / 'source-info.json').write_text(json.dumps(info))
            self.assertFalse(MODULE.identity(root)['dirty'])
            MODULE.generate(root, root / 'build')
            self.assertFalse(MODULE.identity(root)['dirty'])
            source.write_bytes(b'changed source')
            self.assertTrue(MODULE.identity(root)['dirty'])
            MODULE.generate(root, root / 'build')
            self.assertIn('+changes', (root / 'build/build_info.h').read_text())
            source.write_bytes(b'original source')
            self.assertFalse(MODULE.identity(root)['dirty'])
            source.unlink()
            self.assertTrue(MODULE.identity(root)['dirty'])

    def test_git_changes_and_exact_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            def git(*args):
                return subprocess.check_output(['git', '-C', directory, *args], text=True, stderr=subprocess.DEVNULL).strip()
            git('init', '-q')
            (root / 'file').write_text('first')
            git('add', 'file')
            git('-c', 'user.name=Build Test', '-c', 'user.email=test@localhost', 'commit', '-qm', 'Fixture')
            original = MODULE.identity(root)
            self.assertEqual(original['revision'], git('rev-parse', 'HEAD'))
            self.assertFalse(original['dirty'])
            (root / 'file').write_text('changed')
            self.assertTrue(MODULE.identity(root)['dirty'])
            self.assertEqual(MODULE.identity(root)['commit_epoch'], original['commit_epoch'])

    def test_missing_metadata_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(MODULE.identity(directory)['revision'], 'unknown')
