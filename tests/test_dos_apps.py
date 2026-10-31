"""The bundled DOS programs: launchers in provision.c match tools/dos_apps.py."""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import dos_apps  # noqa: E402


def launchers():
    text = (ROOT / 'src/lib/provision.c').read_text()
    table = text[text.index('dos_app_launchers[]'):]
    table = table[:table.index('};')]
    return re.findall(r'\{"([^"]+)","([^"]+)"\}', table)


class DosAppTests(unittest.TestCase):
    def test_launchers_match_manifest(self):
        expected = [(app['title'] + '.dos', f'{dos_apps.BIN}/{app["name"]}.BAT') for app in dos_apps.APPS]
        self.assertEqual(launchers(), expected)

    def test_names_are_short(self):
        for app in dos_apps.APPS:
            self.assertRegex(app['name'], r'^[A-Z0-9]{1,8}$')
            self.assertTrue(app['license'])
            path, commit = app['package']
            self.assertRegex(commit, r'^[0-9a-f]{40}$', path)

    def test_batch_file(self):
        app = dos_apps.APPS[1]
        self.assertEqual(dos_apps.batch(app), '@ECHO OFF\r\nCD \\NETHACK\r\nNETHACK.EXE\r\nCD \\\r\n')


if __name__ == '__main__':
    unittest.main()
