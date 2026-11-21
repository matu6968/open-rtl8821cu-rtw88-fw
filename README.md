# `open-rtl8821cu-rtw88-fw`

This repository builds one WLAN firmware image for the Realtek RTL8811CU
and RTL8821CU USB 802.11ac adapters. Linux rtw88 loads the image into the
adapter's RAM. The build uses source code, linker scripts, and generated
padding.

The RTL8811CU has been tested in station mode with open and WPA2 networks.
The production image passed an RTL8811CU bench test covering probe, scan,
WPA2 association and reconnect, IP traffic, and firmware logs. RTL8821CU
board detection is covered by offline tests and awaits testing on hardware.
Bluetooth is unsupported. WEP, WPA, and WPA3 are untested.

## Build

Install GNU Make 4.3 or newer and Python 3.10 or newer. To build the
freestanding MIPS cross-toolchain, also install Git, a native C/C++ compiler,
Bison, Flex, Texinfo, and the GMP, MPFR, and MPC development libraries.
On Ubuntu the development packages are `libgmp-dev`, `libmpfr-dev`, and
`libmpc-dev`.

```sh
make toolchain
make
make check
```

The toolchain target downloads pinned GCC and binutils revisions. Set
`GCC_SRC` and `BINUTILS_SRC` to use local Git checkouts of those revisions,
or set `MIPS_PREFIX=/path/to/mipsel-elf` when invoking Make to use an existing
installation. The baseline was built with GCC 16.2.1 (20260815) and GNU
Binutils 2.47. `TOOLCHAIN_JOBS` sets the bootstrap's parallel job count.

The firmware is `target_firmware/rtw8821c_fw.bin`. The neighboring JSON file
records its SHA-256, section layout, compiler, and instruction audit.
`make clean` removes the firmware outputs and its intermediate files.

## Use with Linux rtw88

Back up the distribution firmware and place `rtw8821c_fw.bin` in the
`rtw88/` subdirectory of your system's firmware search path. If the driver
loads from an initramfs, regenerate it with the replacement image. Replug
the adapter to load the new image. Restore the backup and replug to return
to the distribution firmware. [rtw88 firmware filename](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/realtek/rtw88/rtw8821c.c)

The optional rtw88 bundle provides kernel log decoding, magic-packet WoWLAN,
and opt-in USB deep LPS. Its build is pinned to Ubuntu
`7.0.0-43-generic`. [Build and load the optional host modules](host/rtw88/README.md)

## Access point mode

Use `hostapd` with the `nl80211` driver and one AP interface. Linux
configures the channel, keys, client state, and beacon template. The
adapter schedules beacon transmission through its hardware queue. The
firmware keeps the AP awake and continues to handle rate control and TX
status for connected clients.

The RTL8811CU bench test used 2.4 GHz WPA2-PSK with CCMP. A separate device
associated, received a DHCP lease, and loaded a local HTTP page over Wi-Fi.

For a 2.4 GHz WPA2-PSK access point, create a `hostapd.conf` with the
following settings. Use your adapter's interface name, a unique passphrase,
and a channel permitted by your local regulatory settings.

```ini
interface=wlan0
driver=nl80211
ssid=rtl8821c-ap
hw_mode=g
channel=1
ieee80211n=1
wmm_enabled=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=<replace-with-your-passphrase>
```

Run `sudo hostapd hostapd.conf` after releasing the target interface from
any other network manager. Configure IP addressing and any routing or
DHCP service on the host.

## Firmware logs

The boot message appears once per firmware load after the driver makes
the C2H transport available. The firmware uses the factory board profile
and the Bluetooth function-enable bit to select its message.

| Board profile | Message |
| --- | --- |
| Bluetooth coexistence configured in the factory eFuse | `RTL8821CU: Bluetooth not yet supported` |
| Programmed Wi-Fi-only board and Bluetooth function bit clear | `RTL8811CU: Wi-Fi only` |
| Missing or conflicting board information | `RTL8821C: variant unknown` |

Start the USB capture before plugging in or reloading the adapter. The
decoder accepts a little-endian USBmon pcap with link type 220. Choose the
bus with `--bus` and optionally filter by device address with `--device`.

```sh
python3 tools/decode_fwlog.py capture.pcap --bus 1
```

The optional rtw88 logging patch prints the same messages as `FW:` lines
in `dmesg`. IQ calibration logs report failures. Rate tracing is enabled by
the driver's H2C `0x49` command with component bit 1 set.
[Kernel logging instructions](host/rtw88/README.md)

## License and issues

The WLAN firmware is GPL-2.0-only. See [COPYING](COPYING) and
[NOTICES.TXT](NOTICES.TXT) for license terms and upstream attributions.
The firmware format was reconstructed from the Realtek
`rtw8821c_fw.bin` distributed by Linux firmware. The source-built
image uses the adapter's existing boot ROM.
[Linux firmware repository](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/)

Please report bugs with the firmware hash, adapter model, and driver
version. [GitHub issues](https://github.com/matu6968/open-rtl8821cu-rtw88-fw/issues)
