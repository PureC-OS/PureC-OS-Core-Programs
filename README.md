# PureC-OS-Core-Programs

Userland of PureC OS: init, shell, coreutils, network daemons, disk utilities, installer and test programs.
The repository lives in programs/PureC-OS-Core-Programs of the PureC OS tree; libc, the toolchain (mk/toolchain.mk),
linker scripts, crt0, libpurec, libwolf and the other libraries are taken from ROOT_DIR (default ../..).

## Build

    make            build every program into $(BIN_DIR)/programs
    make ports      lua, vim, tcc
    make packages   one .pkg.tar.gz (+ .sig when ~/.config/purec-pkg/signing.pem exists) per package in $(BIN_DIR)/core-repo

`make iso` in the OS repo runs `make packages` here, installs the packages with the host pkg into $(BIN_DIR)/stage
and puts that tree into the initramfs (mk/pkg/stage.sh, mk/gen_assets.py --stage).

Version 1.0.0 (file VERSION), pkgrel 1. The file list of each package is packages.conf; mkpackages.py builds them.

## Packages

| package | contents |
| --- | --- |
| init | init, getty, initctl, systemctl, journalctl, reboot, poweroff, shutdown, halt, manual pages |
| sh | /bin/sh, /usr/bin/sh |
| coreutils | file, text and shell utilities (ls cat cp mv rm grep sort man less ...), /usr/bin duplicates, manual pages |
| net-tools | netd dhcpc resolved ntpd ip ping host ss hostname httpget httpsget iwxctl |
| disk-utils | fdisk mkfs blkid mkinitcpio hddcheck |
| accounts | sudo id whoami |
| system | system, battery, cpuinfo, uptime, free, ps, kill |
| installer | /sbin/installer |
| core-tests | hello forktest proctest thrtest posixtest fbtest udptest memhog memtest fputest burn eat echo-dyn dltest-dyn |
| lua, micropython | built only when their binaries exist |
| base | meta package, depends on init sh coreutils net-tools disk-utils accounts system pkg |

pkg itself is built by programs/purec-os-pkg (`make package`) and copied into the same repository directory.
curl, cmaketest and qtcoretest are built or installed elsewhere and stay in assets/manifest.txt.
