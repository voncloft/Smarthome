import base64
import importlib.util
from pathlib import Path
import types
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("bridge", Path(__file__).parents[1] / "broadlink_bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)
PACKET = bytes.fromhex("2600040001020304")
CODE = base64.b64encode(PACKET).decode()


class StorageError(Exception):
    pass


class Mini:
    def __init__(self, mac="001122334455", host="192.168.1.20"):
        self.mac = bytes.fromhex(mac)
        self.host = (host, 80)
        self.model = "RM4 mini"
        self.TYPE = "RM4MINI"
        self.name = "Room"
        self.is_locked = False
        self.auth = Mock(return_value=True)
        self.send_data = Mock()
        self.enter_learning = Mock()
        self.check_data = Mock(return_value=PACKET)


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.bedroom = Mini()
        self.livingroom = Mini("aabbccddeeff", "192.168.1.21")
        self.api = types.SimpleNamespace(
            remote=types.SimpleNamespace(rmmini=Mini),
            exceptions=types.SimpleNamespace(StorageError=StorageError),
            hello=Mock(return_value=self.bedroom),
            discover=Mock(return_value=[self.bedroom, self.livingroom, object(), self.bedroom]))
        self.request = {"action": "send", "host": "192.168.1.20", "id": "001122334455", "code": CODE}

    def test_targets_one_hub_without_discovery(self):
        bridge.execute(self.request, self.api)
        self.api.hello.assert_called_once_with("192.168.1.20", timeout=5)
        self.api.discover.assert_not_called()
        self.bedroom.send_data.assert_called_once_with(PACKET)
        self.livingroom.send_data.assert_not_called()

    def test_changed_ip_identity_never_sends(self):
        self.api.hello.return_value = self.livingroom
        with self.assertRaisesRegex(ValueError, "different hub"):
            bridge.execute(self.request, self.api)
        self.livingroom.auth.assert_not_called()
        self.livingroom.send_data.assert_not_called()

    def test_locked_hub_never_sends(self):
        self.bedroom.is_locked = True
        with self.assertRaisesRegex(ValueError, "Lock device"):
            bridge.execute(self.request, self.api)
        self.bedroom.send_data.assert_not_called()

    def test_invalid_or_rf_code_rejected_before_network(self):
        for code in ("bad!", base64.b64encode(b"\xb2\x00\x02\x00\x01\x02").decode(),
                     base64.b64encode(b"\x26\x00\x09\x00\x01\x02").decode()):
            with self.subTest(code=code), self.assertRaises(ValueError):
                bridge.execute({**self.request, "code": code}, self.api)
        self.api.hello.assert_not_called()

    def test_discovery_filters_and_deduplicates(self):
        result = bridge.execute({"action": "discover"}, self.api)
        self.assertEqual([x["id"] for x in result["hubs"]], ["001122334455", "aabbccddeeff"])
        self.bedroom.auth.assert_not_called()
        self.bedroom.send_data.assert_not_called()

    def test_learning_waits_for_signal_without_transmitting(self):
        self.bedroom.check_data.side_effect = [StorageError(), PACKET]
        progress = Mock()
        with patch.object(bridge.time, "sleep"):
            result = bridge.execute({**self.request, "action": "learn"}, self.api, progress)
        self.assertEqual(result["code"], CODE)
        self.bedroom.enter_learning.assert_called_once()
        self.bedroom.send_data.assert_not_called()
        progress.assert_called_once()

    def test_failed_authentication_does_not_send(self):
        self.bedroom.auth.return_value = False
        with self.assertRaisesRegex(ValueError, "authentication"):
            bridge.execute(self.request, self.api)
        self.bedroom.send_data.assert_not_called()

    def test_learning_timeout(self):
        with patch.object(bridge.time, "monotonic", side_effect=[0, 31]):
            with self.assertRaisesRegex(ValueError, "No infrared signal"):
                bridge.execute({**self.request, "action": "learn"}, self.api)
        self.bedroom.send_data.assert_not_called()

    def test_support_check_has_no_network(self):
        bridge.execute({"action": "check"}, self.api)
        self.api.hello.assert_not_called()
        self.api.discover.assert_not_called()


if __name__ == "__main__":
    unittest.main()
