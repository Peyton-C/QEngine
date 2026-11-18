# Engine Firmware versions
Every Engine OS release, the devices it shipped for, whether QEngine boots it on each, and the last date QEngine booted it on any device.

## Key
| Mark | Meaning |
| --- | --- |
| `Y` | Booted |
| `N` | Does not boot, or boots with issues, see the notes under the table |
| `?` | Not tried, a build is available |
| `!` | Not tried, no build available |
| `F` | Factory build, preinstalled only and not tried |
| `-` | Not released for that device |

A build counts as available when QEngine has that device's image or a known download for it, or, from 1.6.0, either of those for any device of the same signing at that version, since from 1.6.0 inMusic ships one Engine binary per architecture and signing.

## Devices
Each update image carries one family. Engine 1.6.0+ use a single binary for unsigned armv7, signed armv7 and signed arm64

| Code | Device | Image family |.
| --- | --- | --- |
| `JP07`  | SC5000 | Unsigned armv7, player (AZ01) |
| `JP08`  | SC5000M | Unsigned armv7, player (AZ01) |
| `JP11`  | PRIME GO | Unsigned armv7, player (AZ01) |
| `JC11`  | PRIME 4 | Unsigned armv7, console (AZ01) |
| `JC16`  | PRIME 2 | Unsigned armv7, console (AZ01) |
| `NH08`  | Mixstream Pro | Unsigned armv7, console (AZ01) |
| `JP13`  | SC6000 | Unsigned armv7, premium (AZ01) |
| `JP14`  | SC6000M | Unsigned armv7, premium (AZ01) |
| `JC11S` | PRIME 4+ | Signed armv7, "for jc11s" (AZ0x) |
| `JP11S` | PRIME GO+ | Signed armv7, "for jc11s" (AZ0x) |
| `JP20`  | SC LIVE 2 | Signed armv7, "for jc11s" (AZ0x) |
| `JP21`  | SC LIVE 4 | Signed armv7, "for jc11s" (AZ0x) |
| `NH08S` | Mixstream Pro+ | Signed armv7, "for nh08s" (AZ0x) |
| `NH10`  | Mixstream Pro Go | Signed armv7, "for nh08s" (AZ0x) |
| `JP24`  | PRIME 4 G2 | Signed arm64 (AZ04) |
| `RMZ2`  | RANE SYSTEM ONE | Signed arm64 (AZ04) |

## 1.x
| Version | Release date | JP07 | JP08 | JP11 | JC11 | JC16 | JP13 | JP14 | Last tested |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1.0.0 | By Jun 2017  | F | - | - | - | - | - | - | - |
| 1.0.1 | Unknown      | Y | - | - | - | - | - | - | 2026-11-09 |
| 1.0.2 | 2017-09-28   | Y | - | - | - | - | - | - | 2026-11-09 |
| 1.0.3 | 2017-11-02   | Y | - | - | - | - | - | - | 2026-11-09 |
| 1.2.0 | 2018-06-18   | Y | ! | - | - | - | - | - | 2026-11-09 |
| 1.2.1 | 2018-09-12   | ! | ! | - | - | - | - | - | - |
| 1.2.2 | 2018-09-14   | Y | ? | - | - | - | - | - | 2026-11-09 |
| 1.3.1 | May 2019     | Y | Y | - | F | - | - | - | 2026-11-10 |
| 1.3.2 | Jun–Jul 2019 | - | ? | - | Y | - | - | - | 2026-11-10 |
| 1.3.3 | 2019-10-30   | - | - | - | Y | - | - | - | 2026-11-09 |
| 1.4.0 | 2019-12-17   | ? | ? | - | ? | - | - | - | - |
| 1.4.1 | By Feb 2020  | Y | Y | F | Y | F | - | - | 2026-11-10 |
| 1.5.0 | By Aug 2020  | - | - | F | - | - | - | - | - |
| 1.5.1 | 2020-08-03   | ? | ? | ? | ? | ? | ? | ? | - |
| 1.5.2 | Aug 2020     | Y | Y | N | Y | Y | Y | Y | 2026-11-10 |
| 1.5.3 | By Sep 2020  | - | - | - | - | - | Y | - | 2026-11-10 |
| 1.5.4 | By Feb 2021  | ! | ! | - | F | - | ! | - | - |
| 1.6.0 | 2021-02-23   | Y | Y | ? | Y | ? | ? | Y | 2026-11-10 |
| 1.6.1 | 2021-04-13   | ? | ? | ? | ? | ? | ? | ? | - |
| 1.6.2 | 2021-06-08   | Y | ? | N | Y | Y | Y | ? | 2026-11-15 |

Prime GO 1.5.2 quits on its battery check, even built as `JC11`.
Prime GO 1.6.2 stays up only when built as `JC11`; as `JP11` it was tried before the fake battery.
1.2.1 was briefly public and pulled. Contemporary coverage lists both models but the M-specific package is unverified.
1.2.0 on SC5000M is owner-reported. The package and distribution are unresolved.
1.5.4 is confirmed for PRIME 4. Denon mentioned possible SC5000/M shipments, and a separate SC6000 owner was unsure of the version.

## 2.x
| Version | Release date | JP07 | JP08 | JP11 | JC11 | JC16 | NH08 | JP13 | JP14 | JP20 | JP21 | NH08S | Last tested |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2.0.0 | 2021-10-06 | ? | ? | ? | ? | ? | - | ? | ? | - | - | - | - |
| 2.0.1 | 2021-10-22 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.0.2 | 2021-11-10 | ? | ? | ? | ? | ? | ? | ? | ? | - | - | - | - |
| 2.1.0 | 2021-12-16 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.1.1 | 2021-12-21 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.1.2 | 2022-02-11 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.2.0 | 2022-04-20 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.2.1 | 2022-05-24 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.2.2 | 2022-06-16 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.3.0 | 2022-08-24 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.3.1 | 2022-09-02 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.3.2 | 2022-09-22 | ? | ? | ? | ? | ? | Y | ? | ? | - | - | - | 2026-11-10 |
| 2.3.3 | 2022-11-04 | - | - | - | - | - | - | - | - | Y | ? | - | 2026-11-11 |
| 2.4.0 | 2022-12-13 | ? | ? | ? | Y | ? | ? | ? | ? | ? | ? | ? | 2026-11-08 |

Mixstream Pro 2.0.1 was reported at first boot and also had a public updater.

## 3.x
| Version | Release date | JP07 | JP08 | JP11 | JC11 | JC16 | NH08 | JP13 | JP14 | JC11S | JP20 | JP21 | NH08S | NH10 | Last tested |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 3.0.0 | 2023-03-03 | ? | ? | ? | ? | ? | ? | ? | ? | - | ? | ? | ? | ? | - |
| 3.0.1 | 2023-04-05 | ? | ? | ? | ? | ? | ? | ? | ? | - | ? | ? | ? | ? | - |
| 3.1.0 | 2023-06-15 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - |
| 3.1.1 | 2023-07-05 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - |
| 3.2.0 | 2023-08-23 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - |
| 3.3.0 | 2023-12-06 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - |
| 3.4.0 | 2024-03-14 | ? | ? | ? | Y | ? | ? | ? | ? | ? | ? | ? | ? | ? | 2026-11-08 |

The catalog flags Mixstream Pro Go 3.0.0 and 3.0.1 OTA.
I've been unable to find the Mixstream Pro Go 3.0.0 and 3.0.1 images as anything besides OTA updates.

## 4.x
| Version | Release date | JP07 | JP08 | JP11 | JC11 | JC16 | NH08 | JP13 | JP14 | JC11S | JP11S | JP20 | JP21 | NH08S | NH10 | JP24 | RMZ2 | Last tested |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 4.0.0 | 2024-06-13     | ? | ? | ? | Y | ? | ? | ? | ? | ? | - | ? | ? | ? | ? | - | - | 2026-11-08 |
| 4.0.1 | 2024-09-16     | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | ? | ? | ? | ? | - | - | - |
| 4.1.0 | 2024-10-18     | ? | ? | ? | Y | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | - | 2026-11-08 |
| 4.2.0 | 2024-11-26     | ? | ? | ? | Y | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | - | 2026-11-08 |
| 4.3.0 | 2025-03-26     | ? | ? | Y | Y | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | - | 2026-11-16 |
| 4.3.1 | 2025-03-26     | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | - | - |
| 4.3.2 | 2025-06-17     | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | - | - |
| 4.3.3 | 2025-08-21     | - | - | - | - | - | - | - | - | ? | ? | ? | ? | ? | ? | - | - | - |
| 4.3.4 | 2025-11-18     | ? | ? | ? | ? | ? | ? | ? | ? | ? | Y | ? | Y | ? | ? | - | - | 2026-11-16 |
| 4.4.0 | 2026 (factory) | - | - | - | - | - | - | - | - | - | - | - | - | - | - | F | F | - |
| 4.5.0 | 2026-01-21     | - | - | - | - | - | - | - | - | - | - | - | - | - | - | - | Y | 2026-08-19 |
| 4.6.0 | 2026-02-26     | - | - | - | - | - | - | - | - | - | - | - | - | - | - | - | Y | 2026-08-13 |

4.4.0 for RANE SYSTEM ONE and Prime 4 G2 came from reviews.
4.3.1 uses the Engine DJ download page date. Public use is independently documented by April 16, 2025.

## 5.x
| Version | Release date | JP07 | JP08 | JP11 | JC11 | JC16 | NH08 | JP13 | JP14 | JC11S | JP11S | JP20 | JP21 | NH08S | NH10 | JP24 | RMZ2 | Last tested |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 5.0.0 | 2026-05-13 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | ? | - |
| 5.0.1 | 2026-05-15 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | ? | - |
| 5.0.2 | 2026-06-24 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | ? | - |
| 5.0.3 | 2026-07-06 | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | ? | - | ? | - |
| 5.0.4 | 2026-07-28 | Y | ? | Y | ? | ? | ? | Y | ? | Y | ? | ? | ? | ? | ? | - | Y | 2026-11-15 |
| 5.1.0 | 2026-09-01 | - | - | - | - | - | - | - | - | - | - | - | - | - | - | Y | - | 2026-11-06 |
| 5.1.1 | 2026-09-28 | - | - | - | - | - | - | - | - | - | - | - | - | - | - | Y | - | 2026-11-06 |
| 5.2.0 | 2026-09-23 | - | - | - | - | - | - | - | - | - | - | - | - | - | - | - | Y | 2026-10-08 |
| 5.3.0 | 2026-11-05 | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | Y | 2026-11-17 |

