import importlib.util
import sys
import types
from pathlib import Path
from unittest import TestCase, mock


def load_cli_module():
    """Load the CLI module with a stubbed zeroconf dependency."""

    fake_zc = types.SimpleNamespace(Zeroconf=mock.Mock(), ServiceBrowser=mock.Mock(), ServiceInfo=mock.Mock())
    module_path = Path(__file__).resolve().parent / "esp32_udp_logger_cli.py"
    spec = importlib.util.spec_from_file_location("esp32_udp_logger_cli", module_path)
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict(sys.modules, {"zeroconf": fake_zc}):
        assert spec and spec.loader
        spec.loader.exec_module(module)  # type: ignore[arg-type]
    return module


class ErrorHandlingTests(TestCase):
    def setUp(self):
        self.cli = load_cli_module()

    def test_send_udp_cmd_bind_error_is_actionable(self):
        bind_error = OSError("permission denied")
        mock_socket = mock.MagicMock()
        instance = mock_socket.return_value.__enter__.return_value
        instance.bind.side_effect = bind_error

        with mock.patch("socket.socket", mock_socket):
            with self.assertRaises(SystemExit) as ctx:
                self.cli.send_udp_cmd("10.0.0.1", 1234, "status")

        self.assertIn("Failed to bind local UDP socket", str(ctx.exception))

    def test_send_udp_cmd_network_error_is_actionable(self):
        network_error = OSError("network unreachable")
        mock_socket = mock.MagicMock()
        instance = mock_socket.return_value.__enter__.return_value
        instance.sendto.side_effect = network_error

        with mock.patch("socket.socket", mock_socket):
            with self.assertRaises(SystemExit) as ctx:
                self.cli.send_udp_cmd("10.0.0.2", 4321, "status")

        self.assertIn("Network error sending command to 10.0.0.2:4321", str(ctx.exception))

    def test_listen_logs_bind_error_is_actionable(self):
        bind_error = OSError("address in use")
        mock_socket = mock.MagicMock()
        instance = mock_socket.return_value.__enter__.return_value
        instance.bind.side_effect = bind_error

        with mock.patch("socket.socket", mock_socket):
            with self.assertRaises(SystemExit) as ctx:
                self.cli.listen_logs(9000)

        self.assertIn("Failed to bind UDP listener on port 9000", str(ctx.exception))

    def test_get_local_ip_for_target_connect_error(self):
        connect_error = OSError("unreachable")
        mock_socket = mock.MagicMock()
        instance = mock_socket.return_value.__enter__.return_value
        instance.connect.side_effect = connect_error

        with mock.patch("socket.socket", mock_socket):
            with self.assertRaises(SystemExit) as ctx:
                self.cli.get_local_ip_for_target("203.0.113.10")

        self.assertIn("Failed to determine local IP for 203.0.113.10", str(ctx.exception))
