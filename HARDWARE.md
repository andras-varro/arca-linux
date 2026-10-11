# RoomWizard Hardware

The Steelcase RoomWizard board as a physical object: what is on it, what each connector goes to, what
is unpopulated, and how the case comes apart without breaking anything.

**This document answers "what is the hardware?".** How any of it *behaves* — device nodes, addresses,
drivers, the traps in driving it — is [`SYSTEM_ANALYSIS.md`](SYSTEM_ANALYSIS.md) §3 onward. Open work is
[`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md). One fact, one home — if you find something stated in two
places, the other copy is the stale one.

**Provenance.** A full teardown on 2026-07-30 of the unit labelled `RW29 1G-093`, with continuity and
voltage measurements on that board, plus on-device readings across two further units. Every statement
here is measured on this hardware unless it carries a tag; the three tags and what each admits to are
[How to read a claim](SYSTEM_ANALYSIS.md#how-to-read-a-claim-in-this-document).

⚠️ **Read [Physical safety](SYSTEM_ANALYSIS.md#physical-safety) before opening a unit.** Up to 57 V sits
on the RJ45 magnetics when powered, and separating the bezel without disconnecting `J2` first tears the
touch flex — that has already cost one unit its touchscreen.

**The photos.** 17 images in [`HardwarePhotos/`](HardwarePhotos/) from that teardown, cited below beside
the parts they show rather than collected into an index. They are stored in **Git LFS**: a clone made
without `git lfs install` gets pointer stubs instead of JPEGs, and `git lfs pull` repairs it.

**The two faces.** `Top-*` is the face toward the screen; `Bottom-*` is the face toward the rear cover —
the one carrying the SoC, RAM, NAND, Ethernet, PoE and every header. Start from the two unobstructed
overviews: [`Top-Overwiev.jpg`](HardwarePhotos/Top-Overwiev.jpg) and
[`Bottom-Overview.jpg`](HardwarePhotos/Bottom-Overview.jpg). ⚠️ **Silkscreen is split across both faces**
— `J5`/`J6` are labelled on the bottom, `P3`/`P4` on the top, and they occupy the same board area, which
is why both appear together in bottom-side photos.

---

## Contents

1. [Identification](#1-identification)
2. [Parts inventory](#2-parts-inventory)
3. [Connectors](#3-connectors)
4. [Unpopulated and expansion](#4-unpopulated-and-expansion)
5. [Enclosure and service](#5-enclosure-and-service)
6. [Display module and touch assembly](#6-display-module-and-touch-assembly)

---

## 1. Identification

| | |
|---|---|
| Board | Steelcase Inc **`550-0204-03`**, **© 2010**, `STM-5 STM-5E20784`, `94V-0`, `TESTED Compulrol` |
| Model string | `Steelcase RoomWizard 20` (`/proc/device-tree/model`) |
| Compatible | `ti,omap3-rw20`, `ti,omap3` |
| U-Boot identity | `soc=omap3`, `board=rw20` |
| OS build | `SteelCase RW20 Embedded Platform (Yocto) 3.1.4` — a Yocto build by eInfochips. ⚠️ A per-unit reading, not a fleet constant: `SYSTEM_ANALYSIS.md#51-as-shipped` |
| Teardown unit | case label `RW29 1G-093`; asset labels `46837.0300`, `47270.0310` |

**The `© 2010` silkscreen is a design copyright, not a build date.** Three independent date codes on the
teardown unit cluster in March 2018: the LCD module (`W180322`), its T-CON board (`1810`) and the RJ45
MagJack (`18111`). A panel swap would not also replace the Ethernet jack, so this is a 2018-built unit on
an eight-year-old board design. (The touch controller's `1043` code is long-lived module stock, ordinary
for a bonded touch assembly.) The panel and T-CON labels are legible in
[`display_back_overview.jpg`](HardwarePhotos/display_back_overview.jpg).

## 2. Parts inventory

Markings as silkscreened and laser-etched, so a part can be identified from a photo without a meter.
What each one *does* is its subsystem section in `SYSTEM_ANALYSIS.md` §3.

| Ref | Marking | Part |
|---|---|---|
| `U11` | `OMAP3503ECUS`, `72P19HQ`, `G1` | TI **OMAP3503** application processor |
| `U12` | `D9RMJ`, `70CI7 / XQ52` | Micron **mobile DDR SDRAM** (`D9RMJ` is Micron's FBGA code) |
| `U13` | `NQH53`, `7ME12 / X5Y3` | Micron **NAND flash** — MT29F2G16ABBEAHC, 256 MiB SLC, 16-bit, BCH8 |
| `U14` | `TPS65930A2`, `74AJKFW $4`, `G1` | TI **TPS65930** — PMIC + audio codec, **TWL4030 family** |
| `U15` | `LAN9221-ABZJ`, `A1751-AB24` | SMSC **LAN9221** Ethernet **MAC+PHY** on the GPMC bus |
| `U16` | `LVDS83B`, `77AK23K G4` | TI **SN65LVDS83B** parallel-RGB → LVDS transmitter |
| `U27` | `MA3232C`, `7AK G4`, `A1R7` | TI **MAX3232C** dual RS-232 transceiver |
| `U1` | `TPS23750`, `6BTG4 / A89N` | TI **802.3af PoE PD** interface + DC/DC controller |
| `U2` | `MT1107 V74968` | isolated feedback regulator (PoE secondary side) |
| `U4` | `PSS4325 / 68154 / C935` | **TPS54325**-class synchronous buck |
| `U17` | `⧠M` logo, `GC5.5V0.47F`, `JAPAN` | Panasonic/Matsushita **"Gold Cap" supercapacitor**, 5.5 V 0.47 F — RTC hold-up |
| `U22`, `U23` | — | Side LED bar drivers (`L8`/`L2` inductors adjacent) |
| `U24` | `CCH / TI 8A / Z86W` | TI, **unidentified** |
| `U25`, `U32` | `WP245 / TI 81K / CD4S` (16-pin) | TI, **unidentified**. `U25` by the touch flex, `U32` by `J5`/`LED1`–`4`. Position suggests level shifters. |
| `U33` | — | **Unpopulated** wide SOIC/SSOP land |
| `Q1`, `D1` | `4848 5BD` · `NHSTQW 3406` | PoE flyback FET / secondary rectifier |
| `SPKR1` | — | Single square metal-can magnetic speaker, ~20 mm |
| `LED1`–`LED5` | — | Discrete SMD LEDs; `LED1`–`4` in a row beside `J5` |
| — | Coilcraft **POE13F-12L** (`1809 J`) | PoE isolated flyback transformer |
| — | `C&K CHINA(9) EP11 0.4VA MAX 1744` | Small transformer near the touch flex — **purpose unconfirmed** |

Where to look for each on the board:

| Photo | Shows |
|---|---|
| [`Bottom-Top-Left2.jpg`](HardwarePhotos/Bottom-Top-Left2.jpg), [`Bottom-Center.jpg`](HardwarePhotos/Bottom-Center.jpg) | Bottom-face detail — the SoC/RAM/NAND corner and mid-board |
| [`Bottom-Bottom-Left.jpg`](HardwarePhotos/Bottom-Bottom-Left.jpg), [`Bottom-Bottom-Right.jpg`](HardwarePhotos/Bottom-Bottom-Right.jpg) | The PoE section — flyback transformer, `Q1`/`D1`, `U1`/`U2` — and `SPKR1` |
| [`Bottom-Top-Right.jpg`](HardwarePhotos/Bottom-Top-Right.jpg) | `U32` beside `LED1`–`4` |
| [`Touch-Connector-Probably.jpg`](HardwarePhotos/Touch-Connector-Probably.jpg) | `U25` and the small `C&K` transformer beside the touch flex |
| [`Top-Left-Top.jpg`](HardwarePhotos/Top-Left-Top.jpg), [`Top-Top-Middle.jpg`](HardwarePhotos/Top-Top-Middle.jpg), [`Top-right-bottom.jpg`](HardwarePhotos/Top-right-bottom.jpg) | Top-face detail — `U27`, the SD socket, the LED harness sockets |

## 3. Connectors

| Ref | Type | Goes to |
|---|---|---|
| `J1` | **microSD** push-push socket | Boot and root storage |
| `J2` | 8-pin white **FFC** | Touch controller flex — ⚠ release before separating the bezel |
| `J3` | TE **MagJack `1-6605834-1`** RJ45 | Ethernet **and PoE power in** |
| `J4` | **micro-USB** | The only USB connector — MUSB OTG **[inferred]** |
| `J5`, `J6` | 1×10 sockets, 2 mm pitch, **empty as shipped** | XBee radio site — [see below](#4-unpopulated-and-expansion) |
| `J7`, `J8` | 5-pin white **JST** | Side LED status bars in the left/right case edges |
| `P2` | Long fine-pitch 2-row, **unpopulated** | **Unknown** — the last unidentified footprint |
| `P3` | 2×7, 0.1", **unpopulated** | **TI-14 JTAG** (high confidence) |
| `P4` | 2×5, 0.1", **header fitted by the operator 2026-09** | **RS-232 console** via `U27` — verified |
| — | 40-pin **FFC** | Display panel harness |

`J1`, `J3`, `J7`/`J8` and the 40-pin display FFC are all visible unobstructed in
[`Top-Overwiev.jpg`](HardwarePhotos/Top-Overwiev.jpg); the touch flex landing and `J2` are in
[`Touch-Connector-Probably.jpg`](HardwarePhotos/Touch-Connector-Probably.jpg).

**Case openings**, along the rear edge in order: RJ45, micro-USB, a second rectangular slot
(unidentified — nothing behind it, possibly a variant SKU), and a pinhole over a white tact **reset
button**. All four in [`Connectors-reset-button.jpg`](HardwarePhotos/Connectors-reset-button.jpg).

## 4. Unpopulated and expansion

[`Bottom-Top-Right.jpg`](HardwarePhotos/Bottom-Top-Right.jpg) is the photo for this whole section:
`J5`/`J6`, the `P3`/`P4` through-holes passing down the channel between them, `LED1`–`4` and `U32`.

**`J5` + `J6` — the XBee socket, empty as shipped.** Two 1×10 female strips at **2.0 mm pitch**, rows
**~24 mm** apart: the Digi XBee footprint (2 mm pitch, 22.86 mm row spacing). `J5` carries a white
**pin-1 dot**, and the **metal inner bezel has a trapezoidal cut-out matching the XBee outline** — the
chassis was tooled for this module. A real XBee test-fits perfectly. **No radio was fitted in any of the
three units as received**, so the batch shipped without the option; there is no antenna on the PCB
because on an XBee the antenna is part of the module. Vendor software confirms the intent — a full
`AT`-command implementation and a ZigBee gateway daemon, both on the legacy `ttyS2`:
[Serial ports](SYSTEM_ANALYSIS.md#312-serial-ports).

⚠️ **Two of our units now have a module seated in them, and both modules are identified.** Each carries
`XB24-ACI-001 revC` — MaxStream, © 2007, `FCC ID OUR-XBEE`: the 1 mW chip-antenna 802.15.4 **Series 1**
part — read off the can, with 64-bit addresses `0013A200 4060D747` and `0013A200 403BEA96`. So the series
question is settled and the vendor's settable `ATMY`/`ATCH` are the right command set
([Serial ports](SYSTEM_ANALYSIS.md#312-serial-ports)). ⚠️ **Orientation is still unverified**, and `J5`
pin 1 is a live 3.3 V rail whatever UART3 does, so a reversed insertion is already a completed
experiment. Neither module ever answered, and the open `DOUT` net below is why — no module was ever
given a receive path. No unit now has a module fitted; the modules are loose spares.

**Pinout, partly measured 2026-07-30.** `J5` carries XBee pins **1–10** (pin 1 is the dotted end), `J6`
carries **11–20**. Numbering runs down one strip and back up the other like a DIP, so pins 1 and 10 are
at opposite ends of `J5`, *not* across from each other — the usual way to get this backwards.

| XBee pin | Socket | Signal | Status |
|---|---|---|---|
| 1 | `J5` | `VCC` | **measured 3.3 V.** In spec — an XBee's absolute max is 3.6 V, so a 5 V reading would have been a stop. Powering a module is safe. |
| 2 | `J5` | `DOUT` — the SoC's RX | **measured OPEN: it does not reach the SoC's `uart3_rx` ball**, so nothing a module says can arrive. Of the twenty pins this was the one that mattered most, and it is the answer to the radio's silence — see below. Unpowered it also reads open (>MΩ) to both pin 1 and pin 10, which excludes a discrete pull at the socket. |
| 3 | `J5` | `DIN` — the SoC's TX | **measured: reaches the SoC's `uart3_tx` pad.** Driven low on command and read at the socket, against a 3.3 V idle — so the pinmux ([Serial ports](SYSTEM_ANALYSIS.md#312-serial-ports)) and the socket trace are both confirmed in this direction. |
| 5 | `J5` | `RESET` | not measured; should sit ~3.3 V released rather than held low. |
| 9 | `J5` | `SLEEP_RQ` | not measured; should not be sitting high. |
| 10 | `J5` | `GND` | **measured ground.** With pin 1 at 3.3 V this confirms the socket is correctly identified *and* correctly oriented. |

The **power** question is therefore settled, and so is the pinmux — the pads come up in `uart3` mode
([Serial ports](SYSTEM_ANALYSIS.md#312-serial-ports)). An XBee fed reversed dies instantly, which is why
the orientation was measured before anything was inserted — and why, on the unit that now has one seated,
the module's health is an open question rather than an assumption. **The two data nets are now measured,
and they disagree.**

⚠️ **`DIN` reaches the SoC and `DOUT` does not, so a module can be talked to and can never answer.**
Measured on a powered, opened unit with the socket empty, black lead on pin 10 throughout, and **pin 1
re-read as 3.3 V beside each reading** so a poor probe contact cannot pass as a result — it produced one
spurious 4.4 V before that control was added. Holding UART3's transmitter low through `LCR` bit 6 takes
**pin 3 from 3.3 V to 0 V**: the same pin in two states, so only the drive changed. **Pin 2 ignores the
`uart3_rx` pad's internal pull.** Driven `PULLUP` → `PULLDOWN` → `PULLUP` with a read-back after each write,
pin 2 read 0 V in all three states, where a pullup of tens of kΩ against a 10 MΩ meter would have to show
~3.3 V on a connected pin. The competing reading — connected, but held at ground by something else, which
would also beat the pullup — is excluded on the same unit: **unpowered, pin 2 to pin 10 is DC-open.**
⚠️ It does not *look* open at first. An ohmmeter reports a different value on every range and then reads
infinite when the leads are lifted and replaced, because its test current is charging a capacitance rather
than finding a resistance; the reconnect is the tell. `probes/xbee_socket_continuity.sh` drives both
directions from the SoC end, so the BGA never needs probing, and its `padup`/`paddown` steps are the
meter-only pin-2 test. **[n=1]** — whether the other two units share the open net is unmeasured.

**`P4` — the RS-232 console. Pinout verified by continuity:**

```
P4 pin 2  ->  U27 pin 14 (T1OUT)   console TX, RS-232 level, out of the device
P4 pin 3  ->  U27 pin 13 (R1IN)    console RX, RS-232 level, into the device
P4 pin 5  ->  U27 pin 15 (GND)     ground
```

Pin 1 is the square pad; even pins on the top row, odd on the bottom. Only these three are wired —
MAX3232 channel 1 only. Three wires, no soldering strictly required (a 0.1" female jumper or pogo pins in
the plated holes will do). **RS-232 levels: a 3.3 V TTL adapter will not work here** — use a real USB↔DB9
adapter, or tap `U27`'s logic side instead. ⚠️ **To a standard DB9 DTE adapter the mapping is pin-for-pin
— P4 2→DB9 2, 3→3, 5→5, no crossover and no null modem** (`[inferred]` from the DB9 pinout; the `P4` side
is the continuity measurement above). Console settings are 115200 8N1, no flow control
([Serial ports](SYSTEM_ANALYSIS.md#312-serial-ports)). **Prove the link with pins 2 and 5 only** — TX and
ground, two wires — and power-cycle: boot text means cable, pinout and baud are all confirmed, with
nothing wired that could inject into the device. A silent terminal with all three connected cannot say
which of the three was wrong. **Discriminate a cable before wiring it**: an RS-232 output idles at
**−5 to −12 V** against its ground, a TTL one idles **high**, and on a four-wire flying-lead cable the red
lead is **VCC and must stay disconnected** — nothing on `P4` takes power.

**A TTL cable still buys a read-only console, for one wire and no purchase.** `U27` pin 11 is `T1IN` — the
SoC's TX at 3.3 V logic, *before* the level shifter — so a TTL adapter's **RX** on pin 11 and its ground
on **`P4` pin 5** (the same net as `U27` pin 15, and a 0.1" hole rather than fine pitch) reads the boot log
with a single sense connection and nothing driven. That is the diagnostic a *failed* boot leaves, when
there is no SSH to ask. ⚠️ **Do not drive `U27` pin 12 (`R1OUT`) to type** — it is the transceiver's own
output into the SoC, so a TTL transmitter there is fighting it; typing needs the RS-232 side, or a MAX3232
breakout between the TTL cable and `P4`. Pin 11 is `[inferred]` from the standard MAX3232 SOIC-16 pinout,
corroborated by this board's own continuity above — pins 14/13/15/16 all land where that pinout says. What comes out of it, and why this project does not use it:
[Serial ports](SYSTEM_ANALYSIS.md#312-serial-ports).

**`P3` — TI-14 JTAG, high confidence.** Continuity against `U27` produces the TI-14 signature:

| `P3` pin | Measured | TI-14 expects | |
|---|---|---|---|
| 4 | GND (via `U16` pin 53) | GND | ✔ |
| 5 | 3.3 V (`U27` pin 16) | `PD` / Vref | ✔ |
| 6 | open | **keyed, no pin** | ✔ |
| 8 | GND | GND | ✔ |
| 10 | GND | GND | ✔ |
| 12 | open | GND | ✖ one discrepancy |

Vref on 5 with grounds on 4/8/10 plus the key at 6 is not an arrangement anything else uses. Pin 12 is
the loose end — a no-connect on this board, or a missed probe. The remaining pins (TMS, nTRST, TDI, TDO,
RTCK, TCK, EMU0, EMU1) read open against `U27` because they run to the SoC. Not actionable — the
boot-path costs in [SYSTEM_ANALYSIS.md §1](SYSTEM_ANALYSIS.md#what-a-boot-path-write-costs-to-undo) mean JTAG is
needed only for `mtd0` — but don't mistake it for a second serial port.

**Test points: `TP1`–`TP59`**, individually labelled and probe-sized, over the whole top side. Dense
clusters at `TP19`–`TP31` (around the SD socket), `TP13`–`TP18` (mid-board), `TP34`–`TP36` and a long run
`TP39`–`TP59`. They are numbered, not named, so silkscreen alone tells you nothing about the net. Mapping
them by meter means probing back to `U14`, whose ADCIN pins are BGA-hidden. **The practical route is the
reverse:** power the unit, touch a resistor from 3.3 V to a candidate test point, and watch which
`in_voltage2..7_raw` moves ([ADC](SYSTEM_ANALYSIS.md#311-adc-and-temperature-twl4030-madc)). That needs no
teardown.

**Room inside the case:** modest but real, around the `J5`/`J6`/`P3`/`P4` corner and near the unpopulated
`U33` and `P2` lands. Two constraints on anything added: the 802.3af power budget
([Power](SYSTEM_ANALYSIS.md#35-network-and-power)) and the fact that the case has **no ventilation
slots**.

## 5. Enclosure and service

**Reopenable, non-destructively: screws and clips, no glue.** Four metal threaded bosses moulded into the
bezel, self-tapping screws for the LCD mount, plastic retention clips along the top and bottom edges.
Bezel material is `>PC/ABS<`, moulded part `560-0540-0x`. The only bonded joint is touch glass to bezel,
which is meant to be permanent. The bosses, the clips and the side LED bars in their channels are all in
[`bezel_with_touch_screen.jpg`](HardwarePhotos/bezel_with_touch_screen.jpg).

**The one destructive risk is the touch flex.** Disconnect `J2` before separating the bezel from the
board — see [Physical safety](SYSTEM_ANALYSIS.md#physical-safety).

Removing **only the rear cover** exposes the entire bottom face — SoC, RAM, NAND, Ethernet, PoE,
`J5`/`J6` — without going near the bezel or the touch flex. That is the safe way to inspect a working
unit.

## 6. Display module and touch assembly

The screen is three separable physical pieces plus the glass: the bezel, the LCD module, and a T-CON
board mounted on the module's rear. The panel part number, the parallel-RGB → LVDS → T-CON signal chain
and the connector positions actually wired are [Display](SYSTEM_ANALYSIS.md#32-display); the touch
controller and its I2C bus are [Touch](SYSTEM_ANALYSIS.md#33-touch). What the photos add is where each
piece sits and what it is labelled.

| Photo | Shows |
|---|---|
| [`display_screen.jpg`](HardwarePhotos/display_screen.jpg) | The bare LCD module, removed from the bezel |
| [`display_back_overview.jpg`](HardwarePhotos/display_back_overview.jpg) | Module rear — the part-number and date-code labels, the connector into the T-CON, and the harness back to the main board's 40-pin FFC |
| [`Screen-Controller.jpg`](HardwarePhotos/Screen-Controller.jpg), [`display_controller_closeup.jpg`](HardwarePhotos/display_controller_closeup.jpg) | The T-CON board in place, then its silicon close enough to read |
| [`Touch-Connector-Probably.jpg`](HardwarePhotos/Touch-Connector-Probably.jpg) | The touch flex, its 8-pin landing at `J2`, and `IC1` — the touch controller silicon, which carries no `U`-prefixed designator and appears nowhere in the main board's inventory above |
| [`bezel_with_touch_screen.jpg`](HardwarePhotos/bezel_with_touch_screen.jpg) | Bezel inner face with the glass bonded in — and **no light aperture** anywhere in it, which is the physical half of why there is no ambient-light sensor to find |
