# Contributing

Issues and pull requests are welcome for reproducible bugs, protocol evidence, documentation corrections and support for additional RP2350 boards.

Before opening a change:

1. Do not include USB serial numbers, personal paths, full Flash backups, private captures or credentials.
2. State the macOS version, board revision, receiver firmware/mode, build profile and exact commands used.
3. Separate observed output from hypotheses. Include raw report bytes or a minimized test case when possible.
4. Run `firmware/verify.sh` and the relevant Node tests. Hardware-only claims should include the probe command and whether the physical controller was connected.
5. Changes to USB descriptors, VID/PID, report layouts, rumble, sensor formats or Flash persistence need an updated limitation/verification note.

Please do not submit changes intended to bypass authentication, platform security, copyright protection or access controls.
