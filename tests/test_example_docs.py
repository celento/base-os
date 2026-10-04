"""Compact in-kernel sample generation matches the host DocStats fixture."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from make_stats_fixture import document


class ExampleDocumentTests(unittest.TestCase):
    def test_exact_original_sample_and_capacity(self):
        compiler = shutil.which('gcc') or shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            source = directory / 'sample.c'
            source.write_text('''#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "example_docs.h"
static unsigned polls;
void platform_poll(void){polls++;}
int main(void){
    char text[65536];memset(text,42,sizeof text);
    assert(!example_stats_document(text,56811));
    for(unsigned i=0;i<sizeof text;i++)assert(text[i]==42);
    unsigned n=example_stats_document(text,sizeof text);
    assert(n==56812&&polls==29);
    for(unsigned i=n;i<sizeof text;i++)assert(text[i]==42);
    assert(fwrite(text,1,n,stdout)==n);
    return 0;
}
''')
            binary = directory / 'sample'
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'src'), str(ROOT / 'src/example_docs.c'),
                            str(source), '-o', str(binary)], check=True)
            self.assertEqual(subprocess.check_output([str(binary)]), document())


if __name__ == '__main__':
    unittest.main()
