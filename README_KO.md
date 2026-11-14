# a5000 full-system emulator (`sony-a5000` QEMU machine)

[English](README.md) | **한국어**

목표: Sony ILCE-5000 공식 펌웨어 1.10을 **수정 없이**, 실기 없이, 실기와 동일하게 구동.

## 구성
| 경로 | 내용 |
|---|---|
| `qemu/` | **이 저장소에 없음**(git 제외된 로컬 체크아웃, 아래 *QEMU 받기* 참고). QEMU v10.1.0 + a5000 머신 (`hw/arm/sony_a5000.c`, `hw/timer/cxd4115_timer.c`, `hw/misc/cxd90014_sysctl.c`, `hw/block/cxd90014_nand.c`, `target/arm/tcg/cpu32.c`의 cortex-a5) |
| `qemu-overlay/` | **QEMU에 새로 추가한 소스 전부**(머신·장치 모델·GPU)를 QEMU 경로 그대로 — 에뮬레이터 코드는 여기서 읽으면 됨 |
| `patches/qemu-v10.1.0-sony-a5000.patch` | 기존 업스트림 파일의 작은 수정만 (빌드 등록, Cortex-A5, A9MPCore, L2C-310) |
| `setup-qemu.sh` | `qemu/` = 업스트림 v10.1.0 + 오버레이 + 패치 생성 |
| `run.sh` | 순정 커널+initrd로 부팅 (기본 = 순정 `console=ttynull`; `A5K_DEBUG_CONSOLE=1` = 부팅 디버그용 시리얼 커널 콘솔) |
| `provision/` | 공장식 NAND 프로그래밍 initramfs (Sony FTL이 빈 NAND를 포맷 → `NAND.BIN` 기록) |
| `tools/` | `mkdata.py`(업데이터에서 `data/` 생성), `a5ktrace.c`(TCG 추적 플러그인: `cc -shared -fPIC -o tools/liba5ktrace.dylib tools/a5ktrace.c -Iqemu/include/qemu $(pkg-config --cflags --libs glib-2.0) -undefined dynamic_lookup`), `mon.py`(HMP), `klog.py`(Sony klog 링 읽기), `wbi.py`(실기 WBI 스냅샷 조회·페이지워크), `sym.py`(커널 심볼) |
| `re/` | 커널 심볼(`kernel_syms.txt`). ELF 복원본(`vmlinux.elf`)·실기 커널 로그는 로컬 전용(비공개) |
| `docs/HARDWARE.md` | SoC 모델 전체와 각 값의 출처(V/S/H) |

아래 명령은 모두 저장소 루트에서 실행합니다.

## 직접 준비할 입력 (이 저장소에 없음)
| 경로 | 내용 |
|---|---|
| `firmware/unpacked/...` | Sony 공식 업데이터 `Update_ILCE5000V110.exe`를 푼 것 (fwtool 구조, 또는 `A5K_FW` 지정) |
| `data/NAND.BIN` | **본인 소유** 카메라의 NAND 덤프 (또는 `provision.sh`에 경로 인자로) |
| `data/*.bin`, `data/wbi/` | 공장 RAM 스냅샷·hand-off 페이지·서브마이컴 프레임 — 업데이터에서 `tools/mkdata.py`로 생성 |

단계별 방법: [`data/README.md`](data/README.md).

## QEMU 받기
`qemu/` 폴더는 **공개하지 않습니다**. 업스트림 QEMU `v10.1.0`과 이 저장소로 다시 만듭니다:
```sh
./setup-qemu.sh     # v10.1.0을 qemu/에 클론 → qemu-overlay/ 복사 → 패치 적용
```
`qemu/`(브랜치 `sony-a5000`)에서 개발할 때는 QEMU 커밋마다 `tools/export-qemu.sh`로 `qemu-overlay/`와 패치를 갱신합니다.

## 현재 상태 (2026-11-14)
- 주의: 디버그 시리얼 콘솔(`A5K_DEBUG_CONSOLE=1`)은 순정 cmdline을 바꾸고 UART0를 av-cam 모니터와 공유함 — UI는 이것 없이만 검증됨
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
./provision.sh            # 최초 1회: 빈 NAND 포맷 + NAND.BIN 기록 → state/a5000.nand
./run.sh -monitor stdio   # 실기 방식 부팅, QEMU 창 = 카메라 패널
```

## 펌웨어 · 법적 고지
- 이 저장소에는 **Sony 펌웨어·바이너리·기기 덤프가 들어 있지 않습니다.** 실행하려면 **본인 소유 기기**에서 직접 추출한 NAND 덤프와 Sony가 공개 배포한 공식 업데이터에서 얻은 파일이 필요합니다(`data/README.md`). 이 파일들을 저장소나 이슈에 올리지 마세요.
- 이 프로젝트는 Sony Group Corporation과 **무관**하며 Sony가 승인·후원하지 않습니다. "Sony", "α", "ILCE", "BIONZ"는 각 소유자의 상표입니다.
- 문서는 상호운용성(에뮬레이션) 목적의 역설계 관찰 기록입니다. 배터리 인증 등 보호 기능의 분석·우회는 범위 밖입니다.
- 실기기 작업은 본인 책임입니다. 이 에뮬레이터는 실기에 아무것도 쓰지 않습니다.

## 라이선스
- QEMU 코드(`qemu-overlay/`, `patches/`)와 `tools/`·`provision/`·스크립트: **GPL-2.0-or-later** (`LICENSE`, QEMU와 동일)
- 문서(`docs/`, `*.md`): **CC BY 4.0**
- QEMU 자체는 원 저작권자의 라이선스(`qemu/COPYING`, `qemu/LICENSE`)를 따릅니다.
