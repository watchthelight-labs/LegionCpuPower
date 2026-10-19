# Changelog

## 0.2.0 (2026-10-19)

- Both fixes on at boot. Boot-args `-legioncpunohwp` and `-legioncpunopl` turn one off.
- Heat guard steps back up: after 30 s at 100 C or more the firmware's power limit returns until the package has been
  below 85 C for a minute, then the raised limit applies again.

## 0.1.0 (2026-10-19)

- First version: per-core HWP requests (`debug.legioncpu.hwp`) and the MCHBAR power limit copied from MSR 0x610
  (`debug.legioncpu.pl`), both switched at runtime and off at boot.
- LegionCpuProbe 1.1 (read-only MSR and MCHBAR probe) and `tools/cpuload.py` for measuring.
