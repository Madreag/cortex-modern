"""Content identity and scratch-copy refusal levers."""
from copy import deepcopy
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest

import acceptance_mod as mod


class ModuleIdentity(unittest.TestCase):
    def setUp(self):
        retained = os.environ.get("CC_ACCEPTANCE_TEST_ROOT")
        if retained:
            self.root = Path(tempfile.mkdtemp(prefix="mod-test-", dir=retained))
        else:
            self.temp = tempfile.TemporaryDirectory()
            self.addCleanup(self.temp.cleanup)
            self.root = Path(self.temp.name)
        self.source = self.root / "original" / "VoidWanderers.rte"
        self.source.mkdir(parents=True)
        (self.source / "z.lua").write_bytes(b"return 42\n")
        (self.source / "Index.ini").write_bytes(b"DataModule\n")
        self.archive, self.receipt = self.root / "module.tar", self.root / "manifest.json"
        self.destination = self.root / "copy" / self.source.name

    def test_sorted_digest_covers_every_file(self):
        value = mod.manifest(self.source)
        expected = "".join(f"{mod.sha256(self.source / name)}  {name}\n" for name in ("Index.ini", "z.lua"))
        self.assertEqual(value["tree_sha256"], hashlib.sha256(expected.encode()).hexdigest())
        self.assertEqual(value["file_count"], 2)

    def test_install_and_all_four_hashes(self):
        reference = mod.pack(self.source, self.archive, self.receipt)
        installed = mod.install(self.archive, self.receipt, self.destination)
        self.assertEqual(reference, installed)
        self.assertEqual(mod.install(self.archive, self.receipt, self.destination), reference)
        self.assertEqual(mod.equal_manifests(dict.fromkeys(("pc", "edith", "mac", "linux"), reference)), reference["tree_sha256"])

    def test_changed_copy_is_refused_and_restored(self):
        original = mod.pack(self.source, self.archive, self.receipt)
        mod.install(self.archive, self.receipt, self.destination)
        change = self.root / "mutation.json"
        result = mod.alter_one_byte(self.destination, "z.lua", self.root, change)
        self.assertEqual(result["files_changed"], 1)
        with self.assertRaisesRegex(ValueError, "differs before engine"):
            mod.equal_manifests(dict(pc=original, edith=original, mac=original, linux=mod.manifest(self.destination)))
        with self.assertRaisesRegex(ValueError, "no file overwritten"):
            mod.install(self.archive, self.receipt, self.destination)
        self.assertEqual(mod.restore_one_byte(change), original)
        self.assertEqual(mod.manifest(self.source), original)

    def test_mutation_cannot_leave_scratch(self):
        with self.assertRaisesRegex(ValueError, "scratch-only"):
            mod.alter_one_byte(self.source, "Index.ini", self.root / "copy", self.root / "mutation.json")

    def test_restore_refuses_a_second_change(self):
        mod.pack(self.source, self.archive, self.receipt)
        mod.install(self.archive, self.receipt, self.destination)
        receipt = self.root / "mutation.json"
        mod.alter_one_byte(self.destination, "z.lua", self.root, receipt)
        (self.destination / "Index.ini").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed after mutation"):
            mod.restore_one_byte(receipt)

    def test_archive_traversal_never_writes_outside_destination(self):
        mod.pack(self.source, self.archive, self.receipt)
        malicious = self.root / "bad.tar"
        with tarfile.open(malicious, "x") as archive:
            entry = tarfile.TarInfo("../escaped")
            entry.size = 1
            archive.addfile(entry, io.BytesIO(b"x"))
        value = json.loads(self.receipt.read_text())
        value["files"] = [dict(path="../escaped", bytes=1, sha256=hashlib.sha256(b"x").hexdigest())]
        malicious_receipt = self.root / "bad.json"
        mod.write_json(malicious_receipt, value)
        with self.assertRaisesRegex(ValueError, "unsafe entry"):
            mod.install(malicious, malicious_receipt, self.destination)
        self.assertFalse(self.destination.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
