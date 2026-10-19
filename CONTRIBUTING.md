# Contributing

The most useful report is a measurement from another Arrow Lake machine: `sysctl debug.legioncpu.status`, the MCHBAR
power limit the probe shows (`ioreg -r -c LegionCpuProbe -w0 | grep Mchbar`), and `tools/cpuload.py` with 1 and all
threads, once with the plugin's switches off and once on. Open an issue with the laptop model, CPU, macOS build and the
versions of OpenCore and Lilu.

Pull requests: keep them small and say how you tested them. I read and test every PR before merging. AI-assisted PRs are
fine as long as you can explain the change, which is the same bar I hold my own code to.

Contributions are under the ISC licence.
