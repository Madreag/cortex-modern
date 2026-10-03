"""Unit rows of the Cloudflare relay proof: the directory's request (CF1), the match evidence (CF2, the coturn
alternative, the three-way table) and the hotspot rows' verdicts and tunnel receipt (C4). No engine, no network."""
from __future__ import annotations

import io
from email.message import Message
import json
import re
import sys
import unittest
from pathlib import Path
from unittest import mock
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parent))
from session_directory import session_directory as directory  # noqa: E402

CONFIG = {'backend': 'cloudflare', 'turn_key_id': 'unit-key-id-0001', 'api_token': 'unit-api-token-never-sent'}
SERVERS = [{'urls': ['stun:stun.cloudflare.com:3478']},
           {'urls': ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:3478?transport=tcp',
                     'turns:turn.cloudflare.com:5349?transport=tcp'],
            'username': 'unit-minted-username', 'credential': 'unit-minted-credential'}]


def answer(status: int = 201, body: dict | None = None) -> mock.MagicMock:
    response = mock.MagicMock()
    response.__enter__.return_value = response
    response.status = status
    response.read.return_value = json.dumps(body if body is not None else {'iceServers': SERVERS}).encode()
    return response


class CloudflareRequest(unittest.TestCase):
    """CF1: Cloudflare refuses urllib's default agent with 403 error 1010, so every real mint failed."""

    def mint(self, ttl: int = 600):
        provider = directory.TurnCredentialProvider(CONFIG)
        with mock.patch.object(directory, 'urlopen', return_value=answer()) as fetch:
            offer = provider.mint('match:1', ttl, 1000)
        return offer, fetch.call_args

    def test_the_request_names_the_product_and_its_version(self):
        _, call = self.mint()
        agent = call.args[0].get_header('User-agent')
        self.assertIsNotNone(agent, 'the Cloudflare request carries no User-Agent; urllib sends its default, which Cloudflare refuses')
        self.assertRegex(agent, r'^cccp-session-directory/\d+(\.\d+)*$')
        self.assertNotIn('urllib', agent.lower())

    def test_the_request_has_cloudflare_s_documented_shape_and_nothing_else(self):
        _, call = self.mint(900)
        request = call.args[0]
        self.assertEqual(request.get_method(), 'POST')
        self.assertEqual(request.full_url, 'https://rtc.live.cloudflare.com/v1/turn/keys/unit-key-id-0001/credentials/generate-ice-servers')
        self.assertEqual({name.lower() for name, _ in request.header_items()}, {'authorization', 'content-type', 'user-agent'})
        self.assertEqual(request.get_header('Authorization'), 'Bearer unit-api-token-never-sent')
        self.assertEqual(request.get_header('Content-type'), 'application/json')
        self.assertEqual(json.loads(request.data), {'ttl': 900})
        timeout = call.kwargs.get('timeout', call.args[1] if len(call.args) > 1 else None)
        self.assertIsNotNone(timeout, 'an unbounded provider call would hold the directory thread')

    def test_cloudflare_s_201_becomes_the_offer_with_its_lifetime(self):
        offer, _ = self.mint(900)
        self.assertEqual(offer['expires_at'], 1900)
        self.assertEqual([server['urls'] for server in offer['iceServers']], [server['urls'] for server in SERVERS])

    def test_a_refusal_is_sanitized_and_names_no_key(self):
        provider = directory.TurnCredentialProvider(CONFIG)
        refused = HTTPError('https://rtc.live.cloudflare.com/', 403, 'Forbidden', Message(), io.BytesIO(b'error code: 1010'))
        with mock.patch.object(directory, 'urlopen', side_effect=refused):
            with self.assertRaises(directory.TurnError) as error:
                provider.mint('match:1', 600, 1000)
        self.assertEqual((error.exception.status, error.exception.body), (502, {'error': 'relay_provider_unavailable'}))
        for secret in (CONFIG['api_token'], CONFIG['turn_key_id']):
            self.assertNotIn(secret, json.dumps(error.exception.body) + str(error.exception))


if __name__ == '__main__':
    unittest.main()
