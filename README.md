# a5000 full-system emulator (`sony-a5000` QEMU machine)

목표: Sony ILCE-5000 공식 펌웨어 1.10을 **수정 없이**, 실기 없이, 실기와 동일하게 구동.

## 구성
| 경로 | 내용 |
|---|---|
| `qemu/` | QEMU v10.1.0 + a5000 머신 (`hw/arm/sony_a5000.c`, `hw/timer/cxd4115_timer.c`, `hw/misc/cxd90014_sysctl.c`, `hw/block/cxd90014_nand.c`, `target/arm/tcg/cpu32.c`의 cortex-a5) |
| `patches/qemu-v10.1.0-sony-a5000.patch` | 위 변경 전체 (QEMU 원본에 재적용용) |
| `run.sh` | 순정 커널+initrd로 부팅 (`A5K_STOCK_CONSOLE=1`이면 순정 `console=ttynull` 그대로) |
| `provision/` | 공장식 NAND 프로그래밍 initramfs (Sony FTL이 빈 NAND를 포맷 → `NAND.BIN` 기록) |
| `tools/` | `a5ktrace.c`(TCG 추적 플러그인: `cc -shared -fPIC -o tools/liba5ktrace.dylib tools/a5ktrace.c -Iqemu/include/qemu $(pkg-config --cflags --libs glib-2.0) -undefined dynamic_lookup`), `mon.py`(HMP), `klog.py`(Sony klog 링 읽기), `wbi.py`(실기 WBI 스냅샷 조회·페이지워크), `sym.py`(커널 심볼) |
| `re/` | 커널 심볼(`kernel_syms.txt`), ELF 복원본(`vmlinux.elf`), 실기 스냅샷 커널 로그 |
| `docs/HARDWARE.md` | SoC 모델 전체와 각 값의 출처(V/S/H) |

## 빌드 / 실행
```sh
cd emulator/qemu/build && ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
    --enable-debug-info --disable-sdl --enable-cocoa --disable-gtk --disable-fuse && ninja qemu-system-arm
emulator/run.sh -nographic                     # 시리얼 콘솔 (Ctrl-A X 종료)
emulator/run.sh -display none -serial stdio -s # + gdb: arm-none-eabi-gdb emulator/re/vmlinux.elf -ex 'target remote :1234'
```

## 현재 상태 (2026-11-14)
- 주의: 엔진 작업 시 `A5K_STOCK_CONSOLE=1` (디버그 시리얼 콘솔이 av-cam의 UART0 모니터와 충돌)
- ✅ 쿼드 Cortex-A5(VFPv4-D16) · GIC · L2C-310 · CXD4115 타이머/WDT · SYSCTL · PL011 · 클럭 생성기 · USB OTG/HDC 리셋 · BOSS DMAC · GPU(SMAPH-S30: fill/transfer, 3D는 완료 신호만)
- ✅ NAND: ARM7 NAND 펌웨어 HLE(메일박스, LDEC LZ77) + raw NAND 백킹 파일. `provision.sh`로 `backup/NAND.BIN`을 Sony FTL 경유 기록
- ✅ 실기 방식 2단계 부팅(`docs/BOOT.md`): 부트로더의 NAND 작업을 Sony NAND 스택으로 재현 → **NAND에서 읽은 커널**로 순정 부팅
- ✅ 순정 init → 압축 rootfs·/usr 마운트 → `/usr/kmod` 33종 → Android 시작 → **LIRO가 카메라 엔진 av-cam.bin 기동** → im.elf가 입력장치 생성, GLES 초기화
- ✅ 엔진 하드웨어 1차: SYSV 프레임 타이머·VPH vsync 60 Hz(IRQ 98/101/102), SIO 5채널, GPIO 18포트, ADC 2유닛 → VDF가 패널(640×480)을 설정
- ✅ av-cam 메모리 DMA(엔진 .data 초기화 복구), 서브마이컴 HLE(전송 계층, 실기 프레임 재생, `docs/RE_SUBMICRO_LINK.md`) → 상태 프레임 교환 동작
- ✅ 카메라 타이밍 이벤트 블록 0xF3C30000(IRQ 83/84: 엔진 프레임 29.97 fps + 1 MHz 비교기) → 엔진 프레임 루틴·int_HS·**서브마이컴 slot A 교환 동작**
- ✅ GIC SPI 타깃을 실기 GICD 덤프 값으로 → CPU1~3 틱·RCU 정상 → umount/i1 정지 해소 → **i0/i1/i2·appFw 초기화 완료**
- ✅ BOSS LDEC LZ77 디코더 메모리 모드(/dev/ldec) → appFw abort 해소. Rigel WindowServer·appFw가 **프레임 렌더링 중**, Android도 기동
- ✅ CPU별 hwtimer(타이머 ch5..8) 자유 계수 수정 → liro dly_tsk 정상 → VPH가 **패널 OSD 레이어를 프레임마다 설정**(ch53 = UI 버퍼)
- ✅ **GPU P3D 명령 리스트 실행**(디코더·래스터라이저·텍스처·블렌딩, `docs/RE_GPU_P3D_CMD.md`) + 셰이더 인터프리터(진행 중) + 패널 OSD RGBA4444 → **순정 UI 첫 화면(지역/날짜/시간 설정 대화상자)이 QEMU 창에 표시**
- ⏳ Rigel 윈도우 합성 셰이더 일부 opcode 미구현, 깊이/스텐실·선/점·밉맵 미구현 상세 `docs/RE_ENGINE_TICK.md`
- ✅ SD 카드: 서브마이컴 슬롯 스위치 모델 → 순정 미디어 스택이 카드 초기화·FAT 읽기 (`-drive if=sd` / `A5K_SD`)
- ✅ 카메라 버튼(`docs/RE_ENGINE_TICK.md` §11): 휠·가운데 = ADC 래더 ch 0x0D, DELETE = ch 0x0C, MOVIE = GPIO P10.10, MENU/PLAY = 서브마이컴 slot A 바이트 28 (호스트: 방향키/Enter/M/P/R/Delete, HMP `sendkey`)
- ✅ 첫 부팅 설정 완주: 지역 설정(세계 지도) → 날짜/시간 → PlayMemories 안내 → 촬영 모드 (SD 없음: 렌즈 경고 → 촬영 OSD)
- ⏳ SD 있음: 이미지 DB 자동 복구 뷰(추정)에서 빈 화면 / 촬영 모드에서 메뉴·휠 무반응 — 조사 중
- ⬜ 센서/ISP(라이브뷰), 렌즈, 오디오

## 사용
```sh
emulator/provision.sh            # 최초 1회: 빈 NAND 포맷 + NAND.BIN 기록 → state/a5000.nand
emulator/run.sh -nographic       # 실기 방식 부팅 (시리얼 콘솔)
emulator/run.sh -monitor stdio   # QEMU 창에 fb= 디버그 화면
```
