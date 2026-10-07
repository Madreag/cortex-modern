"""A clean refresh ignores only text line endings and refuses actual worktree changes."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_inventory_oracle_evidence import INVENTORY


class WindowsRefresh(unittest.TestCase):
    def test_r03_git_checkout_handles_crlf_without_hiding_content_changes(self):
        path=INVENTORY/'refresh_windows.py'
        self.assertTrue(path.is_file(),'both refresh scripts need a Git-native refresh path')
        spec=importlib.util.spec_from_file_location('refresh_windows_unit',path)
        refresh=importlib.util.module_from_spec(spec);spec.loader.exec_module(refresh)
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            def git(*args):return subprocess.check_output(['git','-C',str(root),*args],stderr=subprocess.DEVNULL,text=True).strip()
            git('init');git('config','user.name','Test');git('config','user.email','test@example.invalid');git('config','core.autocrlf','false')
            target=root/'sample.txt';target.write_bytes(b'one\ntwo\n');git('add','.');git('commit','-m','Fixture')
            target.write_bytes(b'one\r\ntwo\r\n')
            clean=refresh.scan_tree(root)
            self.assertEqual(clean['differing_files'],[])
            self.assertEqual(clean['line_ending_only'],['sample.txt'])
            target.write_bytes(b'one\r\nCHANGED\r\n')
            self.assertEqual(refresh.scan_tree(root)['differing_files'],['sample.txt'])
            self.assertFalse(refresh.normalized_equal(b'\0a\nb',b'\0a\r\nb'))
            git('add','sample.txt')
            self.assertEqual(refresh.scan_tree(root)['differing_files'],['sample.txt'])
        for name in ('ally_refresh_exe.sh','laptop_refresh_exe.sh'):
            text=(INVENTORY.parent/name).read_text()
            self.assertIn('refresh_windows.py',text)
            self.assertNotIn('laptop_build.sh',text)


if __name__=='__main__':unittest.main()
