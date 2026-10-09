import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import two_box_match
from test_soak_oracle_evidence import analyzed_pair


class RelayReceipts(unittest.TestCase):
    def test_linux_alternative_needs_its_own_build_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness, meta = analyzed_pair(root)
            meta['machines']['client'] = 'Linux'
            (root / 'client-build.json').write_text('{}')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertFalse(two_box_match.analyze_match(harness, root, meta)['passed'])

    def test_each_relay_only_peer_needs_selected_route_and_same_session_offer(self):
        for missing in ('none', 'offer', 'selected', 'session', 'client', 'host', 'contradiction'):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                harness, meta = analyzed_pair(root)
                meta.update(path='relay', session_id='session-one')
                for peer in ('host', 'client'):
                    lines = [f'[net-ice] session {"other" if missing == "session" else "session-one"} join_mode=ice',
                             '[net-ice] selected candidate=relay connection=12',
                             f'[net-route] RouteAllowed route={"direct" if missing == peer else "relay"} allowed=1 connection=12']
                    if missing == 'selected': lines.pop(1)
                    if missing == 'contradiction': lines.append('[net-route] RouteAllowed route=direct allowed=1 connection=13')
                    (root / peer / 'stdout.log').write_text('\n'.join(lines))
                if missing != 'offer':
                    (root / 'service.log').write_text('INFO relay_offer_issued ' + json.dumps(dict(session_id='session-one',
                        match_id='match-one', provider='coturn', generation=1, expires_at=1600, server_count=1)))
                with contextlib.redirect_stdout(io.StringIO()):
                    result = two_box_match.analyze_match(harness, root, meta)
                self.assertEqual(result['passed'], missing == 'none', result['relay'])

    def test_directory_mint_receipt_contains_only_public_evidence(self):
        from session_directory import session_directory as directory
        fields = dict(connection_protocol=1, name='unit', activity='test', scene='test', mode='pvp', peer_count=2, seats_free=1,
            game_version='1', build_id='a', network_protocol_version=1, lockstep_codec_version=1, controller_frame_version=1,
            match_config_hash='a'*64, session_identity_hash='b'*64, module_manifest_hash='c'*64,
            listen_port=41010, listen_addrs=['127.0.0.1'], join_mode='ice')
        store = directory.SessionDirectory(300, 5, turn_config=dict(backend='coturn', static_auth_secret='NEVER_LOG_SECRET',
                                                                  relay_urls=['turn:relay.example:3478?transport=udp']))
        registered = store.register(fields, '127.0.0.1', 10, '0123456789abcdef')
        with patch.object(directory.time, 'time', return_value=1000), self.assertLogs(directory.LOGGER, level='INFO') as logged:
            offer = store.mint_ice_servers(registered['session_id'], dict(token=registered['token'], match_id='match-one', ttl=600),
                                           '0123456789abcdef', 11)
        line = logged.output[0]
        self.assertNotIn('NEVER_LOG_SECRET', line)
        self.assertNotIn(registered['token'], line)
        self.assertNotIn(offer['iceServers'][0]['credential'], line)
        receipt = json.loads(line.split('relay_offer_issued ', 1)[1])
        self.assertEqual(receipt, dict(session_id=registered['session_id'], match_id='match-one', provider='coturn',
                                       generation=1, expires_at=1600, server_count=1))

    def test_direct_transport_cannot_pass_a_directory_relay_arm(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            harness, meta = analyzed_pair(root)
            meta.update(path='directory-relay', session_id='session-one')
            with contextlib.redirect_stdout(io.StringIO()):
                result = two_box_match.analyze_match(harness, root, meta)
        self.assertFalse(result['passed'], result)


if __name__ == '__main__':
    unittest.main()
