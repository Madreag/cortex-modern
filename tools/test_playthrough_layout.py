"""Check the authored Players headings against their actual column controls."""
import configparser
from pathlib import Path
import unittest


class PlayersHeadings(unittest.TestCase):
    def test_each_heading_starts_over_its_column(self):
        controls = configparser.ConfigParser(interpolation=None, comment_prefixes=("//", ";", "#"))
        controls.read(Path(__file__).resolve().parents[1] / "Data/Base.rte/GUIs/MainMenuSubMenuGUI.ini")
        for heading, row, text in (("LabelHostSeatHeader", "LabelHostSeatName0", "Player"),
                                   ("LabelHostSeatTypeHeader", "ComboHostSeatType0", "Type"),
                                   ("LabelHostSeatTeamHeader", "ComboHostSeatTeam0", "Team"),
                                   ("LabelHostSeatDelayHeader", "LabelHostSeatDelay0", "Delay"),
                                   ("LabelHostSeatStateHeader", "LabelHostSeatState0", "State")):
            with self.subTest(column=text):
                self.assertIn(heading, controls)
                self.assertEqual(controls[heading]["Text"], text)
                self.assertEqual(int(controls[heading]["X"]), int(controls[row]["X"]))
                self.assertLessEqual(int(controls[heading]["Width"]), int(controls[row]["Width"]))


if __name__ == "__main__":
    unittest.main()
