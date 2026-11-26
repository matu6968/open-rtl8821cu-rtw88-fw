# `open-rtl8821cu-rtw88-fw`

This repository builds separate WLAN and Bluetooth firmware images for the
Realtek RTL8821C family. Linux rtw88 loads the WLAN image into adapter RAM.
It supports RTL8811CU and RTL8821CU USB adapters and includes experimental awake
station paths for RTL8821CE PCIe and RTL8821CS SDIO.

The RTL8811CU has been tested in station mode with open, WPA2 and WPA3 networks.
The production image passed an RTL8811CU bench test covering probe, scan,
WPA2 association and reconnect, IP traffic, and firmware logs. A short WPA3-SAE
station test also passed association, DHCP, and traffic. During that run, rtw88
reported one timeout leaving ordinary power save.

An RTL8821CU USB board with Bluetooth ROM 1 passed short BLE GATT and
encrypted Classic RFCOMM tests alongside channel 1 Wi-Fi with both
source-built images. The Bluetooth patch uses the factory controller ROM
for BLE and Classic. Ten-minute coexistence qualification, WEP, and WPA
remain open.

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

Plain `make` produces three separate files.

- WLAN firmware at `target_firmware/rtw8821c_fw.bin`
- Bluetooth ROM 1 firmware at `target_firmware/bluetooth-rom1-coex/rtl_bt/rtl8821c_fw.bin`
- Bluetooth board configuration at `target_firmware/bluetooth-rom1-coex/rtl_bt/rtl8821c_config.bin`

The WLAN JSON records its SHA-256, section layout, compiler, and instruction
audit. The Bluetooth manifest records its version, board profile, and hashes.
`make clean` removes the selected firmware outputs and intermediate files.

## Use with Linux rtw88

Back up the distribution firmware and place `rtw8821c_fw.bin` in the
`rtw88/` subdirectory of your system's firmware search path. If the driver
loads from an initramfs, regenerate it with the replacement image. Replug
the adapter to load the new image. Restore the backup and replug to return
to the distribution firmware. [rtw88 firmware filename](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/realtek/rtw88/rtw8821c.c)

The optional rtw88 bundle provides kernel log decoding, magic-packet WoWLAN,
and opt-in USB deep LPS. Its build is pinned to Ubuntu
`7.0.0-43-generic`. [Build and load the optional host modules](host/rtw88/README.md)

## PCIe and SDIO station bring-up

Linux uses the same WLAN image for all three buses. In stock WLAN 24.11.0,
`0x80102014` decodes bits 13:12 of `SYS_STATUS1` (`0x00f4`), and
`0x8003d964` uses the decoded interface to select low-power settings.
The source keeps the stock interface numbering. The boot event with ID 6
records the full register value and decoded interface.

| Linux bus driver | Raw strap | Interface ID | Logical eFuse MAC offset |
| --- | --- | --- | --- |
| `rtw88_8821cu` | 1 | 1 | `0x107` |
| `rtw88_8821ce` | 2 or 3 | 3 | `0xd0` |
| `rtw88_8821cs` | 0 | 2 | `0x11a` |

The PCIe and SDIO mappings are based on the stock disassembly and await
confirmation on those devices. Both experimental paths keep the station
awake. AP, low-power wake, and Bluetooth coexistence on these buses await
qualification.

1. Start with the stock WLAN image and the normal in-kernel bus driver.
   Identify PCIe `10ec:b821` or `10ec:c821` with `lspci -nnk`, or
   SDIO `024c:c821` under `/sys/bus/sdio/devices` on a native SDIO host.
   Record the kernel version, device and firmware hash. On a kernel with
   rtw88 debugfs, write `f0 4` then `f4 4` to the target PHY's
   `rtw88/read_reg` file, reading it after each write. This records chip
   cut and strap through the driver's register-read interface.
2. Use an alternate network connection during testing. Save the current
   `/sys/module/firmware_class/parameters/path` value and point it at a
   temporary directory containing `rtw88/rtw8821c_fw.bin`. This override
   applies to new firmware loads system-wide. Rebind the target device and
   verify the firmware version in the kernel log. Disable station power
   saving with `iw dev <interface> set power_save off` before association.
3. Compare stock and source across two cold starts and a driver rebind.
   For a device that powers off with the host, use a persistent test
   directory and the `firmware_class.path` kernel command-line option
   for the source cold boots. Verify the loaded image at each boot.
   Scan, associate to WPA2, obtain DHCP, and pass bidirectional traffic
   and a reconnect on both bands. With rtw88 firmware debugging enabled
   (`debug_mask=0x10`), look for rate and TX-status C2H reports and check
   for download, calibration, and TX report timeouts.
4. Load the stock image again, verify it reconnects, and restore the
   saved firmware search path.

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
| Bluetooth configured in the factory eFuse, default WLAN image | `RTL8821CU: Bluetooth not yet supported` |
| Factory profile matches the experimental WLAN image | `RTL8821CU: experimental Bluetooth mailbox` |
| Programmed Wi-Fi-only board and Bluetooth function bit clear | `RTL8811CU: Wi-Fi only` |
| Missing or conflicting board information | `RTL8821C: variant unknown` |
| PCIe or SDIO strap | `RTL8821C: PCIe strap XX` or `RTL8821C: SDIO strap XX` |

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

## Bluetooth bring-up

The RTL8821CU exposes a Bluetooth HCI function that Linux handles through
`btusb` and `btrtl`. Its firmware path is `rtl_bt/rtl8821c_fw.bin` alongside
the separate Linux firmware configuration `rtl_bt/rtl8821c_config.bin`.
The production WLAN image continues to use `rtw88/rtw8821c_fw.bin`.
[Linux Bluetooth loader](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btrtl.c)

### Controller patch

These are source-built ROM patches. The controller still runs the
factory mask ROM for its Bluetooth stack and radio. The stock patches
contain many additional overrides. The ROM 1 container size includes a
zero-filled gap up to the stock version slot and gives little indication
of how much code was ported. [Image sizes and stock override audit](src/bt-fw/rom1-notes.md#image-sizes-and-stock-override-coverage)

The Bluetooth build compiles the MIPS16 patch in `src/bt-fw` and generates
the configuration for the tested RTL8821CU USB board (`0bda:c820`, Bluetooth
ROM 1, WLAN eFuse profile `0x04222141`). Use `make bluetooth-rom1` to build
just the Bluetooth files, or `make COEX_BOARD=0x04222141` to build them
alongside the board-specific WLAN image used in the BLE bench tests.

```sh
make bluetooth-rom1
make check-bluetooth
make COEX_BOARD=0x04222141
```

The default Bluetooth patch is `0x0001000b`. To build the earlier board
config patch, use `make bluetooth-rom1 BT_ROM1_COEX=0`, which writes
version `0x00010007` under `target_firmware/bluetooth-rom1/`. Use
`BT_ROM1_OUTPUT` to select a different Bluetooth output directory.
The build uses the existing MIPS toolchain and requires no stock firmware files.
Linux appends a generated board config
with one record at `0x00d9` containing byte `0x0f`. The patch publishes its
version, validates the record, applies it to controller RAM at `0x80120139`,
and returns to the factory ROM.

The [ROM 1 analysis](src/bt-fw/rom1-notes.md) records the Ghidra maps, the
stock callback assignments, and the Bluetooth and WLAN paths exercised
during bring-up.

### RTL8821CS UART (hardware-untested)

```sh
make bluetooth-rom1 BT_ROM1_TRANSPORT=uart
make check-bluetooth BT_ROM1_TRANSPORT=uart
```

The UART build emits patch version `0x0001000c` and these Linux firmware
files under `target_firmware/bluetooth-rom1-uart/`.

- `rtl_bt/rtl8821cs_fw.bin`
- `rtl_bt/rtl8821cs_config.bin`

Linux's Realtek H5 driver uses the separate config to select 1,500,000 baud
and RTS/CTS before uploading the patch to the controller's resident ROM.
The patch applies the UART record to controller RAM and installs a UART
recovery callback. The manifest identifies this build as
`hardware-untested`. It targets Bluetooth ROM 1 and uses resident ROM
behavior for HCI DMA, command handling, and coexistence.

For a hardware trial, first boot with the board's stock firmware and
confirm that Linux uses Realtek H5 and reports ROM 1. Temporarily select
the generated firmware and config, then cold boot. Check the kernel log for
patch version `0x0001000c`, and use `btmon` to confirm HCI Reset and
bidirectional ACL traffic before and after a suspend/resume cycle.
[UART analysis and hardware validation gaps](src/bt-fw/rom1-notes.md#rtl8821cs-uart)

### Full ROM 1 reconstruction

The full profiles are under reconstruction. Each build checks the stock
function, callback, and data inventory before it emits an epatch.

```sh
make bluetooth-rom1 BT_ROM1_PROFILE=full BT_ROM1_TRANSPORT=usb
make bluetooth-rom1 BT_ROM1_PROFILE=full BT_ROM1_TRANSPORT=uart
```

These commands currently report the outstanding coverage and stop before
producing Linux firmware files. Run `make check-bluetooth` to exercise the
reconstructed config and eFuse parsers and the UART DMA routines offline.
[ROM 1 reconstruction status](src/bt-fw/rom1-notes.md#full-rom-1-reconstruction)

### Stock reference

For reference analysis and optional offline comparison, use the stock
firmware from your distribution.

```sh
make bluetooth
stock=/lib/firmware/rtl_bt/rtl8821c_fw.bin.zst
python3 tools/bt_epatch.py inspect "$stock"
BT_STOCK_IMAGE="$stock" make check
python3 tools/bt_epatch.py extract "$stock" --rom 1 -o build/bluetooth/stock-rom1.bin
build/toolchain/bin/mipsel-elf-objdump -D -b binary -m mips16 -EL \
  --adjust-vma=0x8010a000 build/bluetooth/stock-rom1.bin
```

Use the `.bin` path for a distribution that ships uncompressed firmware.
The tool extracts raw entries by default. `extract --download` substitutes
the version word the same way Linux does, and `--config FILE` appends the
separate configuration. `pack --version VERSION --patch ROM=FILE -o OUTPUT`
wraps complete raw patch payloads.

The reference stock image has version `0x75b8f098`, project ID 10, and
uncompressed SHA-256
`dfe7ef772d078ec09490c02190a32fcd40b3eaf3b040049d8930eb80e6d5743c`.
The separate reference config has SHA-256
`292717b0610ce920f66b734c299d349ea59a4c4400044078d8fc65625f17e382`.

| Bluetooth ROM | Patch ID | Container offset | Length | Version word address |
| --- | --- | --- | --- | --- |
| 0 | 1 | `0x30` | `0x4c14` | `0x8010ec10` |
| 1 | 2 | `0x4cc0` | `0x8864` | `0x80112860` |

Both little-endian MIPS16 entries read the version from the last four bytes
at a consistent load base of `0x8010a000`. They write the high halfword first
to `0x80120056` and the low halfword to `0x80120054`. The entries then
install distinct callback sets in controller RAM. ROM 0 calls patch
functions at `0x8010e801`, `0x8010a201`, and `0x8010bc89`. ROM 1 calls
`0x8000f225` outside the patch in addition to its own patch functions.
For example, the entry in each ROM variant installs a different function at
callback slot `0x80120a10` (`0x8010c30d` in ROM 0 and `0x8010fdb1` in ROM 1).
`make bluetooth` builds the isolated version-publishing fragment and its
disassembly. The loadable ROM 1 image keeps the reference version slot at
`0x80112860` and pads the linked code to that slot with zeros. ROM 0
remains unsupported by the source download.

### Bluetooth mask ROM capture

`tools/dump_bt_rom1.py` reads the resident controller address space from
the installed Bluetooth controller. It selects the HCI device by USB port,
checks for `0bda:c820` and ROM revision 1, and reads halfwords with Realtek's
HCI command `0xfc61`. It emits a binary only after two complete reads
match. The default window covers `0x80000000` through `0x8006ffff`, the
448 KiB range recorded in earlier RTL8821C research. It includes the
ROM routines and constants referenced by the vendor patch.
[Linux HCI register reader](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btrtl.c)
[University of Lille RTL8821C ROM analysis, §6.5.1](https://pepite-depot.univ-lille.fr/LIBRE/EDSPI/2021/2021LILUI017.pdf)

```sh
mkdir -p build/bluetooth/mask-rom1
sudo python3 tools/dump_bt_rom1.py --usb-port 1-2 \
  > build/bluetooth/mask-rom1/bt-rom1-80000000.bin
```

The USB port in this example is the relay port used on the bench. Keep the
Bluetooth controller powered in BlueZ. The tool uses read commands with the
firmware that is already running.

On 2026-11-24, two reads of each range matched on ROM 1 with the vendor
patch `0x75b8f098` active. The assembled 458,752-byte image in
`build/bluetooth/mask-rom1/bt-rom1-80000000-full.bin` has SHA-256
`a3d7440d74b29d389c04cd3c4ec8af65b7cb932d7e456517587de2b51c773028`.
A separate full sweep using 32-bit reads matched byte for byte. The last
byte other than `0xff` is at `0x80065a8b`.

The ROM starts with 32-bit MIPS boot code at `0x80000000`. The coexistence
handlers below use little-endian MIPS16. Code addresses in this table
have the MIPS16 function-pointer tag cleared.

| ROM address | Observed behavior |
| --- | --- |
| `0x8000e018` | Passes the inbound message to the callback at `0x80120994`. A null callback or a zero return reaches native dispatch, where command `0x23` calls the status builder at `0x8000da90`. |
| `0x8000e538` | Initializes the mailbox queue through `0x80008854`, passing `0x801214cc` as the handle location. |
| `0x80010c04` | Reads the scoreboard and passes a pointer to the snapshot to the callback at `0x80120a14`. A null callback or a zero return reaches the ROM acknowledgement at `0xb000a0bd`. |
| `0x8000d9bc` | Queues the two mailbox data words using the handle at `0x801214cc` after the gate at `0x8000fb04` opens. The gate latches bit 29 of `0xb000a044` in the byte at `0x80120048`. |

The ROM implements both the status reply and the scoreboard acknowledgement.
These paths provide concrete starting points for tracing initialization
and explaining why the source patch's scoreboard probes got zero replies.

### Source firmware bench results

On 2026-11-24, the ROM 1 patch `0x00010000` booted with vendor WLAN 24.11.0
and then with the source WLAN coexistence image for `0x04222141`. The source pair
initialized the factory Bluetooth address over four relay cold starts.
USBmon matched all 35 WLAN chunks and all 139 Bluetooth fragments against
the generated images and configuration. Two fresh Bluetooth builds matched
byte for byte. All 20 offline tests passed for both WLAN build profiles
with the optional vendor-image comparison enabled.

| Test with both source downloads | Result |
| --- | --- |
| BLE discovery and GATT read/write with 2.4 GHz WLAN | 513 exchanges in 600.7 seconds, 620 of 620 Wi-Fi pings |
| Classic discovery, pairing and bidirectional RFCOMM with 2.4 GHz WLAN | 584 exchanges in 600.7 seconds. Wi-Fi lost its association and passed 2 of 598 pings |
| 5 GHz WLAN smoke test | Associated at 5220 MHz and passed all five pings |

Reassociation at 2.4 GHz timed out during the active Classic link. A short
repeat reproduced ping loss as the Classic link came up. The source pair
still needs Classic coexistence qualification.

### Experimental WLAN mailbox

The opt-in WLAN image queues the rtw88 BT status query (`0x61`) and WLAN
channel notification (`0x66`). Stock WLAN routines `0x80035a30` and
`0x80035ab8` translate them to sideband messages `0x23` and `0x11`.
The matching TX/RX transport appears in the WLAN ROM at `0x80007dec` and
`0x80007ef8`. The receive routine checks `0xb8003418` and reads its data
window at `0xb8003410`, from the ROM literal at `0x800151c0`. Incoming
sideband status replies and events (`0x23` and `0x27`) produce rtw88
C2H `0x09`, with source 1 and 2 respectively, as in stock dispatch at
`0x80035fe8` and `0x80035ff4`. The implementation keeps a queued report
while the WLAN C2H transport is busy.

Stock TDMA command `0x60` reaches `0x800359ec`. It copies five bytes and
defers enabled settings to the scheduler at `0x80035ed8`. Both paths use
the handler pointer at `0x802073a8`. The ROM seeds it with `0x800079e9` and
the stock WLAN patch replaces it with `0x8003bb21`. Source WLAN implements
the measured antenna-arbitration schedule for the shared-antenna two-slot
mode on the test board. rtw88 supplies the coexistence tables and grant
policy. Other TDMA modes and BT calibration coordination remain open.
The WLAN reference image has SHA-256
`2ef409bc418549fcf294061dd0cae1fc22fd9da79b60524950b25de18732f3f0`.
[Pinned rtw88 source manifest](host/rtw88/source.json)

Build the test WLAN image after capturing the actual board profile. The
four bytes of `COEX_BOARD`, most significant first, are the WLAN chip cut
from bits 15:12 of register `0x00f0` and the raw logical Wi-Fi eFuse bytes
at `0xca`, `0xc1`, and `0xc3`, in that order. Linux's stock WLAN probe reads
the physical eFuse using register `0x0030`. The source WLAN firmware also
exposes its expanded map at `g_efuse_logical` through the DBG6/DBG7 reader,
with the address in the matching firmware manifest.

```sh
make COEX_BOARD="$profile"
```

Use `target_firmware/coex-$profile/rtw8821c_fw.bin` on the test host. The
firmware additionally checks the factory Bluetooth board marker and the
live Bluetooth function-enable bit before using the sideband registers.
The default WLAN image uses the Wi-Fi-only configuration.

On 2026-11-22, two clean builds matched for each WLAN configuration and the
Bluetooth fragment. All 17 offline tests passed with the stock Bluetooth image.

| Artifact | SHA-256 |
| --- | --- |
| 2026-11-22 default WLAN image used for the hardware baseline | `d4575ddfebcbb6f5e6287ea6aac4be32a151c37dd342343ecbc8a2addfd1dcf3` |
| Experimental WLAN image with synthetic profile `0x04212001` | `90fccec40def9a6b2b177170bcbea5ec3ac28c735daf2155842ce6b61516617f` |
| Bluetooth version fragment ELF | `e227fb2f5e64cd84a34404cfa248275b09150453761aa78dad53533809893583` |

The RTL8811CU regression check on 2026-11-22 passed scan, 5 GHz association,
and five directed ping packets. A second radio saw the temporary channel 6
AP beacon. After the AP was removed, the adapter rejoined its original
network and passed the same traffic check.

The RTL8821CU bench adapter on 2026-11-24 switched from its USB storage
personality (`0bda:1a2b`) to the composite device `0bda:c820`. Bluetooth
bound to interfaces 0 and 1, and rtw88 bound to interface 2. Three controlled
relay cold starts initialized the installed 2026-11-22 WLAN image and the
stock Bluetooth `0x75b8f098` patch on ROM 1. The Bluetooth download contained
34,916 bytes of patch data and 10 bytes of board configuration.

On channel 1, the adapter discovered nearby BLE advertisements while
passing 15 of 15 Wi-Fi pings. Classic inquiry also started while Wi-Fi
passed 15 of 15 pings. A Classic peer and connected Bluetooth data transfer
remain untested. An earlier plug on a different USB port reported
`-EPROTO` on both functions shortly before USB disconnect. The WLAN
checksum message followed the first USB errors. The fault did not recur
during the three relay starts.

A USBmon and HCI capture of a subsequent cold start recovered all 512
physical Wi-Fi eFuse bytes from the driver's read traffic. The WLAN cut is
4 and logical eFuse bytes `0xca`, `0xc1`, and `0xc3` are `0x22`, `0x21`,
and `0x41`, giving `COEX_BOARD=0x04222141`. The 139 captured Bluetooth
download fragments matched the stock ROM 1 payload and configuration
byte for byte. The experimental WLAN image for that profile passed all
19 offline tests.

In the hardware comparison, both stock WLAN 24.11.0 and the experimental
WLAN image ran with the same stock Bluetooth patch. The USB capture matched
every WLAN download chunk against the respective image and captured the
experimental mailbox boot message. Wi-Fi was associated on channel 1
with power saving disabled. Both runs passed 15 of 15 WLAN pings during
BLE discovery and another 15 of 15 during Classic inquiry. Classic pairing
and connected Bluetooth traffic remain untested.

In the first comparison, stock WLAN emitted 15 `0x09` Bluetooth status
reports, while the experimental image emitted zero. The firmware had read
the receive payload at `0xb8003420` and discarded the resulting command
byte. After correcting the receive address, two cold starts of the
experimental image produced three query replies each, plus five and
seven asynchronous reports respectively. rtw88 sent seven queries each
time, including queries before the first captured reply. Both runs
passed all 30 WLAN pings during the BLE and Classic discovery tests. The
test restored the installed firmware and verified both USB functions.

### ROM 1 coexistence investigation

With source WLAN and the vendor Bluetooth patch, WLAN read `0x8002` from
its scoreboard at register `0x00aa`. The source Bluetooth version patch
left it at `0x0000`. rtw88 uses bit 1 to detect a running Bluetooth
controller. In the dual-source captures it sent zero `0x61` status queries
and received zero `0x09` reports.

The ROM 1 reference patch writes the Bluetooth scoreboard through
`0xb000a0bc`. Its initialization at `0x8010af21` installs the mailbox
handler at callback slot `0x80120994` and the scoreboard handler at
`0x80120a14`. The mailbox handler dispatches query `0x23` and channel
notification `0x11`. The status builder at `0x8010bb55` sends a six-byte
payload using the resident ROM entry `0x8000d9bd`. The channel handler at
`0x8010b005` builds a 10-byte channel map at `0x80124110`. The scoreboard
handler acknowledges an incoming interrupt by setting bit 7 of the byte
at `0xb000a0bd`.

A test patch wrote `0x00024000` to `0xb000a0bc` at startup. WLAN then read
`0x0002` and sent seven queries with zero replies. During BLE discovery the
controller timed out on HCI command `0x202d` and Linux reset the USB device.
A second test installed source callbacks in both slots and reproduced the
timeout with seven queries and zero replies. A third test also set bit 31
at `0x80120144`, as the stock initialization does. Both Bluetooth links
worked in that run, the scoreboard stayed at `0x0000`, and Classic Wi-Fi
again passed only two pings. The vendor Bluetooth control with the same
WLAN build returned three query replies and nine unsolicited reports.
Its BLE connection and Classic discovery returned errors before the
connected traffic tests, so the Classic coexistence comparison is
inconclusive.

The ROM calls `0x8010a001` at `0x8000ea4e`, creates the mailbox queue at
`0x8000eb88`, then invokes the callback in `0x801209bc` at `0x8000ec1a`.
The stock patch registers `0x801123b9` there. Its call to `0x8010ab2d`
writes `0x80024000` to the scoreboard. Bit 7 at `0x80120147` gates the
ROM's scoreboard interrupt path at `0x80001296`.

```sh
make check-bluetooth
```

This builds the experimental coexistence patch (`0x0001000b`) in
`target_firmware/bluetooth-rom1-coex/`. The patch enables the ROM's scoreboard
interrupt and coexistence timers. Its late-init callback publishes the stock
startup scoreboard word when the queue, status timer, and mailbox gate are
ready. The ROM calls its timer initializer at `0x80063038` at startup and
again at `0x8001cce2` during controller reset. This resets the status-change
guards. The source mailbox callback enables those guards on the next WLAN
query, then lets the ROM dispatch the query and build the reply. The resident
50 ms poll at `0x80063114` handles status changes. The source patch also
applies the WLAN component of the adaptive frequency hopping map on channel
notices and reuses the ROM's channel-map installer.

The first late-init probe (`0x00010004`) published `0x8002` at the WLAN
scoreboard. Both WLAN builds received status-query replies. The captures
contained zero unsolicited Bluetooth status reports with that probe.
The four short runs used Wi-Fi channel 1 and an Intel Bluetooth peer.
USBmon matched all 35 WLAN chunks and 139 Bluetooth fragments on each boot.

| Wi-Fi / Bluetooth | BLE | Classic |
| --- | --- | --- |
| Stock / stock | GATT passed, 35 of 35 Wi-Fi pings | RFCOMM passed, 19 of 29 Wi-Fi pings. Wi-Fi traffic recovered during the run |
| Stock / `0x00010004` | GATT passed, 26 of 26 Wi-Fi pings | Inquiry returned other devices and timed out finding the Intel peer |
| Source / stock | The link failed to establish with HCI reason `0x3e` | BlueZ reported discovery in progress |
| Source / `0x00010004` | Intel advertisement timed out | Intel inquiry timed out |

The source pair also passed a five-ping Wi-Fi smoke test at 5220 MHz. The
relay runs restored the installed firmware and Bluetooth peer settings.

Probe `0x00010005` started the resident status timer and set both guards in
the late-init callback. It still produced query replies without unsolicited
reports with stock WLAN. The source-WLAN run reproduced the BLE link setup
failure seen with stock Bluetooth.

Probe `0x00010006` initialized the factory Bluetooth address and published
the active scoreboard in both WLAN trials. Both captures matched all 35
WLAN chunks and all 139 Bluetooth fragments. With stock WLAN, the probe
sent seven query replies and six unsolicited reports. It completed 18 BLE
GATT and 20 Classic RFCOMM exchanges in separate 20-second tests. WLAN
delivered all 31 pings in each test.

With source WLAN, the same probe sent six query replies and one unsolicited
report. BLE discovered the peer but failed to establish the link with HCI
reason `0x3e`, which also occurred with stock Bluetooth and source WLAN.
Classic discovery then returned `InProgress` from BlueZ. Two further cold
boots ran Classic before BLE with source WLAN, once with stock Bluetooth
and once with probe `0x00010006`. Both inquiries timed out finding the
Intel peer. Both BLE link attempts failed after discovery. The 5 GHz WLAN
smoke tests passed. The bench restored the installed firmware after each
run. The dual-source 2.4 GHz qualification remains open.

### ROM 1 configuration trials

Bluetooth `0x00010008` read back `0x0f` at `0x80120139` with both WLAN
builds. Each boot matched all 35 WLAN chunks, all 139 Bluetooth fragments,
and the factory Bluetooth address. Tests used Wi-Fi channel 1 and an Intel
Bluetooth peer.

| Wi-Fi / Bluetooth | Classic RFCOMM | BLE GATT |
| --- | --- | --- |
| Stock / `0x00010008` | 21 exchanges, 33 of 33 Wi-Fi pings | 18 exchanges, 42 of 42 Wi-Fi pings |
| Source / `0x00010008` | Peer discovery timed out, 26 of 26 Wi-Fi pings | Link establishment failed with HCI reason `0x3e`, 8 of 8 Wi-Fi pings |
| Stock / stock | Peer discovery timed out, 26 of 26 Wi-Fi pings | 17 exchanges, 35 of 35 Wi-Fi pings |
| Source / stock | Peer discovery timed out, 26 of 26 Wi-Fi pings | Link establishment failed with HCI reason `0x3e`, 6 of 6 Wi-Fi pings |

Both source-WLAN combinations passed a five-ping smoke test at 5220 MHz.
Both WLAN builds received TDMA command `0x60`. Only the stock WLAN build
in this trial applied the beacon-relative slot transitions and MAC
priority control. The bench restored installed firmware after all four
trials.

### ROM 1 channel-map trials

With stock WLAN on channel 1, Bluetooth `0x0001000a` completed 18 BLE
GATT exchanges with 28 of 28 Wi-Fi pings, and 20 Classic RFCOMM exchanges
with 29 of 29 Wi-Fi pings. Each traffic test ran for 20 seconds.

The shared-antenna driver sent clear-channel notices during normal
2.4 GHz operation. Directed H2C `0x66` tests applied channel 1 and 6
notices and then cleared them. Both WLAN builds produced the expected
WLAN and live Bluetooth maps in controller RAM.

| Notice | Excluded Bluetooth channel indices |
| --- | --- |
| Channel 1, span 24 | 0 through 22 |
| Channel 6, span 20 | 24 through 45 |
| Clear | None |

With both source images on channel 1, BLE again failed with HCI reason
`0x3e`. BlueZ reported discovery in progress when the subsequent Classic
test began. On 5220 MHz, the same pair passed the five-ping Wi-Fi smoke
test and then completed 12 GATT exchanges while Wi-Fi stayed associated.
USBmon matched both firmware downloads on each boot. The bench restored
the installed images after each run.

### BLE coexistence on channel 1

With baseline source WLAN, the Bluetooth controller accepted the BLE
connection command and reported an initial connection. It disconnected
248 ms later with HCI reason `0x3e`. During passing stock WLAN runs,
the antenna-arbitration bits at `0x06cc` switched in both directions
in each beacon cycle. Baseline source WLAN held them clear.

The opt-in WLAN build schedules those transitions for the observed
shared-antenna two-slot commands. The schedule alone passed two short
channel 1 cold-boot tests and still dropped the first connection on a
third. Adjusting the schedule phase to match the stock measurements also
left intermittent first-connection failures.

Bluetooth `0x0001000b` uses its ROM's coexistence setup flag to signal
the start of LE initiation. Source WLAN holds the antenna arbitration
in the setup state for five seconds, then resumes the rtw88 schedule.
USBmon recorded that setup event approximately two milliseconds after
the first LE Create Connection command. The register capture confirmed
the five-second hold.

With both source images, BLE established on the first connection attempt
on three independent cold boots. Wi-Fi stayed on channel 1 with power
saving disabled.

| Cold boot | BLE GATT exchanges | Concurrent Wi-Fi pings |
| --- | --- | --- |
| 1 | 12 in 13.1 seconds | 56 of 56 |
| 2 | 12 in 13.1 seconds | 48 of 48 |
| 3 | 104 in 120.9 seconds | 256 of 256 |

USBmon matched all 35 WLAN chunks and all 139 Bluetooth fragments on each
boot. The trials recorded no USB errors, HCI timeouts, or link-establishment
failures. Two independent clean builds reproduced both bench images, and
each offline configuration passed 23 tests. The bench restored the
installed firmware. Ten-minute coexistence qualification remains open.

### Classic coexistence on channel 1

On 2026-11-26, the RTL8821CU ROM 1 USB patch `0x0001000b` completed
Classic inquiry, fresh pairing, and encrypted bidirectional RFCOMM
against an Intel AX210 peer. The board used the same source WLAN
coexistence image as the BLE channel 1 tests.

| Realtek WLAN condition | Classic RFCOMM | WLAN gateway pings |
| --- | --- | --- |
| Disconnected | 118 exchanges in 120.7 seconds | — |
| Associated at 2412 MHz, power saving off | 114 exchanges in 120.9 seconds | 261 of 263 during the trial |

USBmon matched all 139 Bluetooth fragments and 35 WLAN chunks. The HCI
trace confirmed BR/EDR encryption and local disconnects after both runs.
Wi-Fi stayed associated until test cleanup. The bench restored the
installed firmware and the peer's discovery and pairing settings.
Classic audio profiles and the ten-minute coexistence runs remain untested
on this version.

### Bench procedure

1. Start with distribution firmware for both functions. Before plugging in
   the dongle, start `sudo btmon -w stock.btsnoop` and a full-length capture
   on the corresponding `usbmonN` interface. Use `lsusb -t` and the HCI
   controller's sysfs device link to identify the Wi-Fi and Bluetooth
   interfaces of the same USB device. Record the kernel log, firmware
   hashes, WLAN board profile, and the Bluetooth ROM reported by `btrtl`.
2. Prepare a temporary firmware directory with the Bluetooth files under
   `rtl_bt/` and the WLAN file at `rtw88/rtw8821c_fw.bin`. Save the current
   `/sys/module/firmware_class/parameters/path` and use it to direct new
   firmware loads to the temporary directory. Run both stock images as a
   control, then test source Bluetooth with stock WLAN, source WLAN with
   stock Bluetooth, and both source images. Cold-replug for each pairing
   and check the loaded versions in the kernel log. Restore the saved
   search path and cold-replug after testing. The downloads go to RAM.
   For each boot, pass the capture, bus number, composite device address,
   and corresponding staged files to `tools/verify_bt_rom1_capture.py`.
   It verifies the WLAN chunks and indexed Bluetooth fragments and reports
   scoreboard reads, H2C commands, C2H Bluetooth status, and USB errors.
3. Verify both source downloads and the factory Bluetooth address over
   three cold plugs. On two of those boots, run ten minutes each of BLE
   discovery and GATT read/write, and Classic discovery, pairing, and
   bidirectional RFCOMM. Run directed 2.4 GHz Wi-Fi traffic with power
   saving disabled alongside each. Require at least 99% ping delivery,
   continuous association, and zero Bluetooth link drops, HCI timeouts,
   or USB resets. Capture the queries, replies, and connection-state
   reports. Finish with a 5 GHz association and traffic smoke test.

`tools/qualify_bt_peer.py` exercises a powered `0bda:c820` adapter and a
powered Intel USB Bluetooth controller on one Linux host. It uses BlueZ,
`python3-dbus`, and `python3-gi`. The bench needs an unpaired Classic peer
and an Ethernet or Wi-Fi route independent of the test adapter. With a
directed ping already running on the RTL8821CU WLAN interface, invoke
`/usr/bin/python3 tools/qualify_bt_peer.py le --seconds 600` and then
`/usr/bin/python3 tools/qualify_bt_peer.py classic --seconds 600`. The
Classic test temporarily enables discovery and pairing on the Intel peer
and removes the bonds it creates.

## License and issues

The WLAN firmware is GPL-2.0-only with [exceptions](COPYING.exceptions) to allow embedding
or using the interfaces from unmodified firmware built from source or from prebuilt binaries
inside of a driver/program that isn't under the GPL. See [COPYING](COPYING) and
[NOTICES.TXT](NOTICES.TXT) for license terms and upstream attributions.
The firmware format was reconstructed from the Realtek
`rtw8821c_fw.bin` distributed by Linux firmware. The source-built
image uses the adapter's existing boot ROM.
[Linux firmware repository](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/)

Please report bugs with the firmware hash, adapter model, and driver
version. [GitHub issues](https://github.com/matu6968/open-rtl8821cu-rtw88-fw/issues)
