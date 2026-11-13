# a5000 full-system emulator (`sony-a5000` QEMU machine)

목표: Sony ILCE-5000 공식 펌웨어 1.10을 **수정 없이**, 실기 없이, 실기와 동일하게 구동.

## 구성
| 경로 | 내용 |
|---|---|
| `qemu/` | QEMU v10.1.0 + a5000 머신 (`hw/arm/sony_a5000.c`, `hw/timer/cxd4115_timer.c`, `hw/misc/cxd90014_sysctl.c`, `hw/block/cxd90014_nand.c`, `target/arm/tcg/cpu32.c`의 cortex-a5) |
| `patches/qemu-v10.1.0-sony-a5000.patch` | 위 변경 전체 (QEMU 원본에 재적용용) |
| `run.sh` | 순정 커널+initrd로 부팅 (`A5K_STOCK_CONSOLE=1`이면 순정 `console=ttynull` 그대로) |
| `provision/` | 공장식 NAND 프로그래밍 initramfs (Sony FTL이 빈 NAND를 포맷 → `NAND.BIN` 기록) |
| `tools/` | `mon.py`(HMP), `klog.py`(Sony klog 링 읽기), `wbi.py`(실기 WBI 스냅샷 조회·페이지워크), `sym.py`(커널 심볼) |
| `re/` | 커널 심볼(`kernel_syms.txt`), ELF 복원본(`vmlinux.elf`), 실기 스냅샷 커널 로그 |
| `docs/HARDWARE.md` | SoC 모델 전체와 각 값의 출처(V/S/H) |

## 빌드 / 실행
```sh
cd emulator/qemu/build && ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
    --enable-debug-info --disable-sdl --enable-cocoa --disable-gtk --disable-fuse && ninja qemu-system-arm
emulator/run.sh -nographic                     # 시리얼 콘솔 (Ctrl-A X 종료)
emulator/run.sh -display none -serial stdio -s # + gdb: arm-none-eabi-gdb emulator/re/vmlinux.elf -ex 'target remote :1234'
```

## 현재 상태 (2026-11-13)
- ✅ 쿼드 Cortex-A5(VFPv4-D16) · GIC · L2C-310 · CXD4115 타이머/WDT · SYSCTL · PL011
- ✅ 커널 3.0.27 부팅 → initrd(ext2) → Sony `/sbin/init` → 커널 모듈(devno/dmac/ldec/lld/nand) 로드
- ⏳ NAND: ARM7 NAND 펌웨어(백업에 없음) 대신 lld.ko 메일박스 프로토콜 HLE 구현 중 → 이후 프로비저닝으로 실기 NAND 재현
- ⬜ 그 다음: `/usr` 커널 모듈 33종 장치(i2c/sircs/input/hdmi/mmc/usb/GPU grm_gles…), im.elf 카메라 엔진, 디스플레이/키 입력, 센서·ISP
