# esp32-udp-logger CLI (macOS / Linux / Windows)

This is a cross-platform helper tool to:
- discover ESP32 devices via mDNS (`_esp32udplog._udp.local`)
- select one device
- send commands: `bind`, `unbind`, `status`, `broadcast on/off`
- listen for UDP logs

## Install

Python 3.9+ required.

```bash
python -m pip install zeroconf
```

## Usage

List devices:
```bash
python esp32_udp_logger_cli.py list
```

Interactive:
```bash
python esp32_udp_logger_cli.py pick
```

Listen logs:
```bash
python esp32_udp_logger_cli.py listen --port 9999
```

Bind a device (unicast logs to your PC):
```bash
python esp32_udp_logger_cli.py bind esp32-udp-logger-7A3F
```

Status:
```bash
python esp32_udp_logger_cli.py status esp32-udp-logger-7A3F
```

Broadcast off/on:
```bash
python esp32_udp_logger_cli.py broadcast-off esp32-udp-logger-7A3F
python esp32_udp_logger_cli.py broadcast-on  esp32-udp-logger-7A3F
```

## Windows firewall note

mDNS discovery can be blocked by firewall rules.
If `list` shows nothing:
- allow Python on Private networks, or
- temporarily disable firewall to test, or
- use the device IP directly (you can still send UDP commands if you know the IP).
