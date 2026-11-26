#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise an RTL8821CU against the Intel adapter on the same Linux host."""

import argparse
from contextlib import contextmanager
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import time
import uuid

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

BLUEZ = 'org.bluez'
PROPS = 'org.freedesktop.DBus.Properties'
MANAGER = 'org.freedesktop.DBus.ObjectManager'
ADAPTER = 'org.bluez.Adapter1'
DEVICE = 'org.bluez.Device1'
SERVICE = 'org.bluez.GattService1'
CHAR = 'org.bluez.GattCharacteristic1'
ADVERTISEMENT = 'org.bluez.LEAdvertisement1'
AGENT = 'org.bluez.Agent1'
ROOT = '/org/rtl8821cu/bench'
SERVICE_UUID = 'aadb4471-26af-462f-92f7-2bdf217d55bf'
CHAR_UUID = 'aadb4472-26af-462f-92f7-2bdf217d55bf'
ATT_CID = 4
BDADDR_LE_PUBLIC = 1
BDADDR_LE_RANDOM = 2
# Linux socket ABI values for Python builds that omit these constants.
SOL_BLUETOOTH = getattr(socket, 'SOL_BLUETOOTH', 274)
BT_SECURITY = getattr(socket, 'BT_SECURITY', 4)
BT_SECURITY_MEDIUM = getattr(socket, 'BT_SECURITY_MEDIUM', 2)


def interface(bus, path, name):
    return dbus.Interface(bus.get_object(BLUEZ, path), name)


def properties(bus, path):
    return interface(bus, path, PROPS)


def managed(bus):
    return interface(bus, '/', MANAGER).GetManagedObjects()


def wait(predicate, duration, failure):
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.20)
    raise TimeoutError(failure)


def set_adapter_flag(props, name, value):
    def apply():
        try:
            props.Set(ADAPTER, name, value)
        except dbus.DBusException as error:
            if error.get_dbus_name() != 'org.bluez.Error.Busy':
                raise
            return False
        return True

    wait(apply, 8, f'BlueZ stayed busy setting {name}')


class Rejected(dbus.DBusException):
    _dbus_error_name = 'org.bluez.Error.Rejected'


class PropertiesObject(dbus.service.Object):
    def __init__(self, bus, path, iface, values):
        super().__init__(bus, path)
        self.object_path = dbus.ObjectPath(path)
        self.iface = iface
        self.values = values

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return self.values if iface == self.iface else {}


class Characteristic(PropertiesObject):
    def __init__(self, bus, service):
        super().__init__(bus, service + '/characteristic', CHAR, {
            'UUID': CHAR_UUID, 'Service': dbus.ObjectPath(service),
            'Flags': dbus.Array(['read', 'write'], signature='s'),
        })
        self.value = b'rtl8821cu'

    @dbus.service.method(CHAR, in_signature='a{sv}', out_signature='ay')
    def ReadValue(self, options):
        return dbus.ByteArray(self.value)

    @dbus.service.method(CHAR, in_signature='aya{sv}', out_signature='')
    def WriteValue(self, value, options):
        self.value = bytes(value)


class Application(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, ROOT)
        self.object_path = dbus.ObjectPath(ROOT)
        service = ROOT + '/service'
        self.service = PropertiesObject(bus, service, SERVICE, {
            'UUID': SERVICE_UUID, 'Primary': dbus.Boolean(True),
        })
        self.characteristic = Characteristic(bus, service)

    @dbus.service.method(MANAGER, out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return {
            item.object_path: {item.iface: item.values}
            for item in (self.service, self.characteristic)
        }


class Advertisement(PropertiesObject):
    @dbus.service.method(ADVERTISEMENT, in_signature='', out_signature='')
    def Release(self):
        pass


class PairingAgent(dbus.service.Object):
    def __init__(self, bus, allowed):
        super().__init__(bus, ROOT + '/agent')
        self.object_path = dbus.ObjectPath(ROOT + '/agent')
        self.allowed = allowed

    def check(self, device):
        if device not in self.allowed:
            raise Rejected('controller is outside the bench test')

    @dbus.service.method(AGENT, in_signature='o', out_signature='')
    def RequestAuthorization(self, device):
        self.check(device)

    @dbus.service.method(AGENT, in_signature='ou', out_signature='')
    def RequestConfirmation(self, device, passkey):
        self.check(device)

    @dbus.service.method(AGENT, in_signature='os', out_signature='')
    def AuthorizeService(self, device, uuid):
        self.check(device)

    @dbus.service.method(AGENT, in_signature='', out_signature='')
    def Cancel(self):
        pass

    @dbus.service.method(AGENT, in_signature='', out_signature='')
    def Release(self):
        pass


def find_adapters(bus):
    realtek, intel = [], []
    for path, props in managed(bus).items():
        adapter = props.get(ADAPTER, {})
        if not adapter.get('Powered'):
            continue
        usb = (Path('/sys/class/bluetooth') / Path(path).name / 'device').resolve().parent
        try:
            vendor = (usb / 'idVendor').read_text().strip()
            product = (usb / 'idProduct').read_text().strip()
        except FileNotFoundError:
            continue
        identity = str(path), str(adapter['Address'])
        if (vendor, product) == ('0bda', 'c820'):
            realtek.append(identity)
        elif vendor == '8087':
            intel.append(identity)
    if len(realtek) != 1 or len(intel) != 1:
        raise RuntimeError('expected one powered 0bda:c820 adapter and one powered Intel adapter')
    return realtek[0], intel[0]


def device_path(adapter, address):
    return adapter + '/dev_' + address.replace(':', '_')


def serve(mode):
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus(private=True)
    loop = GLib.MainLoop()
    (rtl_path, rtl), (intel_path, intel) = find_adapters(bus)

    def ready():
        print('READY', flush=True)

    def failed(error):
        print(str(error), file=sys.stderr, flush=True)
        loop.quit()

    if mode == 'serve-le':
        app = Application(bus)
        ad = Advertisement(bus, ROOT + '/advertisement', ADVERTISEMENT, {
            'Type': 'peripheral', 'ServiceUUIDs': dbus.Array([SERVICE_UUID], signature='s'),
        })
        adv = interface(bus, intel_path, 'org.bluez.LEAdvertisingManager1')

        def registered():
            adv.RegisterAdvertisement(ad.object_path, {}, reply_handler=ready, error_handler=failed)

        interface(bus, intel_path, 'org.bluez.GattManager1').RegisterApplication(
            app.object_path, {}, reply_handler=registered, error_handler=failed)
    else:
        agent = PairingAgent(bus, {device_path(rtl_path, intel), device_path(intel_path, rtl)})
        manager = interface(bus, '/org/bluez', 'org.bluez.AgentManager1')
        manager.RegisterAgent(agent.object_path, 'NoInputNoOutput')
        manager.RequestDefaultAgent(agent.object_path)
        ready()
    loop.run()


@contextmanager
def helper(mode):
    with subprocess.Popen([sys.executable, __file__, mode], text=True, stdout=subprocess.PIPE) as process:
        try:
            readable, _, _ = select.select([process.stdout], [], [], 12)
            if not readable or process.stdout.readline().strip() != 'READY':
                raise RuntimeError(f'{mode} did not initialize')
            yield
        finally:
            process.terminate()
            try:
                process.wait(3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


@contextmanager
def discover(bus, adapter_path, transport, uuid=None):
    adapter = interface(bus, adapter_path, ADAPTER)
    settings = {'Transport': transport}
    if uuid:
        settings['UUIDs'] = dbus.Array([uuid], signature='s')
    adapter.SetDiscoveryFilter(settings)
    adapter.StartDiscovery()
    try:
        yield
    finally:
        adapter.StopDiscovery()
        adapter.SetDiscoveryFilter({})


def repeat(seconds, operation, label):
    started = time.monotonic()
    count = 0
    while True:
        operation(count)
        count += 1
        elapsed = time.monotonic() - started
        if elapsed >= seconds:
            print(f'{label}: {count} exchanges in {elapsed:.1f}s', flush=True)
            return
        time.sleep(1)


def le_test(bus, seconds):
    (rtl_path, rtl), _ = find_adapters(bus)
    with helper('serve-le'):
        with discover(bus, rtl_path, 'le', SERVICE_UUID):
            peer = wait(lambda: next((path for path, props in managed(bus).items()
                        if str(path).startswith(rtl_path + '/') and
                        SERVICE_UUID in props.get(DEVICE, {}).get('UUIDs', ())), None),
                        20, 'RTL8821CU did not discover the Intel BLE advertisement')
        print('Intel BLE advertisement discovered', flush=True)
        remote = properties(bus, peer)
        address = str(remote.Get(DEVICE, 'Address'))
        address_type = (BDADDR_LE_RANDOM if remote.Get(DEVICE, 'AddressType') == 'random'
                        else BDADDR_LE_PUBLIC)
        # Bind ATT to the Realtek adapter explicitly. BlueZ's Connect also tries
        # BR/EDR profiles when both adapters belong to the same daemon.
        with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP) as att:
            att.settimeout(12)
            att.bind((rtl, 0, ATT_CID, BDADDR_LE_PUBLIC))
            att.connect((address, 0, ATT_CID, address_type))

            def request(command, response):
                att.sendall(command)
                for _ in range(8):
                    data = att.recv(517)
                    if data and data[0] == response:
                        return data[1:]
                    # The Intel-side daemon may also act as a GATT client.
                    if len(data) == 3 and data[0] == 0x02:
                        att.sendall(b'\x03\x05\x02')
                    elif len(data) >= 3 and data[0] in (0x04, 0x06, 0x08, 0x10):
                        att.sendall(b'\x01' + data[:3] + b'\x0a')
                    else:
                        raise RuntimeError(f'ATT 0x{command[0]:02x} returned {data.hex()}')
                raise RuntimeError('ATT peer did not return the requested response')

            char_type = uuid.UUID(CHAR_UUID).int.to_bytes(16, 'little')
            entry = request(b'\x08\x01\x00\xff\xff' + char_type, 0x09)
            if len(entry) != 12 or entry[0] != 11 or entry[3:] != b'rtl8821cu':
                raise RuntimeError('GATT initial read mismatch')
            handle = entry[1:3]

            def exchange(sequence):
                payload = sequence.to_bytes(4, 'little')
                request(b'\x12' + handle + payload, 0x13)
                if request(b'\x0a' + handle, 0x0b) != payload:
                    raise RuntimeError('GATT readback mismatch')

            repeat(seconds, exchange, 'BLE GATT')


def classic_test(bus, seconds):
    (rtl_path, rtl), (intel_path, intel) = find_adapters(bus)
    peer, reverse = device_path(rtl_path, intel), device_path(intel_path, rtl)
    initial = managed(bus)
    if any(initial.get(path, {}).get(DEVICE, {}).get('Paired') for path in (peer, reverse)):
        raise RuntimeError('Classic qualification requires an unpaired test peer')
    intel_props = properties(bus, intel_path)
    flags = {name: intel_props.Get(ADAPTER, name) for name in ('Discoverable', 'Pairable')}
    try:
        with helper('serve-agent'):
            set_adapter_flag(intel_props, 'Pairable', dbus.Boolean(True))
            set_adapter_flag(intel_props, 'Discoverable', dbus.Boolean(True))
            with discover(bus, rtl_path, 'bredr'):
                wait(lambda: managed(bus).get(peer, {}).get(DEVICE, {}).get('Class'),
                     25, 'RTL8821CU did not discover the Intel Classic controller')
            print('Intel Classic controller discovered', flush=True)
            with socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM) as server, \
                    socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM) as client:
                server.settimeout(12)
                client.settimeout(12)
                security = bytes((BT_SECURITY_MEDIUM, 0))
                server.setsockopt(SOL_BLUETOOTH, BT_SECURITY, security)
                client.setsockopt(SOL_BLUETOOTH, BT_SECURITY, security)
                server.bind((intel, 0))
                server.listen(1)
                client.bind((rtl, 0))
                client.connect((intel, server.getsockname()[1]))
                connected, incoming = server.accept()
                with connected:
                    connected.settimeout(8)
                    if incoming[0] != rtl:
                        raise RuntimeError('unexpected RFCOMM peer')
                    wait(lambda: properties(bus, peer).Get(DEVICE, 'Paired'),
                         5, 'authenticated RFCOMM link established without a reported bond')
                    print('Intel Classic controller paired', flush=True)

                    def exchange(sequence):
                        data = sequence.to_bytes(4, 'little')
                        client.sendall(data)
                        if connected.recv(len(data), socket.MSG_WAITALL) != data:
                            raise RuntimeError('Classic forward data mismatch')
                        connected.sendall(data[::-1])
                        if client.recv(len(data), socket.MSG_WAITALL) != data[::-1]:
                            raise RuntimeError('Classic reverse data mismatch')

                    repeat(seconds, exchange, 'Classic RFCOMM')
    finally:
        try:
            for adapter, path in ((rtl_path, peer), (intel_path, reverse)):
                if managed(bus).get(path, {}).get(DEVICE, {}).get('Paired'):
                    interface(bus, adapter, ADAPTER).RemoveDevice(path)
        finally:
            for name, value in flags.items():
                if intel_props.Get(ADAPTER, name) != value:
                    set_adapter_flag(intel_props, name, value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('le', 'classic', 'serve-le', 'serve-agent'))
    parser.add_argument('--seconds', type=int, default=600)
    args = parser.parse_args()
    if args.seconds < 0:
        parser.error('--seconds must be nonnegative')
    if args.mode.startswith('serve-'):
        serve(args.mode)
    else:
        bus = dbus.SystemBus(private=True)
        (le_test if args.mode == 'le' else classic_test)(bus, args.seconds)


if __name__ == '__main__':
    if len(sys.argv) > 1 and not sys.argv[1].startswith('serve-'):
        signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
