# RTL8821CU Bluetooth ROM 1 analysis

The analysis covers USB device `0bda:c820`, Bluetooth ROM 1, and WLAN
eFuse profile `0x04222141`.

## Ghidra address map

The Bluetooth Ghidra program is `/rtl8821c/rtl8821cu-bt-rom1-stock.elf`.
It uses the `MIPS:LE:32:default` language with the MIPS16 context set at
tagged entry points. Clear bit 0 for an instruction address. Keep bit 0
when storing a MIPS16 pointer. The reset vector at `0x80000000` starts in
MIPS32.

| Region | Address | Size | SHA-256 |
| --- | --- | --- | --- |
| Controller ROM capture | `0x80000000` | `0x70000` | `a3d7440d74b29d389c04cd3c4ec8af65b7cb932d7e456517587de2b51c773028` |
| Stock epatch ROM 1 entry, raw | `0x8010a000` | `0x8864` | `50ccea46bfeb8ba3215ece78b9d4541cbbdce03a2de0ac2f54a30f211149ade6` |

ROM startup calls the downloaded entry at instruction `0x8000ea4e`,
initializes the mailbox queue at `0x8000eb88`, then calls the late hook
through slot `0x801209bc` at `0x8000ec1a`. The source linker reserves
`0x80112860` for the version and defines the start of the appended board
configuration at `0x80112864`. HCI Reset reaches the controller reset
routine at `0x8001cb04`, which calls the coexistence timer initializer at
`0x80063038`. The same initializer is called during cold startup.

## Bluetooth behavior

| Operation | Stock entry point | Source ROM 1 behavior |
| --- | --- | --- |
| Version publication | `0x8010a000` | Writes the high then low halfword to `0x80120056` and `0x80120054`. |
| Appended board data | `0x8010e614` | Validates the single board record and writes its byte to `0x80120060 + 0x00d9`. The broader stock record set, including special offset `0x1007`, is outside this board profile. |
| Mailbox and scoreboard startup | `0x8010af20`, `0x801123b8`, `0x8010ab2c` | The coexistence build enables ROM timers and IRQ dispatch, and publishes `0x80024000` when the queue and status timer are ready. |
| Command `0x23` and asynchronous `0x27` | `0x8010c68c`, `0x8010bb54` | A WLAN query rearms the ROM status guards after HCI reset. The resident status builder at `0x8000da90` and timer at `0x80063114` send reports. |
| WLAN channel notice `0x11` | `0x8010c68c`, `0x8010b004` | Replaces the WLAN component of the live AFH mask, retaining exclusions outside the old WLAN range. The ROM routine at `0x800500b4` installs the map and refreshes per-channel state and its timer. |
| Legacy LE Create Connection `0x200d` | ROM dispatch through `0x80120ad0` | Publishes the existing link-setup indication before returning to native HCI dispatch. The ROM status builder at `0x8000da90` places byte `0x80120991` at HB1 bit 2. |
| Scoreboard IRQ | `0x8010c494` | ROM `0x80010c04` acknowledges the IRQ. The stock hook also programs RF offsets `0x1a6` and `0x17a` and adjusts priority and gain. Those adjustments remain unimplemented. |
| Classic, BLE, HCI, and controller radio procedures | Resident controller ROM and stock overrides in the table below | The source builds use resident controller procedures. The stock overrides, including frame, event, and radio paths whose contracts are uncharacterized, remain open. |

The stock `0x11` handler uses `0x80124110` for the WLAN mask and
`0x80123f90` for the live 79-channel map. The source build supports a
2.4 GHz channel number or a 5 GHz harmonic already expressed as a Bluetooth
channel. It drops the old WLAN exclusion when WLAN sends a clear notice and
keeps the unused bit in the final octet clear.

The stock HCI hook in slot `0x80120ad0` also combines the WLAN mask with
the host's Set AFH Host Channel Classification command (`0x0c3f`). The
source hook forwards that command to native HCI processing. Combinations
in which the host issues a new classification after a WLAN notice still
need qualification.

## Image sizes and stock override coverage

The stock and source images below use the same ROM 1 download extent for
each transport. The version word is at the end of that extent and Linux
appends the board config after it. The source ELF places its MIPS16 code,
literal pools, and constants at the beginning and fills the gap with zeros.
The stock containers also include a 19,476-byte ROM 0 patch.

| ROM 1 target | Patch extent | Source code and constants | Source zero-filled gap | Nonzero bytes in stock patch |
| --- | ---: | ---: | ---: | ---: |
| RTL8821CU USB, coexistence build `0x0001000b` | 34,916 B | 792 B | 34,120 B | 32,972 B |
| RTL8821CS UART `0x0001000c` | 36,928 B | 292 B | 36,632 B | 34,868 B |

Source sizes are the ELF `.text` section, which includes constants.
Each extent also contains the four-byte version slot. The stock count
counts all nonzero bytes in the extracted ROM 1 entry, including data.
To reproduce the source measurement after building both variants, run
`build/toolchain/bin/mipsel-elf-size -A build/bluetooth/rom1-coex/patch.elf build/bluetooth/rom1-uart/patch.elf`.

Ghidra shows at least 72 distinct tagged patch pointers written by the
stock USB entry, its coexistence installer, and its late initializer.
The corresponding UART routines write at least 78. These counts cover
direct pointer writes in those three routines and exclude their other
callees. The USB source installs three hooks for the mailbox, HCI link
setup reporting, and late scoreboard initialization. The UART source
installs one hook for transport recovery.

| Stock path | Evidence | Coverage in the source patches |
| --- | --- | --- |
| HCI command adjustments | Slot `0x80120ad0` points to MIPS16 code at `0x80110524` (USB) or `0x80110c2c` (UART). The stock handler merges the host AFH map with WLAN exclusions on `0x0c3f` and adjusts state for LE Start Encryption `0x2019` and Classic Inquiry `0x0401`. | Those adjustments are absent. USB adds reporting for `0x200d` and both builds use resident command handling. |
| HCI DMA | UART late init at `0x80112b64` replaces the descriptor setup and the handlers in `0x80120014`, `0x80120018`, `0x8012001c`, and `0x80121330`. Its handlers process a single descriptor where the captured ROM handlers loop through rings. | Both builds use the resident DMA path. Whether CS UART needs the stock descriptor layout for reliable ACL data requires hardware validation. |
| WLAN coexistence and RF setup | Both stock builds install a separate scoreboard IRQ hook at `0x80120a14`. Stock UART late init also calls `0x8010ab48`, which publishes the scoreboard and programs RF register `0x21a`. | The USB build covers selected scoreboard, mailbox, and AFH behavior described above. UART uses the ROM defaults. |

The RTL8821CU BLE bench exercises the ROM and the USB coexistence subset
together. Stock behavior outside that tested subset still needs
qualification. The RTL8821CS ROM and hardware behavior remain unverified.

## Full ROM 1 reconstruction

The coverage inventory pins the two stock ROM 1 payload hashes and records
Ghidra function entries, tagged callback targets, installer addresses,
and byte ranges. Both payloads have a preliminary classification across
their full extent.

| Transport | Decompiled function entries | Known callback slots | Instruction bytes | Data bytes |
| --- | ---: | ---: | ---: | ---: |
| CU USB | 210 | 73 | 30,420 | 4,296 |
| CS UART | 221 | 79 | 32,092 | 4,624 |

The slot totals include the extra assignment at `0x80126a70` in the
scoreboard publisher, beyond the three initial installers counted above.
The data includes PC-relative literal pools and an identical 740-byte tail
before the loader version word. The stock builds load data from that tail
at several offsets. For example, CU instructions read through literals at
`0x8010c490`, `0x8010d1a4`, and `0x80112110`. The table contents and use
still need source mappings. Function bodies and instruction mode were
checked against the stock disassembly when the decompiler misidentified
leaf functions or decoded a literal as an instruction.
[Coverage inventory](rom1-coverage.json)

The full-profile entry uses the common stock-format multi-record parser
at CU `0x8010e614` and CS `0x8010e7bc`, including the exceptional record
offset `0x1007`. The source validates the complete input before the first
write and bounds that record to the four bytes before `0x80127b48`.
It also reimplements the factory eFuse address and RX mode setup from CU
`0x8010d500` and `0x8010d53c` and their CS counterparts at `0x8010d6a8`
and `0x8010d6e4`.

The CS DMA descriptor reset, descriptor ownership check, and three receive
interrupt routines pass a host replay of the stock register and descriptor
operations. The stock packet wrapper at `0x8010efac` forwards packets to
ROM `0x8002c7ec` and queues a message through `0x800087f8`. The queue
fields and the late initialization sequence still need verification before
installing those DMA callbacks. The existing UART recovery hook is covered
separately below.

The Ghidra exporter takes `usb` or `uart` and an output directory. Run it
against the corresponding annotated stock project and keep the function
list, instruction listing, and pseudocode under
`build/bluetooth/rom1-analysis/`. The firmware build uses the reviewed
coverage inventory to gate packaging and checks that the recorded source
symbols are present in the linked image once the inventory is complete.
[Ghidra exporter](../../tools/ghidra/ExportBtRom1.java)

## RTL8821CS UART

On an RTL8821CS board such as the RG35XX Plus, Wi-Fi is attached to SDIO
and Bluetooth is attached to a UART with RTS/CTS. Linux binds the
Bluetooth UART to its Realtek H5 setup. The H5 driver starts at
115200 baud with even parity, reads the controller identity, and asks
`btrtl` to select firmware and config by the Bluetooth HCI bus. The UART
entry requires `rtl_bt/rtl8821cs_config.bin` alongside
`rtl_bt/rtl8821cs_fw.bin`. The driver sends Set Baud Rate (`0xfc17`),
switches the host UART to the configured baud rate and flow control,
then sends the indexed epatch fragments with `0xfc20` over H5. The
controller's bootstrap responds to the serial link and baud command
before the downloaded patch starts.
[Example board device tree](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/allwinner/sun50i-h700-anbernic-rg35xx-plus.dts)
[Linux H5 driver](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_h5.c)
[Linux Realtek Bluetooth loader](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btrtl.c)

The repository's stock `fw/blobs/rtl88/rtl8821c_fw.bin` matches the
Linux USB reference. Both Linux reference files report project 10 and
version `0x75b8f098`, and each packages both ROM revisions. Their ROM 0
entries are byte-identical. Their ROM 1 entries differ in content and
layout. Realtek submitted the second file, `rtl8821cs_fw.bin`, for UART.
[Realtek's UART firmware submission](https://gitlab.com/kernel-firmware/linux-firmware/-/commit/3653d692bd0da291af02e9e4a613049315e241b9)

| ROM 1 reference | Entry length | Version word | Appended config |
| --- | --- | --- | --- |
| RTL8821CU USB | `0x8864` | `0x80112860` | `0x80112864` |
| RTL8821CS UART | `0x9040` | `0x8011303c` | `0x80113040` |

Linux chooses the firmware filename by HCI bus and then selects the
first epatch entry whose ID matches the ROM revision plus one. Both
stock ROM 1 payloads use ID 2. A shared ROM 1 binary would therefore
need one payload with controller-side transport selection and a common
version and appended-config layout. Linux could load identical firmware
bytes under the existing two names with separate board configs.
[Linux Realtek Bluetooth loader](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btrtl.c)

The 25-byte UART config examined here has a 16-byte record at offset
`0x000c`. Its baud word `0x04928002` maps to 1,500,000 baud and bit 2
of value byte 12 enables RTS/CTS. The USB source config supplies only
record `0x00d9`. Linux's UART setup rejects that config because the
baud record is missing.

The Ghidra program `/rtl8821c/rtl8821cs-bt-rom1-stock.elf` maps the UART
patch at `0x8010a000`. Its ROM 1 entry has SHA-256
`d13d41206777152ee40271eafcbe0fc47d9108c3aa85654f8667b2a4539e21c9`.
Its parser at `0x8010e7bc` copies the UART config record into
`0x8012006c` through `0x8012007b`. The entry installs callback
`0x80111a51` at `0x80120934`, a slot the stock USB patch leaves alone.
The UART hook updates the high half of `0xb5000030` from saved state
based on bit 2 of `0xb000a0a0`, clears the saved word at `0x80108a08`,
and calls resident routine `0x8000b528` with interrupts held.
In the available USB ROM dump, the resident branch at
`0x8000cec8` dispatches that slot from its transport recovery path.
The same ROM dump also contains handling for the `0xfc17` command and
its completion. The CS ROM itself has yet to be captured.

The `BT_ROM1_TRANSPORT=uart` build applies this UART config record and
installs the recovery callback. The build uses the ROM's native H5 and
HCI DMA paths and is marked `hardware-untested`. The stock CS late
initializer at `0x80112b64` also overrides HCI DMA callbacks and receive
descriptor setup. Those overrides and the CS resident entry points need
verification on hardware. Confirm download, HCI Reset, data transfer,
and transport recovery through a sleep/wake cycle on an RTL8821CS board.

## WLAN TDMA trace

The WLAN comparison is in Ghidra program
`/rtl8821c/rtl8821cu-wlan-stock-coex.elf`. Both WLAN builds received
`60 61 35 03 11 10 00 00`, `60 61 35 03 11 11 00 00`,
`60 61 08 03 11 11 00 00`, and the all-zero `0x60` disable payload from
rtw88 during the channel 1 trials. Stock firmware applies five parameter
bytes and schedules WLAN/BT sharing. The source WLAN implements the antenna
arbitration for the observed `0x60` two-slot mode on board `0x04222141`.

| Address | Observed operation |
| --- | --- |
| `0x800359ec` | Saves five H2C bytes at `0x802084c8`. Disable applies immediately. Enable sets a deferred work flag. |
| `0x80035ed8` | The foreground worker checks the flag and calls the TDMA setup callback. |
| `0x8003bb20` | Copies the parameters to `0x80204a70`, reads the beacon interval at `0xb8640554`, sets early-beacon timing at `0xb8640558`, and updates slot and MAC priority state. |
| `0x80103dd4`, `0x80103cbc` | Drive paired beacon-related transitions. The latter issues sideband `0x42` and updates the WLAN timing state. |
| `0x800069cc` | Changes the low two coexistence table control bits at `0xb86406cc` at setup and on slot events when the command flags select that behavior. |
| `0x80103aa0`, `0x80103b1c` | The first-window handler arms work with the second H2C parameter and advances the slot state. The dispatcher selects timer, beacon, or slot-extension handling from that state. |
| `0x80103988`, `0x8003a958` | Queue timed work into the stock WLAN scheduler. |
| `0x800075b4`, `0x8003be00` | Timer dispatch and stock slot transition. The transition controls transmit priority and the packet path according to the command flags. |

During the channel 1 BLE test, stock WLAN alternated `0x06cc` between
`0x18000018` and `0x1800001b` within each 100 TU beacon interval. The
previous source WLAN build held `0x18000018` for 1,176 samples. Both
builds had the same coexistence lookup tables, station TX pause byte,
and Bluetooth statistics control byte during the test. Holding the
arbitration bits clear and changing the statistics control did not
recover BLE on the previous source build.

The source foreground scheduler uses the associated beacon clock and
the first-slot duration plus the three-unit transition window. It
updates only the two arbitration bits and returns to the stock setup
state on disable or beacon loss. On a link-setup report, WLAN holds that
same state for five seconds and then resumes the requested schedule.
Other TDMA modes, stock MAC priority overrides, and additional
packet-scheduling callbacks remain open.

## Direct stock callback assignments

The table records 73 assignments recovered from four installers in the
Bluetooth Ghidra program. `Entry` is `0x8010a000`, `coex init` is
`0x8010af20`, `late init` is `0x801123b8`, and `coex late` is
`0x8010ab2c`. ROM literal loads are candidate lookup sites obtained by
finding aligned ROM words equal to each slot and the MIPS16 PC-relative
loads that reference them. A dash indicates that this scan found no
direct ROM literal load. For example, ROM code near
`0x8006104a` looks up slots relative to `0x80126a04`.
The source implementation leaves stock-only vtables at their ROM defaults.

| Installer | RAM slot | Tagged target | ROM literal loads | Source disposition |
| --- | --- | --- | --- | --- |
| entry | `80120f64` | `8010d76d` | `80046e74` | ROM retained, stock behavior uncharacterized |
| entry | `801206f8` | `8010d991` | — | ROM retained, stock behavior uncharacterized |
| entry | `80120ed8` | `8010fbbd` | `8003bae8` | ROM retained. Stock writes RF offset `0x2c` using connection state. |
| entry | `80120c34` | `8010fa65` | `800337fa` | ROM retained, stock behavior uncharacterized |
| entry | `80120c40` | `8010cea1` | `80030498` | ROM retained, stock behavior uncharacterized |
| entry | `801209ec` | `8010ef09` | `80010558` | ROM retained, stock behavior uncharacterized |
| entry | `80120c20` | `8010eedd` | `80033fb8` | ROM retained, stock behavior uncharacterized |
| entry | `80120618` | `80111609` | — | ROM retained, stock behavior uncharacterized |
| entry | `8012061c` | `8010e5a9` | — | ROM retained, stock behavior uncharacterized |
| entry | `80120f98` | `80111479` | `80007928` | ROM retained, stock behavior uncharacterized |
| entry | `80120a10` | `8010fdb1` | `8000fbc0` | ROM retained, stock behavior uncharacterized |
| entry | `80120a08` | `8010fc85` | `8000fd4c` | ROM retained, stock behavior uncharacterized |
| entry | `80120e14` | `80112125` | `80036cbe` | ROM calibration retained. Stock applies additional RF tables. |
| entry | `80120904` | `80111715` | `800009ac` | ROM retained, stock behavior uncharacterized |
| entry | `801209bc` | `801123b9` | `8000ec14` | Source late-start callback publishes the scoreboard |
| entry | `80120c30` | `8011124d` | `800341de` | ROM retained, stock behavior uncharacterized |
| entry | `80120ad0` | `80110525` | `8001e710` | Source reports LE initiation and returns to ROM HCI. Stock wraps host AFH and adjusts inquiry and connection state. |
| entry | `80120754` | `8010ecd5` | — | ROM retained, stock behavior uncharacterized |
| entry | `8012075c` | `8010e6ad` | — | ROM retained, stock behavior uncharacterized |
| entry | `80120fa4` | `8010e7b1` | `8000686c` | ROM retained, stock behavior uncharacterized |
| entry | `80120708` | `8010d4b9` | `800073da`, `80042baa`, `80042fbe`, `8004789c`, `80048dbc`, `800499d8`, `80049afa`, `8004a45c` | ROM retained, stock behavior uncharacterized |
| entry | `80120ab0` | `80110f41` | `8001b694` | ROM retained, stock behavior uncharacterized |
| entry | `80120ff4` | `8010ff89` | `80050342` | ROM retained, stock behavior uncharacterized |
| entry | `801209f4` | `8010cdd5` | `8000ff0e` | ROM retained, stock behavior uncharacterized |
| entry | `801209e0` | `8010f2a5` | `80010228` | ROM retained, stock behavior uncharacterized |
| entry | `80120914` | `8010ce21` | `8000ae78` | ROM retained, stock behavior uncharacterized |
| entry | `80120928` | `8010f365` | `8000d2b0` | ROM retained, stock behavior uncharacterized |
| entry | `80120a00` | `801110a5` | `80010060` | ROM retained, stock behavior uncharacterized |
| entry | `801208e4` | `8010f78d` | `8000048e` | ROM retained, stock behavior uncharacterized |
| entry | `80120fac` | `8010e189` | `8004857e`, `80048680` | ROM retained, stock behavior uncharacterized |
| entry | `80120f84` | `8010e539` | `80006404` | ROM retained, stock behavior uncharacterized |
| entry | `801211b8` | `8010ce11` | `8005e644` | ROM retained, stock behavior uncharacterized |
| entry | `80120d24` | `8010ded1` | `80034856` | ROM retained, stock behavior uncharacterized |
| entry | `80120df0` | `8010dbb5` | `80002424` | ROM retained, stock behavior uncharacterized |
| entry | `80120d0c` | `8010ced5` | `800053d8` | ROM retained, stock behavior uncharacterized |
| entry | `80120d04` | `8010ec49` | `8000527a` | ROM retained, stock behavior uncharacterized |
| entry | `80120d28` | `8010e35d` | `80001d86` | ROM retained, stock behavior uncharacterized |
| entry | `80120df4` | `8010d315` | `80004088` | ROM retained, stock behavior uncharacterized |
| entry | `80120c58` | `80111135` | `800301b0` | ROM retained, stock behavior uncharacterized |
| entry | `80120c28` | `8010f271` | `80031f6a` | ROM retained, stock behavior uncharacterized |
| entry | `80120bf4` | `8010f42d` | `8002dfcc` | ROM retained, stock behavior uncharacterized |
| entry | `80120c04` | `8010f3e1` | `8002f306` | ROM retained, stock behavior uncharacterized |
| entry | `801209f8` | `8010cd79` | `8000f8ec` | ROM retained, stock behavior uncharacterized |
| entry | `801209d0` | `8010cd95` | `80010a2e` | ROM retained, stock behavior uncharacterized |
| entry | `80120e2c` | `8010ce51` | `8003562a` | ROM retained, stock behavior uncharacterized |
| entry | `80120f88` | `8010ee81` | `80007592` | ROM retained, stock behavior uncharacterized |
| entry | `80120fa0` | `8010ee05` | `80006db8` | ROM retained, stock behavior uncharacterized |
| entry | `80120d20` | `8010cef9` | `80004412` | ROM retained, stock behavior uncharacterized |
| entry | `801206e4` | `8010eb81` | `8004163c` | ROM retained, stock behavior uncharacterized |
| entry | `801206fc` | `8010eb69` | — | ROM retained, stock behavior uncharacterized |
| entry | `801206ec` | `8010eb51` | — | ROM retained, stock behavior uncharacterized |
| entry | `801206e8` | `8010eb39` | — | ROM retained, stock behavior uncharacterized |
| entry | `801206f0` | `8010eb21` | — | ROM retained, stock behavior uncharacterized |
| entry | `801206f4` | `8010eb09` | — | ROM retained, stock behavior uncharacterized |
| entry | `80120f9c` | `8010da01` | `800060ac` | ROM retained, stock behavior uncharacterized |
| coex init | `80120994` | `8010c68d` | `8000e020` | Source rearms reports and applies WLAN AFH notices |
| coex init | `801209e4` | `8010a589` | `80010830` | Resident path retained. Stock callback customizes scoreboard state |
| coex init | `80120a14` | `8010c495` | `80010c0c` | Resident path retained. Stock callback adjusts RF state and acknowledges IRQ |
| coex init | `80120d1c` | `8010ba81` | `800045da` | ROM retained. Stock checks active link state and may send sideband `0x39`. |
| coex init | `80120c80` | `8010b859` | `80032d56` | ROM retained, stock behavior uncharacterized |
| coex init | `80120fe0` | `8010a6ad` | `8004dbea` | ROM retained, stock behavior uncharacterized |
| late init | `80126a4c` | `80110889` | — | ROM retained, stock behavior uncharacterized |
| late init | `80126a34` | `801100a1` | — | ROM retained, stock behavior uncharacterized |
| late init | `80126a64` | `8010ef35` | — | ROM retained, stock behavior uncharacterized |
| late init | `80126a40` | `801107c5` | — | ROM retained, stock behavior uncharacterized |
| late init | `80126a1c` | `80110745` | — | ROM retained, stock behavior uncharacterized |
| late init | `80126a04` | `8010f775` | `8006104a`, `8006105e`, `800610b8`, `8006112a`, `800611aa`, `800611e2`, `80061214`, `80061266` | ROM retained, stock behavior uncharacterized |
| late init | `80120fc0` | `8010de61` | `8004aaca` | ROM retained, stock behavior uncharacterized |
| late init | `80121530` | `8010da21` | — | ROM retained, stock behavior uncharacterized |
| late init | `80121534` | `8010dac5` | — | ROM retained, stock behavior uncharacterized |
| late init | `801220b8` | `8010f80d` | `80008a78`, `800091c4`, `8000a33e`, `800101e0`, `8002c1b2`, `8002c1d0`, `8002c812`, `80031194` | ROM retained, stock behavior uncharacterized |
| late init | `80121248` | `8010f16d` | `80008a82`, `800091d8`, `8000a270`, `8000a34c`, `8002c806`, `80031188` | ROM retained, stock behavior uncharacterized |
| coex late | `80126a70` | `8010ca89` | — | ROM retained. Stock calls ROM `0x8000e3f0` when WLAN scoreboard bit 1 is set. |
