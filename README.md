# ZY-M100-24GV2 firmware

Custom firmware for the Zigbee module inside the Tuya ZY-M100-24GV2 24 GHz presence sensor
(`TS0601` / `_TZE204_7gclukjs`, also sold as Wenzhi WZ-M100-W). It stops the sensor from flooding
your Zigbee network, and it installs over the air from Zigbee2MQTT: no soldering, no Tuya hub.

Support for the ZY-M100-24GV3 (`_TZE204_ya4ft0w4`) is [being tested](#zy-m100-24gv3).

Measured on a converted sensor: traffic went from 35–120 messages a minute to 4 in 3 minutes, and presence
still reports instantly. Details in [verification](docs/verification.md#on-hardware).


## The problem

The stock sensor sends its target distance about once a second, all the time, plus repeats of
values that haven't changed. A few of these can keep a whole Zigbee network busy. The vendor fixed
this for other models with an MCU update, but never for the M100 24 GHz line
([wzwenzhi/Wenzhi-ZigBee2mqtt#11](https://github.com/wzwenzhi/Wenzhi-ZigBee2mqtt/issues/11),
[Koenkk/zigbee2mqtt#19045](https://github.com/Koenkk/zigbee2mqtt/issues/19045)). The only fix so
far was opening the sensor and flashing the radar MCU with an ST-Link.

## The fix

The sensor has two chips: a GD32 MCU that runs the radar and produces the flood, and a Tuya ZT3L
Zigbee module that forwards everything it gets.
- The GD32 can't be updated over the air: it receives Tuya MCU updates but has no bootloader to
  install them.
- The Zigbee module can be, through z2m's normal OTA.

So this replaces the module's firmware. The GD32 still chatters over its UART, but the module now
only puts real changes on air, and adds settings the stock firmware never had. Installing it from
stock uses the same method romasku/tuya-zigbee-switch uses for 180+ Tuya device types. More in
[findings](docs/findings.md) and [how it works](docs/how-it-works.md).

| | Stock | This firmware |
|---|---|---|
| Distance reports | ~1/s, always, with repeats | Off by default. 1 s–1 h interval, deadband, never a repeat |
| Unchanged values | re-sent every 10 s / 10 min | never sent |
| Presence | immediate | immediate |
| Illuminance | frequent | at most every 60 s, only on real change |
| Distance calibration | none | offset and scale, stored on the device |
| Updates | none from the vendor for this model | over the air from z2m, new releases show up as "update available" |
| Recovery | n/a | safe mode keeps Zigbee and OTA working; A/B update slots |

## Requirements

- Zigbee2MQTT 2.x. Tested against 2.14.2.
- The sensor paired and working in z2m, showing up as `ZY-M100-24GV2` with manufacturer
  `_TZE204_7gclukjs`. The 24GV3 is in testing, see [below](#zy-m100-24gv3). Other ZY-M100
  variants use different hardware or DPs and are **not** supported.
- The sensor close to the coordinator or a router, on stable power.

### ZY-M100-24GV3

| Sensor | Manufacturer | Status |
|---|---|---|
| ZY-M100-24GV2 | `_TZE204_7gclukjs` | Supported |
| ZY-M100-24GV3 | `_TZE204_ya4ft0w4` | **In testing**, pre-release only |
| ZY-M100-24GV3 | `_TZE200_ya4ft0w4` | Not supported, different Zigbee module |
| ZY-M100-24GV3 | `_TZE204_gkfbdvyx`, `_TZE200_gkfbdvyx` | Not supported, different radar firmware |

The V3 test build is [v1.0.4-rc1](https://github.com/jkaberg/M100_24Ghz_radar/releases/tag/v1.0.4-rc1).
It runs on one tester's V3: presence and illuminance work. Distance, changing settings and
long-term running are not confirmed yet. The regular releases below don't offer anything to a V3.

There is no way back to the stock firmware over the air, and a module that never comes back on
Zigbee needs a wired flash. If you're OK with that and want to help, convert one unit and follow
[issue #1](https://github.com/jkaberg/M100_24Ghz_radar/issues/1), which has the instructions.

## Install

### 1. Set up z2m (once)

**Converter.** One file covers the sensor before and after the update. Either:
- in the z2m frontend (windfront): **Settings → Dev console**, add an external converter named
  `zy-m100-zrd.mjs` and paste in
  [its contents](https://github.com/jkaberg/M100_24Ghz_radar/releases/latest/download/zy-m100-zrd.mjs);
- or put the file in `external_converters/` in the z2m data directory (the folder with
  `configuration.yaml`):
  ```sh
  curl -L -o <z2m data directory>/external_converters/zy-m100-zrd.mjs \
    https://github.com/jkaberg/M100_24Ghz_radar/releases/latest/download/zy-m100-zrd.mjs
  ```

**OTA index.** Point z2m at the index published with every release, either in the OTA section of
the frontend's **Settings** (`zigbee_ota_override_index_location`) or in `configuration.yaml`:

```yaml
ota:
  zigbee_ota_override_index_location: https://github.com/jkaberg/M100_24Ghz_radar/releases/latest/download/z2m_ota_index.json
```

Restart z2m. The device page of the sensor now has an OTA section.

The index offers the first image **only** to `TS0601` / `_TZE204_7gclukjs`. Every stock Tuya MCU
module shares its OTA IDs, so without that filter other Tuya devices would be offered an image
that breaks them. Each file is pinned by SHA-512. z2m's normal OTA index is still used for all
your other devices.

<details>
<summary>Offline install, without the hosted index</summary>

Download `zy-m100-zrd-z2m-<version>.zip` from [Releases](https://github.com/jkaberg/M100_24Ghz_radar/releases)
and unpack it into the z2m data directory. It contains the converter, the images and
`ota/local_ota_index.json`. Then use:

```yaml
ota:
  zigbee_ota_override_index_location: ota/local_ota_index.json
```

For later updates, unpack a newer zip over the old files and restart z2m.
</details>

### 2. Update the sensor

1. On the device page in z2m: **OTA** → **Check for update** → **Update**. This takes 10–20 min.
   Don't cut the power during the update, or for a minute afterwards.
2. When it reaches 100 %, press **Permit join**. On its first boot the firmware moves itself into
   the chip's own update slots (a few seconds, maybe a restart or two, which is safe) and then
   joins again with the same IEEE address.
3. That's it: z2m re-interviews and reconfigures it by itself, and it shows up as
   `ZY-M100-24GV2-ZRD`.

### Later updates

New releases show up in z2m as "update available", just press **Update**. Updates go into the
chip's second slot and only take over once complete and checked, and settings are kept. If a
release notes a converter change, replace `zy-m100-zrd.mjs` the same way as above and restart z2m.

## Using it

| Exposed | |
|---|---|
| `occupancy` | Presence, on every change |
| `presence_state` | `none` / `presence` / `move`, at most every 10 s |
| `illuminance` | At most every 60 s, and only on a real change |
| `target_distance` | Distance to the target in m, only when distance reporting is on |
| `distance_report_interval` | **0 = off (default)**, otherwise 1–3600 s: at most one distance report per interval. Repeated values are never sent |
| `distance_deadband` | Only report distance after it moved this many cm. 0 = every change, 10–20 skips the radar's ±10 cm jitter |
| `distance_offset` | Calibration in cm, added to every distance reading (−300…300). Stand at a known distance and adjust until it matches |
| `distance_scale` | Calibration in %, applied before the offset (50…200). Only needed if the error grows with distance |
| `move_sensitivity`, `presence_sensitivity`, `detection_distance_min/max`, `presence_timeout` | Radar settings, same as before. The radar rounds ranges to 0.75 m steps. A sensitivity of `0` means it hasn't been set since the radar MCU last started (it can't be read back) |
| `safe_mode` | `true` = started without the radar code, see below |

In Home Assistant some entities change name: `presence` → `occupancy`, `state` → `presence_state`,
`distance` → `target_distance`. Check automations that used the old ones.

## When something goes wrong

| What you see | What to do |
|---|---|
| Update stalls or fails | Move the sensor closer and retry. Nothing switches over until a download is complete and checked |
| Back on Zigbee but values are missing or wrong | Press **Reconfigure** on the device page (and **Interview** if it still shows the stock model). If it persists, open an issue: fixes ship over the air |
| `safe_mode` is `true` | The module restarted 5 times within 30 min of each other, so it now runs Zigbee and OTA only. Install a fixed image, or leave it powered for 30 min and restart it once |
| Can't re-pair | Power-cycle it 10 times, each under 2 s, then power-cycle once more. Holding the sensor's key for 6 s also works, if your unit has one |
| Never shows up on Zigbee again | Wired recovery: USB-UART on the ZT3L's SWS pad (pad 17), then `make -C firmware flash`. See [how it works](docs/how-it-works.md#recovery-by-design) |

## Building from source

Releases are built by [GitHub Actions](.github/workflows/release.yml) when a `v*` tag is pushed.
To build locally:

```sh
tools/setup.sh            # toolchain, Telink SDK 3.7.2.0, test dumps, python venv (sha256-checked)
firmware/build.sh         # current version, with image and ELF checks
make -C firmware/tests    # host tests, power-cut and NV simulations, GD32 frame check, fuzzing
python3 tools/make_bundle.py   # dist/release/: images, converters, hosted OTA index, zip
```

## Docs

- [Findings](docs/findings.md): why it floods, why the radar MCU can't be fixed over the air, and
  what the stock module looks like
- [How it works](docs/how-it-works.md): firmware design, first-boot handoff, recovery
- [Verification](docs/verification.md): what has been tested, the red-team rounds, residual risk
- GD32 UART protocol, [V2](docs/gd32-7gclukjs-protocol.md) and
  [V3](docs/gd32-ya4ft0w4-protocol.md): reverse engineering of the radar MCU
- [Red-team reports](docs/redteam/)

## Credits

This builds directly on other people's work:

- [slacky1965/tuya_thermostat_zrd](https://github.com/slacky1965/tuya_thermostat_zrd): the
  TuyaMCU-bridge firmware structure, build system and OTA packaging this started from (Apache-2.0)
- [romasku/tuya-zigbee-switch](https://github.com/romasku/tuya-zigbee-switch): the first-boot A/B
  handoff in `firmware/src/ota_scheme/` (MIT) and the proof that stock Tuya modules take custom
  firmware over the air
- [Telink Zigbee SDK](https://github.com/telink-semi/telink_zigbee_sdk) (Apache-2.0)
- [Andrik45719/ZY-M100](https://github.com/Andrik45719/ZY-M100): the GD32 dumps and first
  analysis of the MCU-OTA path, used for the reverse engineering and the tests
- [andi-at-1/z2m-tuya-mcu-ota](https://github.com/andi-at-1/z2m-tuya-mcu-ota): the Tuya MCU-OTA
  protocol work that showed what can and can't be updated
- [pvvx](https://github.com/pvvx): TlsrPgm for wired recovery, and ZigbeeTLc, which confirmed the
  voltage-sense pin choice
- adiov, tandarra, kkossev and everyone in
  [Koenkk/zigbee2mqtt#19045](https://github.com/Koenkk/zigbee2mqtt/issues/19045) who took these
  sensors apart and documented what they found
- [Zigbee2MQTT](https://github.com/Koenkk/zigbee2mqtt) and zigbee-herdsman

## License

Apache-2.0, see [LICENSE](LICENSE). `firmware/src/ota_scheme/` contains MIT-licensed code from
romasku/tuya-zigbee-switch, see [NOTICE](NOTICE).
