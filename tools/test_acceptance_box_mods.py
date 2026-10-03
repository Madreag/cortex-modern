import unittest

from acceptance_box_mods import shell_command, validate_target


class Targets(unittest.TestCase):
    def test_saved_alias_and_lane_root(self):
        box = dict(name="edith", ssh="edith", scratch="D:/mx/test", python="python",
                   destination="D:/mx/test/engine/Data/VoidWanderers.rte")
        validate_target(box, "test")
        for key, value in (("ssh", "different"), ("scratch", "D:/mx/another"),
                           ("destination", "D:/Projects/live/Data/VoidWanderers.rte")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_target({**box, key: value}, "test")

    def test_shell_quoting_is_literal(self):
        command = shell_command({"name": "edith"}, ["python", "x's file.py", "$(no)"])
        self.assertEqual(command, "& 'python' 'x''s file.py' '$(no)'")
        command = shell_command({"name": "mac"}, ["python", "$(no)"])
        self.assertEqual(command, "python '$(no)'")


if __name__ == "__main__":
    unittest.main(verbosity=2)
