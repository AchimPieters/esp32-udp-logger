#!/usr/bin/env python3
import argparse
import socket
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

from zeroconf import Zeroconf, ServiceBrowser, ServiceInfo

SERVICE_TYPE = "_esp32udplog._udp.local."
DEFAULT_RX_PORT = 9998
DEFAULT_TX_PORT = 9999

@dataclass
class Device:
    name: str
    host: str
    ip: str
    port: int

class _Listener:
    def __init__(self):
        self.devices: Dict[str, Device] = {}

    def add_service(self, zc: Zeroconf, service_type: str, name: str) -> None:
        info = zc.get_service_info(service_type, name, timeout=1500)
        dev = _info_to_device(info, name)
        if dev:
            self.devices[dev.name] = dev

    def update_service(self, zc: Zeroconf, service_type: str, name: str) -> None:
        self.add_service(zc, service_type, name)

    def remove_service(self, zc: Zeroconf, service_type: str, name: str) -> None:
        pass

def _info_to_device(info: Optional[ServiceInfo], fqdn: str) -> Optional[Device]:
    if not info:
        return None
    instance = fqdn.split("._esp32udplog._udp.local.")[0]
    port = int(info.port) if info.port else DEFAULT_RX_PORT

    ip = ""
    for addr in (info.parsed_addresses() or []):
        if ":" not in addr:  # prefer IPv4
            ip = addr
            break

    host = (info.server or "").rstrip(".")
    if not ip:
        return None

    return Device(name=instance, host=host if host else instance + ".local", ip=ip, port=port)

def discover(timeout_s: float = 2.5) -> List[Device]:
    zc = Zeroconf()
    listener = _Listener()
    browser = ServiceBrowser(zc, SERVICE_TYPE, listener)
    try:
        time.sleep(timeout_s)
        time.sleep(0.25)
        devices = list(listener.devices.values())
        devices.sort(key=lambda d: d.name.lower())
        return devices
    finally:
        browser.cancel()
        zc.close()

def pick_device(devices: List[Device]) -> Device:
    if not devices:
        raise SystemExit("No devices found. (mDNS blocked? component not advertising _esp32udplog._udp?)")
    for i, d in enumerate(devices, start=1):
        print(f"{i:2d}) {d.name:24s}  ip={d.ip:15s}  rx_port={d.port}")
    while True:
        sel = input(f"Select device [1-{len(devices)}]: ").strip()
        if sel.isdigit():
            n = int(sel)
            if 1 <= n <= len(devices):
                return devices[n - 1]
        print("Invalid selection.")

def send_udp_cmd(ip: str, port: int, cmd: str, expect_reply: bool = True, reply_timeout_s: float = 1.0) -> str:
    cmd_bytes = cmd.encode("utf-8", errors="replace")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(reply_timeout_s)
        s.bind(("", 0))
        try:
            s.sendto(cmd_bytes, (ip, port))
            if not expect_reply:
                return ""
            try:
                data, _ = s.recvfrom(2048)
                return data.decode("utf-8", errors="replace")
            except socket.timeout:
                return ""
        except OSError as e:
            raise SystemExit(f"Network error sending command to {ip}:{port}: {e}") from e

def get_local_ip_for_target(target_ip: str) -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((target_ip, 9))
            return s.getsockname()[0]
    except OSError as e:
        raise SystemExit(f"Failed to determine local IP for {target_ip}: {e}") from e

def listen_logs(port: int) -> None:
    print(f"Listening for UDP logs on 0.0.0.0:{port} (Ctrl+C to stop)")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("", port))
        while True:
            data, _ = s.recvfrom(65535)
            txt = data.decode("utf-8", errors="replace")
            sys.stdout.write(txt)
            if not txt.endswith("\n"):
                sys.stdout.write("\n")
            sys.stdout.flush()

def main() -> None:
    ap = argparse.ArgumentParser(prog="esp32-udp-logger-cli")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="List devices discovered via mDNS service _esp32udplog._udp")

    p_pick = sub.add_parser("pick", help="Interactive picker + action menu")
    p_pick.add_argument("--tx-port", type=int, default=DEFAULT_TX_PORT, help="Your PC receive port")

    p_bind = sub.add_parser("bind", help="Bind a device to unicast logs to your PC")
    p_bind.add_argument("device", help="Device instance name (e.g. esp32-udp-logger-7A3F) or 'pick'")
    p_bind.add_argument("--pc-ip", default="", help="Your PC IP (auto if omitted)")
    p_bind.add_argument("--tx-port", type=int, default=DEFAULT_TX_PORT, help="Your PC receive port")

    for name, helptext in [
        ("unbind", "Revert device to broadcast mode"),
        ("status", "Ask device status"),
        ("broadcast-on", "Enable broadcast sending on device"),
        ("broadcast-off", "Disable broadcast sending on device"),
    ]:
        p = sub.add_parser(name, help=helptext)
        p.add_argument("device")

    p_listen = sub.add_parser("listen", help="Listen for UDP logs")
    p_listen.add_argument("--port", type=int, default=DEFAULT_TX_PORT)

    args = ap.parse_args()

    if args.cmd == "listen":
        listen_logs(args.port)
        return

    devices = discover()

    if args.cmd == "list":
        if not devices:
            print("No devices found.")
            return
        for d in devices:
            print(f"{d.name}\tip={d.ip}\trx_port={d.port}")
        return

    def resolve(name: str) -> Device:
        if name == "pick":
            return pick_device(devices)
        for d in devices:
            if d.name == name:
                return d
        for d in devices:
            if d.name.lower() == name.lower():
                return d
        raise SystemExit(f"Device not found: {name}. Run: {sys.argv[0]} list")

    if args.cmd == "pick":
        d = pick_device(devices)
        print(f"Selected {d.name} (ip={d.ip}, rx_port={d.port})")
        print("Actions:")
        print("  1) bind to me")
        print("  2) status")
        print("  3) unbind")
        print("  4) broadcast off")
        print("  5) broadcast on")
        while True:
            a = input("Choose [1-5]: ").strip()
            if a in {"1","2","3","4","5"}:
                break

        if a == "1":
            pc_ip = get_local_ip_for_target(d.ip)
            r = send_udp_cmd(d.ip, d.port, f"bind {pc_ip} {args.tx_port}")
            print(r or "OK (no reply)")
        elif a == "2":
            print(send_udp_cmd(d.ip, d.port, "status") or "(no reply)")
        elif a == "3":
            print(send_udp_cmd(d.ip, d.port, "unbind") or "OK (no reply)")
        elif a == "4":
            print(send_udp_cmd(d.ip, d.port, "broadcast off") or "OK (no reply)")
        elif a == "5":
            print(send_udp_cmd(d.ip, d.port, "broadcast on") or "OK (no reply)")
        return

    d = resolve(getattr(args, "device"))

    if args.cmd == "bind":
        pc_ip = args.pc_ip.strip() or get_local_ip_for_target(d.ip)
        r = send_udp_cmd(d.ip, d.port, f"bind {pc_ip} {args.tx_port}")
        print(r or "OK (no reply)")
        return

    if args.cmd == "unbind":
        print(send_udp_cmd(d.ip, d.port, "unbind") or "OK (no reply)")
        return

    if args.cmd == "status":
        print(send_udp_cmd(d.ip, d.port, "status") or "(no reply)")
        return

    if args.cmd == "broadcast-on":
        print(send_udp_cmd(d.ip, d.port, "broadcast on") or "OK (no reply)")
        return

    if args.cmd == "broadcast-off":
        print(send_udp_cmd(d.ip, d.port, "broadcast off") or "OK (no reply)")
        return

if __name__ == "__main__":
    main()
