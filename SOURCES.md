# Core sources

Every Libretro core that RetroNest can load, with the upstream repository and the exact commit the
binary was built from. Cores marked "no" in the last column are development-only and are left out
of release builds.

| Core | Version | License | Upstream | Commit | In release builds |
|---|---|---|---|---|---|
| bsnes | 115.1 | GPL-3.0 | https://github.com/libretro/bsnes-libretro | `b51612a74c4ff4b91aa55eb094940d9d848e0e96` | yes |
| clownmdemu | v1.6.12 | AGPL-3.0 | https://github.com/Clownacy/clownmdemu-libretro | `d43c270` | yes |
| fceumm | (SVN) | GPL-2.0 | https://github.com/libretro/libretro-fceumm | `e0f3086` | yes |
| gearsystem | 3.9.24-12-g1be9a12 | GPL-3.0 | https://github.com/drhelius/Gearsystem | `1be9a12` | yes |
| genesis_plus_gx | v1.7.4 | Non-commercial | https://github.com/libretro/Genesis-Plus-GX | `f27655b` | no |
| melondsds | 1.4.0 | GPL-3.0 | https://github.com/JesseTG/melonds-ds | `f394adbacb5722ee97c1b37c8064da9a25818310` | yes |
| mgba | 0.11-212-7a12d6d | MPL-2.0 | https://github.com/mgba-emu/mgba | `7a12d6d` | yes |
| mupen64plus_next_gles3 | 3.0-Vulkan | GPL-2.0 | https://github.com/libretro/mupen64plus-libretro-nx | `15bada8` | yes |
| pcsx_rearmed | r26l-137-g7b0e8f8 | GPL-2.0 | https://github.com/libretro/pcsx_rearmed | `7b0e8f8` | yes |
| ppsspp |  | GPL-2.0 | https://github.com/hrydgard/ppsspp | `0acbc18` | yes |
| snes9x | 1.63 | Non-commercial | https://github.com/libretro/snes9x | `fae2fea` | no |
| stella | 8.0_pre | GPL-2.0 | https://github.com/stella-emu/stella | `da5a002c318843bea1d233932508b9439798de0d` | yes |

Cores built by this project (`tools/cores/pins.json`, `tools/cores/build_cores.sh`,
and, when arcade is on, `tools/arcade/`) are built from the commits above without source changes other
than the build options in those scripts. The remaining cores are the prebuilt binaries of the
Libretro buildbot; their commit is read from the version string inside the binary.

The machine-readable form of this table, with file hashes, is `corehost/CORES_MANIFEST.json`.
