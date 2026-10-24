"""The named-peer interface finds the per-box transport kit from every place it documents, or names what is missing."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import spread_peers as spread


def kit(folder, marker):
    folder.mkdir(parents=True)
    (folder/'pool.py').write_text(f'KIT = {marker!r}\n', encoding='utf-8')
    (folder/'pool_transport.py').write_text(f'KIT = {marker!r}\n', encoding='utf-8')
    (folder/'boxes.json').write_text(json.dumps(dict(boxes=[])), encoding='utf-8')
    return folder


class KitLocatorTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.box_file = self.root/'box-file.json'
        environment = {key: value for key, value in os.environ.items() if key not in ('CORTEX_POOL_DISPATCHER', 'CORTEX_BOXES')}
        environment['CORTEX_BOXES'] = str(self.box_file)
        patcher = patch.dict(os.environ, environment, clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)
        saved = list(sys.path)
        self.addCleanup(lambda: sys.path.__setitem__(slice(None), saved))
        for name in ('pool', 'pool_transport'):
            self.addCleanup(sys.modules.pop, name, None)
            sys.modules.pop(name, None)

    def test_no_kit_anywhere_is_a_named_refusal(self):
        with self.assertRaises(spread.SpreadRefusal) as refused:
            spread.installed_pool()
        self.assertIn('per-box transport kit', str(refused.exception))
        self.assertIn('pool_transport.py', str(refused.exception))

    def test_the_box_file_names_the_kit(self):
        folder = kit(self.root/'kit', 'box-file')
        self.box_file.write_text(json.dumps(dict(schema=1, boxes=[], paths=dict(pool_dispatcher=str(folder/'pool_transport.py')))),
                                 encoding='utf-8')
        pool, transport, path = spread.installed_pool()
        self.assertEqual((pool.KIT, transport.KIT, path), ('box-file', 'box-file', (folder/'pool_transport.py').resolve()))

    def test_a_registry_beside_the_kit_names_it(self):
        folder = kit(self.root/'registry-kit', 'registry')
        pool, transport, path = spread.installed_pool(registry=folder/'boxes.json')
        self.assertEqual((pool.KIT, path), ('registry', (folder/'pool_transport.py').resolve()))

    def test_a_named_folder_without_the_transport_says_so(self):
        (self.root/'empty').mkdir()
        with self.assertRaises(spread.SpreadRefusal) as refused:
            spread.installed_pool(dispatcher=self.root/'empty'/'pool_transport.py')
        self.assertIn('has no pool_transport.py beside it', str(refused.exception))


if __name__ == '__main__':
    unittest.main()
