# rbAmp ESP-IDF Component — Documentation

Documentation for the **rbAmp** esp-idf client for the rbAmp I²C energy-monitoring modules (Basic / Standard).

## Contents

- [Overview](01_overview.md)
- [Tier Support](02_tiers.md)
- [Sensor Selection](03_sensor_selection.md)
- [Hardware Setup](04_hardware.md)
- [Quickstart](05_quickstart.md)
- [Examples](06_examples.md)
- [DIY Integrations](07_diy_integrations.md)
- [Cloud Integrations](08_cloud_integrations.md)
- [API Reference](09_api_reference.md)
- [Troubleshooting](10_troubleshooting.md)
- [Changelog](11_changelog.md)

## Quick glossary

| Term | Meaning |
|---|---|
| **UI1 / UI2 / UI3** | Module variants with a voltage input plus 1 / 2 / 3 current channels (power, PF and Wh available) |
| **I1 / I2 / I3** | Current-only variants (no voltage, no power or Wh) |
| **BASIC / STANDARD (STD)** | The two shipping module lines; `rbamp_begin()` detects which one is connected |
| **RT** | Real-time values (U, I, P, PF), refreshed by the module about every 100 ms |
| **Window / period** | The module averages over 100 ms windows; a *period* is the interval between two of your snapshot commands |
| **Snapshot command** | Command that freezes the average over the current period so it can be read |
| **Snapshot** | The result of one snapshot command + read (`rbamp_period_snapshot_t`) |
| **Virgin module** | A module still at the factory address `0x50` |
| **GC (General-Call)** | One broadcast I²C frame that makes several modules take their snapshot at the same moment |
| **DRDY** | Optional output pin: a short low pulse each time fresh RT data is ready |
