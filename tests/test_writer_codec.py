"""Bounded Writer format tests, plus independent Pandoc RTF interoperability."""
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def native_model(path):
    data = path.read_bytes()
    magic, version, flags, length, reserved = struct.unpack_from('<4sHHII', data)
    assert (magic, version, flags, reserved) == (b'BWR1', 1, 0, 0)
    assert length <= 32768 and len(data) == 18 + 3 * length
    return (data[16:16 + length], data[16 + length:17 + 2 * length],
            data[17 + 2 * length:])


def rtf_semantics(data):
    """Small independent token/state reader for the supported emitted subset.

    This is not an RTF import feature. It checks the standard meaning of every
    emitted byte, including whitespace, paragraph alignment, size and EOF state.
    Pandoc below independently verifies text and visible inline styles.
    """
    source = data.decode('ascii')
    assert source.startswith('{\\rtf1') and source.endswith('}')
    state = {'style': 0, 'alignment': 0, 'size': 24, 'skip': False}
    stack, characters = [], []
    end = None
    position = 0

    def emit(character):
        if not state['skip']:
            characters.append((character, state['style'], state['alignment'], state['size']))

    while position < len(source):
        c = source[position]
        position += 1
        if c == '{':
            stack.append(state.copy())
        elif c == '}':
            assert stack, 'unbalanced close group'
            if len(stack) == 1:
                end = state.copy()
            state = stack.pop()
        elif c in '\r\n':
            continue
        elif c != '\\':
            emit(c)
        else:
            assert position < len(source)
            c = source[position]
            if c in '\\{}':
                position += 1
                emit(c)
                continue
            word = re.match(r'([A-Za-z]+)(-?\d+)? ?', source[position:])
            assert word, f'unsupported RTF token at {position}'
            position += word.end()
            control = word[1]
            argument = int(word[2]) if word[2] is not None else 1
            if control == 'fonttbl':
                state['skip'] = True
            elif state['skip']:
                continue
            elif control in ('b', 'i', 'ul'):
                bit = {'b': 1, 'i': 2, 'ul': 4}[control]
                state['style'] = state['style'] | bit if argument else state['style'] & ~bit
            elif control in ('ql', 'qc', 'qr'):
                state['alignment'] = {'ql': 0, 'qc': 1, 'qr': 2}[control]
            elif control == 'pard':
                state['alignment'] = 0
            elif control == 'plain':
                state['style'], state['size'] = 0, 24
            elif control == 'fs':
                state['size'] = argument
            elif control == 'tab':
                emit('\t')
            elif control == 'par':
                emit('\n')
            else:
                assert control in ('rtf', 'ansi', 'ansicpg', 'deff', 'f'), control
    assert not stack and end is not None
    return characters, end


def inline_characters(inlines, style=0):
    result = []
    for node in inlines:
        tag = node['t']
        if tag == 'Str':
            result.extend((c, style) for c in node['c'])
        elif tag in ('Space', 'SoftBreak', 'LineBreak'):
            result.append((' ', style))
        elif tag in ('Strong', 'Emph', 'Underline'):
            bit = {'Strong': 1, 'Emph': 2, 'Underline': 4}[tag]
            result.extend(inline_characters(node['c'], style | bit))
        else:
            raise AssertionError(f'Unexpected Pandoc inline: {node}')
    return result


class WriterCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('A host C compiler is required for Writer codec tests')
        cls.temp = tempfile.TemporaryDirectory(prefix='baseos-writer-codec-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = pathlib.Path(cls.temp.name)
        executable = cls.directory / 'writer_codec_test'
        subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                        str(ROOT / 'tests/writer_codec_test.c'),
                        str(ROOT / 'src/writer_codec.c'), '-o', str(executable)], check=True)
        env = dict(os.environ)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
        cls.host_result = subprocess.run([str(executable), str(cls.directory)], check=True,
                                         text=True, capture_output=True, env=env)

    def test_host_bounds_and_atomicity(self):
        self.assertIn('Writer codec tests passed', self.host_result.stdout)
        self.assertIn('Maximum alternating-paragraph RTF: 901218 bytes', self.host_result.stdout)

    def test_canonical_native_fixture(self):
        text, style, paragraph = native_model(self.directory / 'styled.bwr')
        self.assertEqual(text, (self.directory / 'styled.txt').read_bytes())
        self.assertEqual(style[:4], bytes([1] * 4))
        self.assertEqual(style[-1], 5)
        for position, value in enumerate(paragraph):
            if position and text[position - 1] != 10:
                self.assertEqual(value, 0)
        self.assertEqual(paragraph[-1], 6)

    def test_rtf_exact_text_all_states_alignment_sizes_and_empty_paragraphs(self):
        for name in ('styled', 'empty', 'transitions', 'ascii'):
            with self.subTest(name=name):
                text, style, paragraph = native_model(self.directory / f'{name}.bwr')
                actual, insertion = rtf_semantics((self.directory / f'{name}.rtf').read_bytes())
                expected = []
                current_paragraph = paragraph[0]
                for position, character in enumerate(text):
                    if not position or text[position - 1] == 10:
                        current_paragraph = paragraph[position]
                    expected.append((chr(character), style[position], current_paragraph & 3,
                                     36 if current_paragraph & 4 else 24))
                if not text or text[-1] == 10:
                    current_paragraph = paragraph[-1]
                self.assertEqual(actual, expected)
                self.assertEqual(insertion['style'], style[-1])
                self.assertEqual(insertion['alignment'], current_paragraph & 3)
                self.assertEqual(insertion['size'], 36 if current_paragraph & 4 else 24)

    def pandoc(self, name):
        reader = shutil.which('pandoc')
        if not reader:
            self.skipTest('Pandoc is not installed; independent RTF interoperability not run')
        result = subprocess.run([reader, '--from=rtf', '--to=json',
                                 str(self.directory / f'{name}.rtf')],
                                check=True, capture_output=True, text=True)
        return json.loads(result.stdout)

    def test_independent_pandoc_text_and_inline_styles(self):
        document = self.pandoc('styled')
        self.assertEqual([block['t'] for block in document['blocks']], ['Para'] * 4)
        paragraphs = [inline_characters(block['c']) for block in document['blocks']]
        self.assertEqual([''.join(c for c, _ in paragraph) for paragraph in paragraphs], [
            'Bold plain Italic Underline All', 'Centered heading', 'Right aligned',
            'Left \\ { } 123 TAB'])
        text, style, _ = native_model(self.directory / 'styled.bwr')
        first_length = text.index(10)
        self.assertEqual(paragraphs[0], list(zip(text[:first_length].decode('ascii'), style[:first_length])))
        # Pandoc deliberately normalizes tabs to spaces and drops empty paras.
        for paragraph in paragraphs[1:]:
            self.assertTrue(all(style == 0 for _, style in paragraph))
        self.assertEqual(self.pandoc('empty')['blocks'], [])

    def test_independent_pandoc_all_64_inline_transitions(self):
        document = self.pandoc('transitions')
        self.assertEqual(len(document['blocks']), 1)
        text, style, _ = native_model(self.directory / 'transitions.bwr')
        self.assertEqual(inline_characters(document['blocks'][0]['c']),
                         list(zip(text.decode('ascii'), style[:-1])))


if __name__ == '__main__':
    unittest.main()
