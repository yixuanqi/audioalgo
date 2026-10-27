import contextlib
import copy
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('workspace_manifest', Path(__file__).with_name('test_node_workspace_manifest.py'))
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)

class WorkspaceStatusTests(unittest.TestCase):
    def run_manifest(self, status, remaining, results=True):
        manifest = json.loads(checker.MANIFEST.read_text(encoding='utf-8'))
        item = manifest['requirements'][0]
        item['status'] = status
        item['remaining_clauses'] = remaining
        if not results:
            item['test_results'] = []
        with tempfile.TemporaryDirectory(dir=checker.EVIDENCE_ROOT) as directory:
            path = Path(directory) / 'manifest.json'
            path.write_text(json.dumps(manifest), encoding='utf-8')
            with patch.object(checker, 'MANIFEST', path), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return checker.main()

    def test_complete_execution_allows_empty_remaining(self):
        self.assertEqual(self.run_manifest('passed', []), 0)

    def test_partial_requires_uncovered_clauses(self):
        self.assertNotEqual(self.run_manifest('partial', []), 0)

    def test_passed_rejects_remaining_or_missing_execution(self):
        self.assertNotEqual(self.run_manifest('passed', ['still missing']), 0)
        self.assertNotEqual(self.run_manifest('passed', [], False), 0)

if __name__ == '__main__':
    unittest.main()
