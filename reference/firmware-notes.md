# fw-stock — firmware/config pulled from stock SM-G986B (y2s, Exynos 990) firmware

Source: `samloader-rs/fw/AP_G986BXXSNHYB1_G986BXXSNHYB1_MQB93101051_REV01_user_low_ship_MULTI_CERT_meta_OS13.tar.md5`
→ member `super.img.lz4` (Android sparse image, blk 4096, 154 chunks, 9 437 184 000 bytes raw).
Vendor build: `samsung/y2sxxx/y2s:11/RP1A.200720.012/G986BXXSNHYB1:user/release-keys` (vendor is frozen at the R/11 VNDK; OS is 13).

## super layout (liblp metadata v10.0, 2 slots, single block device `super`)

| partition | size (bytes) | fs |
|---|---|---|
| system  | 6 241 730 560 | (not extracted) |
| vendor  | 1 462 374 400 | **ext4** (vol `vendor`, 351 370 × 4 KiB blocks, features: ext_attr dir_index filetype extent flex_bg sparse_super large_file huge_file uninit_bg dir_nlink extra_isize; `e2fsck -fn` clean) |
| product | 1 264 021 504 | (not extracted) |
| odm     |     4 349 952 | ext4 (only `etc/build.prop`, selinux, fs_config) |

No `_a`/`_b` suffixes (non-A/B device). Each partition is one LINEAR extent.

## How it was extracted (repeatable, no simg2img/lpunpack needed)

```sh
cd fw-stock
# 1+2: stream tar -> lz4 -> sparse parser; writes only the requested LP partitions (no 9 GB intermediate)
tar -xOf ../samloader-rs/fw/AP_G986BXXSNHYB1_*_meta_OS13.tar.md5 super.img.lz4 \
  | lz4 -dc | python3 extract_super_partitions.py tmp vendor odm      # ~5 s
# 3: ext4 -> use Homebrew e2fsprogs debugfs
D=/opt/homebrew/opt/e2fsprogs/sbin/debugfs
$D -R 'rdump /firmware tmp/full/vendor' tmp/vendor.img   # "changing ownership" warnings are harmless
$D -R 'rdump /etc      tmp/full/vendor' tmp/vendor.img   # do NOT pipe debugfs into `head` (SIGPIPE truncates rdump)
$D -R 'dump /build.prop tmp/full/vendor/build.prop' tmp/vendor.img
$D -R 'rdump /etc tmp/full/odm' tmp/odm.img
```
`extract_super_partitions.py` (kept here) parses the sparse format (RAW/FILL/DONT_CARE/CRC), reads LP geometry @4096 and
primary metadata @0x3000 from the stream head, then writes each partition's LINEAR extents to `<name>.img`.
All 501 files under vendor `/etc`, `/firmware`, `/tee` and odm `/etc` were size-verified against the debugfs listing.

`fw-stock/vendor.img` (1.46 GB raw ext4, < 2 GB so kept) can be re-read any time with `debugfs -R 'ls -l /path' vendor.img`
or `debugfs -R 'dump /path out' vendor.img`.

## Notes / what does not exist in this vendor

- Whole `/vendor/firmware` (74 MB, 108 files) was copied, so camera/DSP/NPU/MFC/NFC/haptics blobs are included too.
- Wi-Fi chip is **BCM4375 rev B1 (PCIe)**, FW `4375b1-roml/config_pcie_release Version=18.41.117 FWID=01-1b4772fd`;
  NVRAM header e.g. `# Hubble Y2 EU Murata ES4.3_191224`. Wi-Fi blobs live in `/vendor/firmware`, `/vendor/etc/wifi` has only supplicant configs.
  Kernel driver picks `bcmdhd_sta.bin_b1` + `nvram.txt_<module>_<rev>` by module (Murata `1rh` / SEMCO) and CID/revision.
- `/vendor/apex/com.samsung.android.wifi.broadcom.signed.apex` was checked: its payload is an empty ext4 (manifest only) → not copied.
- Bluetooth: BCM4375B1 `.hcd` patchram (Murata / SEMCO / SEMCO-SEM); **no `/vendor/etc/bluetooth/` directory** exists. BT addr is read from `/mnt/vendor/efs/bluetooth/bt_addr` (see init.exynos990.rc).
- Audio: amps are **Cirrus CS35L40** ×2 (`bot` = bottom loudspeaker, `rcv` = earpiece/receiver used as 2nd stereo speaker; mixer controls "Left/Right DSP1 Firmware"); haptics Cirrus CS40L2x.
  ABOX topology `abox_tplg.bin` + Calliope DSP fw (`calliope_*`). No `sound_trigger*` XML in vendor/etc (only `lib*/hw/sound_trigger.primary.exynos990.so`).
- Display: **no panel / mafpc / panel `.dat` files exist in vendor** (on this device panel tables/mAFPC data are presumably built into the kernel panel driver / DTBO). Nothing copied for display.
- File modes were normalised to 0644 on copy.

## Files (path relative to fw-stock/, size in bytes)

| file | size | type | note |
|---|---:|---|---|
| `odm/etc/build.prop` | 863 | platform | odm build properties |
| `vendor/build.prop` | 5591 | platform | vendor build properties |
| `vendor/default.prop` | 686 | platform | vendor default properties |
| `vendor/etc/LVACFS_Calibration.dat` | 885620 | audio | LVACFS acoustic speaker tuning data |
| `vendor/etc/LVACFS_Configuration.txt` | 1786 | audio | LVACFS acoustic speaker tuning data |
| `vendor/etc/LVACFS_ControlParams.txt` | 17768 | audio | LVACFS acoustic speaker tuning data |
| `vendor/etc/SoundBoosterParam.txt` | 11907 | audio | SoundBooster params (text) |
| `vendor/etc/a2dpsink_audio_policy_configuration.xml` | 908 | audio | audio policy include (module-specific) |
| `vendor/etc/abox_debug.xml` | 2256 | audio | ABOX debug config |
| `vendor/etc/audio_board_info.xml` | 4146 | audio | audio board info (built-in mic characteristics) |
| `vendor/etc/audio_effects.xml` | 1833 | audio | audio effects config |
| `vendor/etc/audio_effects_common.conf` | 1396 | audio | audio effects config |
| `vendor/etc/audio_effects_sec.xml` | 4850 | audio | audio effects config |
| `vendor/etc/audio_effects_spatializer.xml` | 394 | audio | audio effects config |
| `vendor/etc/audio_policy_configuration.xml` | 9122 | audio | audio policy configuration |
| `vendor/etc/audio_policy_configuration_base.xml` | 18293 | audio | audio policy configuration |
| `vendor/etc/audio_policy_volumes.xml` | 12895 | audio | audio policy volume curves |
| `vendor/etc/bluetooth_audio_policy_configuration.xml` | 2099 | audio | audio policy: BT audio HAL module |
| `vendor/etc/btcon.json` | 3 | bt | Bluetooth config json |
| `vendor/etc/dax3_media_codecs_dolby_audio.xml` | 3010 | audio | Dolby audio codecs declaration |
| `vendor/etc/default_volume_tables.xml` | 5133 | audio | default volume tables |
| `vendor/etc/dolby/dax-default.xml` | 116596 | audio | Dolby DAX default tuning |
| `vendor/etc/floating_feature.xml` | 37754 | platform | Samsung floating feature flags |
| `vendor/etc/fstab.exynos990` | 2924 | platform | vendor fstab |
| `vendor/etc/hearing_aid_audio_policy_configuration.xml` | 685 | audio | audio policy include (module-specific) |
| `vendor/etc/init/android.hardware.audio.service.rc` | 406 | audio | audio HAL service rc |
| `vendor/etc/init/android.hardware.bluetooth@1.0-service.rc` | 371 | bt | Bluetooth HAL service rc |
| `vendor/etc/init/android.hardware.wifi.supplicant-service.rc` | 920 | wifi | Wi-Fi HAL/supplicant service rc |
| `vendor/etc/init/android.hardware.wifi@1.0-service.rc` | 448 | wifi | Wi-Fi HAL/supplicant service rc |
| `vendor/etc/init/init.exynos990.rc` | 59855 | platform | main vendor init rc (abox/bt/sysfs setup) |
| `vendor/etc/init/vendor.samsung.hardware.wifi@2.0-service.rc` | 437 | wifi | Wi-Fi HAL/supplicant service rc |
| `vendor/etc/init/wifi.rc` | 2440 | wifi | Wi-Fi init rc (sysfs/data perms) |
| `vendor/etc/init/wifi_brcm.rc` | 1291 | wifi | Wi-Fi init rc (sysfs/data perms) |
| `vendor/etc/media_codecs_dolby_audio.xml` | 3010 | audio | Dolby audio codecs declaration |
| `vendor/etc/mixer_gains.xml` | 81810 | audio | ALSA mixer gain tables |
| `vendor/etc/mixer_paths.xml` | 166220 | audio | ALSA mixer paths (routes for ABOX/codec/amps) |
| `vendor/etc/mixer_usb_default.xml` | 7727 | audio | USB audio mixer config |
| `vendor/etc/mixer_usb_gray.xml` | 7704 | audio | USB audio mixer config |
| `vendor/etc/mixer_usb_white.xml` | 7703 | audio | USB audio mixer config |
| `vendor/etc/playback_record_audio_policy_configuration.xml` | 1630 | audio | audio policy include (module-specific) |
| `vendor/etc/r_submix_audio_policy_configuration.xml` | 2265 | audio | audio policy include (module-specific) |
| `vendor/etc/sec_audio_volume_curve.xml` | 363 | audio | Samsung volume curve |
| `vendor/etc/situation_audio_policy_volumes_sec.xml` | 12454 | audio | Samsung situation-based volume tables |
| `vendor/etc/tms_audio_policy_configuration.xml` | 2140 | audio | audio policy include (module-specific) |
| `vendor/etc/usb_audio_policy_configuration.xml` | 1766 | audio | audio policy include (module-specific) |
| `vendor/etc/wifi/indoorchannel.info` | 14979 | wifi | Wi-Fi indoor-channel restriction table |
| `vendor/etc/wifi/p2p_supplicant_overlay.conf` | 373 | wifi | wpa_supplicant / p2p config overlay |
| `vendor/etc/wifi/wpa_supplicant.conf` | 115 | wifi | wpa_supplicant / p2p config overlay |
| `vendor/etc/wifi/wpa_supplicant_overlay.conf` | 108 | wifi | wpa_supplicant / p2p config overlay |
| `vendor/etc/wlan_common_rc` | 2440 | wifi | Wi-Fi init rc fragment (sysfs perms /sys/wifi/*) |
| `vendor/etc/wlan_vendor_rc` | 1291 | wifi | Wi-Fi init rc fragment (sysfs perms /sys/wifi/*) |
| `vendor/firmware/APBargeIn_AUDIO_SLSI.bin` | 5898 | audio | ABOX SLSI voice params, barge-in |
| `vendor/firmware/APBiBF_AUDIO_SLSI.bin` | 984 | audio | ABOX SLSI voice params, bi-beamforming |
| `vendor/firmware/APDV_AUDIO_SLSI.bin` | 441426 | audio | ABOX SLSI voice params, DMV6.3.0 (dual-mic voice) |
| `vendor/firmware/APSV_AUDIO_SLSI.bin` | 318042 | audio | ABOX SLSI voice params, SV1.2.0 (single-mic voice) |
| `vendor/firmware/APTV_AUDIO_SLSI.bin` | 3674 | audio | ABOX voice params, Goodix TVCAST |
| `vendor/firmware/AP_AUDIO_SLSI.bin` | 324402 | audio | ABOX SLSI voice solution parameter blob (main) |
| `vendor/firmware/NPU_NFD_P4.7.tflite` | 2476300 | npu | NPU face-detection model |
| `vendor/firmware/SoundBoosterParam.bin` | 8704 | audio | SoundBooster speaker tuning params |
| `vendor/firmware/a2dpcom.bin` | 434192 | audio | ABOX DSP module: A2DP offload codec |
| `vendor/firmware/abox_tplg.bin` | 1009260 | audio | ABOX ASoC topology binary (DAI links/mixers/routes), loaded by abox driver |
| `vendor/firmware/abox_tplg.conf` | 143098 | audio | ABOX topology source config (text) for abox_tplg.bin |
| `vendor/firmware/avsyncmix.bin` | 5684 | audio | ABOX DSP module: AV-sync mixer (Calliope2 OEM post-process) |
| `vendor/firmware/bayer2rgb.bin` | 52708 | dsp | DSP kernel: bayer conversion |
| `vendor/firmware/bayer2yuv.bin` | 60876 | dsp | DSP kernel: bayer conversion |
| `vendor/firmware/bcm4375B1_murata.hcd` | 176645 | bt | BCM4375B1 Bluetooth patchram, Murata module |
| `vendor/firmware/bcm4375B1_semco.hcd` | 176645 | bt | BCM4375B1 Bluetooth patchram, SEMCO module |
| `vendor/firmware/bcm4375B1_semco_sem.hcd` | 176658 | bt | BCM4375B1 Bluetooth patchram, SEMCO module (SEM variant) |
| `vendor/firmware/bcmdhd_clm.blob` | 29224 | wifi | Wi-Fi CLM regulatory/country blob |
| `vendor/firmware/bcmdhd_mfg.bin_b1` | 1124726 | wifi | BCM4375B1 Wi-Fi firmware, MFG/factory-test mode |
| `vendor/firmware/bcmdhd_mon.bin_b1` | 1296195 | wifi | BCM4375B1 Wi-Fi firmware, monitor/sniffer mode |
| `vendor/firmware/bcmdhd_sta.bin_b1` | 1297036 | wifi | BCM4375B1 PCIe Wi-Fi firmware, STA/normal mode (18.41.117) |
| `vendor/firmware/bidirmic.bin` | 293120 | audio | ABOX DSP module: bidirectional mic |
| `vendor/firmware/calliope2.dt` | 7312 | audio | ABOX Calliope2 firmware register/config table |
| `vendor/firmware/calliope_dram.bin` | 427468 | audio | ABOX Calliope DSP firmware, DRAM image (_2 = alt/newer rev) |
| `vendor/firmware/calliope_dram_2.bin` | 2451448 | audio | ABOX Calliope DSP firmware, DRAM image (_2 = alt/newer rev) |
| `vendor/firmware/calliope_sram.bin` | 57888 | audio | ABOX Calliope DSP firmware, SRAM image (_2 = alt/newer rev) |
| `vendor/firmware/calliope_sram_2.bin` | 50132 | audio | ABOX Calliope DSP firmware, SRAM image (_2 = alt/newer rev) |
| `vendor/firmware/cs35l40-bot-dsp1-spk-prot-calib.bin` | 980 | audio | Cirrus CS35L40 amp speaker-protection calibration coeffs (bot=loudspeaker, rcv=earpiece) |
| `vendor/firmware/cs35l40-bot-dsp1-spk-prot.bin` | 4216 | audio | Cirrus CS35L40 amp DSP1 speaker-protection tuning coeffs (bin) |
| `vendor/firmware/cs35l40-bot-dsp1-spk-prot.wmfw` | 31780 | audio | Cirrus CS35L40 amp DSP1 speaker-protection firmware (wmfw) |
| `vendor/firmware/cs35l40-rcv-dsp1-spk-prot-calib.bin` | 980 | audio | Cirrus CS35L40 amp speaker-protection calibration coeffs (bot=loudspeaker, rcv=earpiece) |
| `vendor/firmware/cs35l40-rcv-dsp1-spk-prot.bin` | 4216 | audio | Cirrus CS35L40 amp DSP1 speaker-protection tuning coeffs (bin) |
| `vendor/firmware/cs35l40-rcv-dsp1-spk-prot.wmfw` | 31780 | audio | Cirrus CS35L40 amp DSP1 speaker-protection firmware (wmfw) |
| `vendor/firmware/cs40l20.bin` | 8684 | haptics | Cirrus CS40L2x haptics tuning/coeff blob |
| `vendor/firmware/cs40l20.wmfw` | 34464 | haptics | Cirrus CS40L2x haptics DSP firmware (wmfw) |
| `vendor/firmware/cs40l25a.wmfw` | 38224 | haptics | Cirrus CS40L2x haptics DSP firmware (wmfw) |
| `vendor/firmware/cs40l25a_cal.wmfw` | 11048 | haptics | Cirrus CS40L2x haptics DSP firmware (wmfw) |
| `vendor/firmware/cs40l25a_clab.bin` | 100 | haptics | Cirrus CS40L2x haptics tuning/coeff blob |
| `vendor/firmware/cs40l25a_clab.wmfw` | 36260 | haptics | Cirrus CS40L2x haptics DSP firmware (wmfw) |
| `vendor/firmware/cs40l25a_exc.bin` | 48 | haptics | Cirrus CS40L2x haptics tuning/coeff blob |
| `vendor/firmware/dax_param.bin` | 34736 | audio | Dolby Atmos (DAX) parameters |
| `vendor/firmware/default_configuration.flm.cfg.bin` | 1328 | misc | binary config blob (FLM) |
| `vendor/firmware/dsp.bin` | 3309444 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_dn.bin` | 3021624 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_do.bin` | 3021624 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_gkt.xml` | 5158 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_iac_dm.bin` | 7164 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_iac_dm_dn.bin` | 7164 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_iac_dm_do.bin` | 7164 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_iac_pm.bin` | 7188 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_iac_pm_dn.bin` | 7368 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_iac_pm_do.bin` | 7520 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_ivp_dm.bin` | 28672 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_ivp_dm_dn.bin` | 28672 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_ivp_dm_do.bin` | 28672 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_ivp_pm.bin` | 2800 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_ivp_pm_dn.bin` | 2800 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_ivp_pm_do.bin` | 2800 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_master.bin` | 76 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dsp_reloc_rules.bin` | 15920 | dsp | Exynos DSP (SCore/IVP) firmware/data |
| `vendor/firmware/dual_cal_tele_tof.bin` | 760 | camera | dual-camera calibration data |
| `vendor/firmware/dual_cal_wide_swide.bin` | 760 | camera | dual-camera calibration data |
| `vendor/firmware/dual_cal_wide_tele.bin` | 760 | camera | dual-camera calibration data |
| `vendor/firmware/dual_cal_wide_tof.bin` | 760 | camera | dual-camera calibration data |
| `vendor/firmware/fmsb.bin` | 163872 | audio | ABOX DSP module (SoundBooster family) |
| `vendor/firmware/gdc.bin` | 13721 | camera | camera GDC kernel params |
| `vendor/firmware/is_lib.bin` | 4717472 | camera | camera ISP (FIMC-IS) library |
| `vendor/firmware/is_mcu_fw.bin` | 32544 | camera | camera MCU (OIS) firmware |
| `vendor/firmware/is_rta.bin` | 3429632 | camera | camera ISP RTA library |
| `vendor/firmware/libann.elf` | 763924 | dsp | Exynos DSP/NPU library (ELF) |
| `vendor/firmware/libenf.elf` | 70656 | dsp | Exynos DSP/NPU library (ELF) |
| `vendor/firmware/libenf_uv.elf` | 89080 | dsp | Exynos DSP/NPU library (ELF) |
| `vendor/firmware/libivp.elf` | 61088 | dsp | Exynos DSP/NPU library (ELF) |
| `vendor/firmware/liblog.elf` | 35532 | dsp | Exynos DSP/NPU library (ELF) |
| `vendor/firmware/libnn.elf` | 324124 | dsp | Exynos DSP/NPU library (ELF) |
| `vendor/firmware/listenbackeffect.bin` | 82576 | audio | ABOX DSP module: listen-back effect |
| `vendor/firmware/mfc_fw.bin` | 697272 | video | MFC (multi-format codec) video firmware |
| `vendor/firmware/nfc/sec_s3nsen4_firmware.bin` | 184620 | nfc | Samsung S3NSEN4 NFC controller firmware |
| `vendor/firmware/nvram.txt_1rh_es11_b0` | 13133 | wifi | Wi-Fi NVRAM, Murata (1RH) module, rev in name (esXX_bN) |
| `vendor/firmware/nvram.txt_1rh_es12_b0` | 13133 | wifi | Wi-Fi NVRAM, Murata (1RH) module, rev in name (esXX_bN) |
| `vendor/firmware/nvram.txt_1rh_es13_b0` | 13563 | wifi | Wi-Fi NVRAM, Murata (1RH) module, rev in name (esXX_bN) |
| `vendor/firmware/nvram.txt_1rh_es32_b0` | 13030 | wifi | Wi-Fi NVRAM, Murata (1RH) module, rev in name (esXX_bN) |
| `vendor/firmware/nvram.txt_1rh_es41_b1` | 13085 | wifi | Wi-Fi NVRAM, Murata (1RH) module, rev in name (esXX_bN) |
| `vendor/firmware/nvram.txt_1rh_es42_b1` | 11231 | wifi | Wi-Fi NVRAM, Murata (1RH) module, rev in name (esXX_bN) |
| `vendor/firmware/nvram.txt_1rh_es43_b1` | 14223 | wifi | Wi-Fi NVRAM, Murata (1RH) module, rev in name (esXX_bN) |
| `vendor/firmware/nvram.txt_CS01_semco_b1` | 13707 | wifi | Wi-Fi NVRAM, SEMCO module, rev in name |
| `vendor/firmware/nvram.txt_ES00_semco_b0` | 11892 | wifi | Wi-Fi NVRAM, SEMCO module, rev in name |
| `vendor/firmware/nvram.txt_ES01_semco_b0` | 9691 | wifi | Wi-Fi NVRAM, SEMCO module, rev in name |
| `vendor/firmware/nvram.txt_ES02_semco_b0` | 11534 | wifi | Wi-Fi NVRAM, SEMCO module, rev in name |
| `vendor/firmware/nvram.txt_ES03_semco_b0` | 11530 | wifi | Wi-Fi NVRAM, SEMCO module, rev in name |
| `vendor/firmware/rgb2bayer.bin` | 3192 | dsp | DSP kernel: to-bayer conversion |
| `vendor/firmware/rxse.bin` | 389280 | audio | ABOX DSP module: voice RX speech enhancement |
| `vendor/firmware/sb_ll.bin` | 57272 | audio | ABOX DSP module: SoundBooster (ll=low-latency/music/others) |
| `vendor/firmware/sb_music.bin` | 124880 | audio | ABOX DSP module: SoundBooster (ll=low-latency/music/others) |
| `vendor/firmware/sb_others.bin` | 124640 | audio | ABOX DSP module: SoundBooster (ll=low-latency/music/others) |
| `vendor/firmware/setfile_2la.bin` | 8055060 | camera | camera sensor setfile (sensor in name) |
| `vendor/firmware/setfile_2ld.bin` | 12565720 | camera | camera sensor setfile (sensor in name) |
| `vendor/firmware/setfile_3j1.bin` | 9987792 | camera | camera sensor setfile (sensor in name) |
| `vendor/firmware/setfile_gw2.bin` | 8699300 | camera | camera sensor setfile (sensor in name) |
| `vendor/firmware/sv.bin` | 1375616 | audio | ABOX DSP module: speech/voice |
| `vendor/firmware/txse1.bin` | 1200048 | audio | ABOX DSP module: voice TX speech enhancement |
| `vendor/firmware/txse2.bin` | 81632 | audio | ABOX DSP module: voice TX speech enhancement |
| `vendor/firmware/txse3.bin` | 982272 | audio | ABOX DSP module: voice TX speech enhancement |
| `vendor/firmware/usbin.bin` | 8480 | audio | ABOX DSP module: USB audio in |
| `vendor/firmware/usbout.bin` | 8904 | audio | ABOX DSP module: USB audio out |
| `vendor/firmware/volumemonitor.bin` | 15576 | audio | ABOX DSP module: volume monitor |
| `vendor/firmware/vts.bin` | 112860 | audio | VTS (voice-trigger subsystem, DMIC) firmware |
| `vendor/firmware/yuv2bayer.bin` | 6028 | dsp | DSP kernel: to-bayer conversion |
| `vendor/ueventd.rc` | 13567 | platform | vendor ueventd rules (device node perms) |

**Total: 160 files, 78 760 227 bytes (~75 MiB)** (+ `vendor.img` 1 462 374 400 bytes, + `extract_super_partitions.py`).
