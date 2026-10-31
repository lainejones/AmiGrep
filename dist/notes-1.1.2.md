An installer. Open the unpacked AmiGrep drawer and double-click **Install_AmiGrep**: it asks where the AmiGrep drawer should go (default `SYS:Tools`) and whether to copy the Shell command `AmiGrep` to `C:` (default yes), then copies AmiGrepGUI, its icon, the README and the licence into the drawer. Run it again to update; your settings are kept. Both programs still run straight from the unpacked drawer too.

Otherwise AmiGrep is the same as [1.1.1](https://github.com/lainejones/AmiGrep/releases/tag/v1.1.1) (the CLI `AmiGrep` is still 1.1).

Use the **.lha**: it keeps the executable bit, which a .zip can't store (see the README). The installer sets it on the copies it installs.

Tested on an A4000 (AmigaOS 3.2): the installer made the drawer and copied everything there.
