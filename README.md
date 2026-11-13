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
- 주의: 엔진 작업 시 `A5K_STOCK_CONSOLE=1` (디버그 시리얼 콘솔이 av-cam의 UART0 모니터와 충돌)
- ✅ 쿼드 Cortex-A5(VFPv4-D16) · GIC · L2C-310 · CXD4115 타이머/WDT · SYSCTL · PL011 · 클럭 생성기 · USB OTG/HDC 리셋 · BOSS DMAC · GPU(SMAPH-S30: fill/transfer, 3D는 완료 신호만)
- ✅ NAND: ARM7 NAND 펌웨어 HLE(메일박스, LDEC LZ77) + raw NAND 백킹 파일. `provision.sh`로 `backup/NAND.BIN`을 Sony FTL 경유 기록
- ✅ 실기 방식 2단계 부팅(`docs/BOOT.md`): 부트로더의 NAND 작업을 Sony NAND 스택으로 재현 → **NAND에서 읽은 커널**로 순정 부팅
- ✅ 순정 init → 압축 rootfs·/usr 마운트 → `/usr/kmod` 33종 → Android 시작 → **LIRO가 카메라 엔진 av-cam.bin 기동** → im.elf가 입력장치 생성, GLES 초기화
- ✅ 엔진 하드웨어 1차: SYSV 프레임 타이머·VPH vsync 60 Hz(IRQ 98/101/102), SIO 5채널, GPIO 18포트, ADC 2유닛 → VDF가 패널(640×480)을 설정
- ⏳ **UI 첫 화면의 관문 = 서브마이컴(전원 마이컴) 링크**: im.elf i1 스레드가 `libmprctrl` 초기화에서 upm.ko 전원 상태 질의로 대기 → upm 값은 서브마이컴 캐시. 링크(SIO ch0 + GPIO 핸드셰이크)는 아직 미개통(`-M ...,submicro=on` 실험 중). `docs/RE_UI_BRINGUP.md`, 부팅 체크포인트는 PA 0x834C0000의 boot_time 로그
- ⏳ 화면: VPH 스캔아웃 모델(콘솔 0) 준비됨, UI가 아직 OSD를 보내지 않음. GPU 3D 커맨드 실행 미구현(완료 신호만)
- ⬜ SD 카드(SDHCI), 센서/ISP(라이브뷰), 렌즈, 서브마이컴(전원/키), 오디오

## 사용
```sh
emulator/provision.sh            # 최초 1회: 빈 NAND 포맷 + NAND.BIN 기록 → state/a5000.nand
emulator/run.sh -nographic       # 실기 방식 부팅 (시리얼 콘솔)
emulator/run.sh -monitor stdio   # QEMU 창에 fb= 디버그 화면
```
