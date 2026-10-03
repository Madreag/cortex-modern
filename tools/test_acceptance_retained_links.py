"""A retained control-file hard link is copied as bytes, never as a filesystem link."""
import io
from pathlib import Path
import tarfile
import unittest
from unittest.mock import patch

import world_mod_cross as world


class KeptBytes(io.BytesIO):
    def close(self): pass


def archive_with_link(target):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w') as archive:
        member = tarfile.TarInfo('./payload.json'); data = b'{"native":true}\n'; member.size = len(data)
        archive.addfile(member, io.BytesIO(data))
        link = tarfile.TarInfo('./payload.retained.json'); link.type = tarfile.LNKTYPE; link.linkname = target
        archive.addfile(link)
    return output.getvalue()


class RetainedLinks(unittest.TestCase):
    def test_streamed_internal_link_becomes_second_complete_copy(self):
        files = {}
        def opened(path, mode='rb', *args, **kwargs):
            if mode == 'xb':
                self.assertNotIn(path.name, files)
                files[path.name] = KeptBytes()
                return files[path.name]
            return io.BytesIO(files[path.name].getvalue())
        with patch.object(Path, 'mkdir'), patch.object(Path, 'exists', return_value=False), patch.object(Path, 'open', opened):
            with tarfile.open(fileobj=io.BytesIO(archive_with_link('./payload.json')), mode='r|') as archive:
                self.assertEqual(world.extract_preserved(archive, Path('D:/mx/virtual-link-test')), 2)
        self.assertEqual(files['payload.json'].getvalue(), files['payload.retained.json'].getvalue())

    def test_link_cannot_read_outside_or_unarchived_files(self):
        for target in ('../secret.json', '/secret.json', './unarchived.json'):
            with self.subTest(target=target), patch.object(Path, 'mkdir'), patch.object(Path, 'exists', return_value=False), \
                    patch.object(Path, 'open', return_value=KeptBytes()):
                with tarfile.open(fileobj=io.BytesIO(archive_with_link(target)), mode='r|') as archive:
                    with self.assertRaises(ValueError): world.extract_preserved(archive, Path('D:/mx/virtual-link-test'))


if __name__ == '__main__': unittest.main()
