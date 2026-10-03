import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import e2e_video as video


class PortClaimOwnership(unittest.TestCase):
    def test_incomplete_claim_cannot_be_stolen_during_publication(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(video, 'PORT_CLAIMS', Path(folder)):
            owner = video.PortClaim([49399])
            original = os.write
            second = []
            def paused_write(handle, data):
                with patch.object(video.os, 'write', original):
                    try:
                        claimant = video.PortClaim([49399]).__enter__()
                    except RuntimeError:
                        second.append('refused')
                    except OSError as error:
                        second.append(type(error).__name__)
                    else:
                        second.append('stolen'); claimant.__exit__(None, None, None)
                return original(handle, data)
            with patch.object(video.os, 'write', paused_write):
                with owner:
                    self.assertEqual(second, ['refused'])

    def test_release_does_not_remove_another_owners_receipt(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(video, 'PORT_CLAIMS', Path(folder)):
            owner = video.PortClaim([49399]).__enter__()
            path = Path(folder) / '49399.claim'
            path.write_text(f'{os.getpid()} successor-token\n')
            owner.__exit__(None, None, None)
            self.assertTrue(path.is_file())


if __name__ == '__main__':
    unittest.main()
