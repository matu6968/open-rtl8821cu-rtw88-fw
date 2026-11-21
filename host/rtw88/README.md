# Optional Linux rtw88 modules

The bundle builds four modules against Ubuntu kernel `7.0.0-43-generic`
and the corresponding `linux-source-7.0.0=7.0.0-43.43` package. It includes
the source firmware's WoWLAN and deep-LPS bridges. The `--fwlog` option
adds C2H text decoding to the bundle.

```sh
python3 host/rtw88/build.py --fwlog --fetch
```

Run this from the repository root with the matching kernel headers
installed. The script checks the source hashes and module vermagic. With
`--source /path/to/drivers/net/wireless/realtek/rtw88`, it uses an existing
copy of the pinned source. It builds the modules in
`host/rtw88/build/fwlog/modules/` and records their hashes in
`host/rtw88/build/fwlog/bundle.json`. The kernel configuration must include
`CONFIG_RTW88_DEBUG=y` for logging.

## Try kernel logging

With the target unplugged and other rtw88 devices disconnected, load the
four temporary modules in dependency order. Use administrator access and
the kernel's normal module-signing policy. Place the firmware in the
normal search path before reconnecting the adapter.

```sh
modules=host/rtw88/build/fwlog/modules
sudo modprobe -r rtw88_8821cu rtw88_8821c rtw88_usb rtw88_core
sudo modprobe mac80211
sudo insmod "$modules/rtw88_core.ko" debug_mask=0x10 disable_lps_deep=Y
sudo insmod "$modules/rtw88_usb.ko"
sudo insmod "$modules/rtw88_8821c.ko"
sudo insmod "$modules/rtw88_8821cu.ko"
sudo dmesg -w
```

Reconnect the adapter and bring the Wi-Fi interface up. The firmware
startup message appears as an `FW:` line when rtw88 initializes the
station transport. To return to your installed modules, unplug the
adapter, unload the four temporary modules in reverse order, and replug.

For magic-packet system wake, pass `source_wowlan=Y` when inserting
`rtw88_core.ko` and enable USB wake on the target adapter. For automatic
USB deep LPS on an associated station, set `source_wowlan=Y` and
`disable_lps_deep=Y` on `rtw88_core.ko`, set `source_deep_auto=Y` on
`rtw88_usb.ko`, and enable station power saving. Both host options start
disabled. Magic-packet wake and automatic USB deep LPS were qualified
separately on the RTL8811CU baseline.
