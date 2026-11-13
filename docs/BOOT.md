# 부팅 흐름 — 실기 부트로더의 역할 재현

실기 a5000은 전원을 켤 때마다 Sony 부트로더(내장 RAM 0xC0000000에 상주)가 NAND를 마운트하고 여러 이미지를 RAM에 올린 뒤 커널로 넘어간다. 커널과 모듈은 그 결과를 **전제**로 동작하므로(검증됨), 에뮬레이터도 같은 상태를 만들어 줘야 한다. 부트로더 자체는 플래시 예약 블록에 있어 백업에 없으므로, **부트로더가 NAND에 의존해 하는 일은 Sony 자신의 NAND 스택으로 재현**하고(1단계 로더), 나머지는 보드 코드가 한다.

## 부트로더가 남기는 것 (펌웨어가 요구하는 것)
| 항목 | 위치 | 소비자 | 근거 | 재현 방법 |
|---|---|---|---|---|
| cmdline | PA 0x80033000, `"kemco"+문자열` | setup_arch | 커널 코드, 스냅샷 원문 | 보드 |
| SPI 타깃 = CPU0 | GICD_ITARGETSR | Sony gic_dist_init(미설정) | 커널 코드 | 보드 스텁 |
| 보조코어 해제 | 0xC0005010+ / SEV | cxd90014_smp_init | 커널 코드, 실기 IRAM 덤프 | 보드 스텁 |
| ARM7 NAND 펌웨어 동작중 | SRAM+0x3F40=2 | lld.ko | lld 분석 | 보드(+ HLE) |
| NAND dev-info | SRAM+0x3F00 (13 words) | lld_probe | 스냅샷·실기 SRAM | 보드 |
| 불량블록표 / init_param | 0xC0000C00 / 0xC0001500 | nand.ko | 실기 IRAM 덤프 | 보드 |
| **MUMIN 마운트 상태** | PA 0x80000000, 128 KiB | nand.ko `boot_mode=0` → `muminblk_init(0x40000000,0x20000,1)` | IDA | **1단계 로더** |
| **Backup.bin 사본** | 헤더 word0 (0x82EC0000) | backup.ko `Backup_load=0` | IDA, 실행 확인 | **1단계 로더** |
| **av-cam.bin** | PA 0x83541000 | liro.ko `adr=` | IDA, 실행 확인 | **1단계 로더** |
| **커널 / initrd** | 0x80038000 / 0x80428000 | — | — | **1단계 로더 → 보드** |
| hand-off 페이지 | 0x80021000 (부트 사유 +0x4C 등) | upm.ko 등 | 스냅샷 | 보드 preload (`data/handoff_80021000.bin`, 스냅샷 값 — 콜드부트 값과 다를 수 있음 [?]) |

## 단계
1. **전원 ON / 리셋 (1단계)**: 보드가 호스트의 `vmlinux.bin` + `provision/loader.cpio`로 부팅.
   `loader.sh`가 무수정 Sony 모듈(devno/dmac/ldec/lld/nand `boot_mode=1`)로 에뮬레이션 NAND를 마운트하고:
   - `a5kcopy -mumin 0x3f03ad20`: nand.ko 장치 구조체(+0x34/+0x38)의 MUMIN 버퍼를 PA 0x80000000에 복사, 자기참조 포인터를 VA 0x40000000 기준으로 재배치
   - `/setting/Backup.bin` → 헤더의 PA, `/system/av-cam.bin` → 0x83541000
   - `/system/vmlinux.bin`, `initrd.img` → 0x90000000 / 0x90400000 스테이징 + 디스크립터 `{"A5KB", kpa, klen, ipa, ilen}` @ 0x80027000
   - `reboot -f` → 커널이 SYSCTL +0x44에 0x11 → SoC 리셋
2. **2단계**: 보드 리셋 핸들러(ROM 리셋 이후 실행)가 디스크립터를 보고 스테이징된 커널/initrd를 제자리로 옮긴 뒤 진입. 이후는 **전부 순정**: NAND에서 읽은 커널 → 순정 initrd → `/sbin/init` → …
3. 게스트가 재부팅하면 디스크립터가 없으므로 다시 1단계부터(실기처럼 매 부팅 부트로더 경유).

## 주의
- `boot_mode=1`은 마운트 실패 시 NAND 전체를 소거한다(업데이터와 동일 동작). NAND 파일은 `cp -c`(APFS 클론)로 백업해 두고 실험할 것.
- `0x3f03ad20`은 모듈 로드 순서가 순정 initrd와 같을 때의 nand.ko 주소(실기 스냅샷과 일치). 로드 순서를 바꾸면 달라진다.
