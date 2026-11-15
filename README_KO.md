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
| `run.sh` | 순정 커널+initrd로 부팅 (기본 = 순정 `console=ttynull`; `A5K_SHELL=<port>` = UART2의 루트 셸을 TCP 소켓으로, `A5K_DEBUG_CONSOLE=1` = 같은 콘솔을 stdio로) |
| `provision/` | 공장식 NAND 프로그래밍 initramfs (Sony FTL이 빈 NAND를 포맷 → `NAND.BIN` 기록) |
| `tools/` | `mkdata.py`(업데이터에서 `data/` 생성), `a5ktrace.c`(TCG 추적 플러그인: `cc -shared -fPIC -o tools/liba5ktrace.dylib tools/a5ktrace.c -Iqemu/include/qemu $(pkg-config --cflags --libs glib-2.0) -undefined dynamic_lookup`), `mon.py`(HMP), `klog.py`(Sony klog 링 읽기), `a5kshell.py`(게스트 셸 + 파일 push/pull), `wbi.py`(실기 WBI 스냅샷 조회·페이지워크), `sym.py`(커널 심볼) |
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

## 현재 상태 (2026-11-15)
- ✅ 순정 UI 옆의 게스트 루트 셸(`docs/RE_GUEST_SHELL.md`): `console=ttyAM1` + `shell-socket=`(UART2), `tools/a5kshell.py` push/pull과 md5 확인. `/android/a5k`에 push한 정적 ARM 바이너리가 촬영 화면이 떠 있는 채로 실행됨. `debug-console=on`은 이제 UART2로 연결(UART0 = av-cam 모니터 포트; `console=ttyAM0`은 UI가 뜨지 않는다고 경고)
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
- ✅ GPU: 첫 부팅~촬영 화면의 draw 779개 전부 렌더링(Rigel 합성 포함). 깊이/스텐실·밉맵·border wrap 추가, 합성 명령 리스트로 테스트(`tools/test_dmp_p3d.py`, `tools/test_dmp_shader.c`)
- ⏳ GPU: geometry 프로그램(실제 점/선), GLES1 패키지 셰이더 opcode 14개, ETC1, 깊이/밉 메모리 배치 미확정 (`docs/RE_GPU_P3D_CMD.md` §7)
- ✅ SD 카드: 서브마이컴 슬롯 스위치 모델 → 순정 미디어 스택이 카드 초기화·FAT 읽기 (`-drive if=sd` / `A5K_SD`)
- ✅ 카메라 버튼(`docs/RE_ENGINE_TICK.md` §11): 휠·가운데 = ADC 래더 ch 0x0D, DELETE = ch 0x0C, MOVIE = GPIO P10.10, MENU/PLAY = 서브마이컴 slot A 바이트 28 (호스트: 방향키/Enter/M/P/R/Delete, HMP `sendkey`)
- ✅ 서브마이컴 RTC (`docs/RE_RTC.md` §9): 카메라 시계 = 호스트 시간(`-rtc base=`), 메뉴에서 설정한 날짜는 응답(ack)되고 세션 동안 유지 → 일반 부팅은 바로 촬영 화면
- ✅ 첫 부팅 설정(`rtc-lost=on`) 완주: 지역 설정(세계 지도) → 날짜/시간 → PlayMemories 안내 → 촬영 OSD
- ⏳ 렌즈 (`docs/RE_LENS_EMOUNT.md` §8): `lens=none` = 펌웨어의 빈 마운트 대기 상태, `lens=ekit` = 렌즈 전원 응답 + 핸드셰이크 0–1단계 후 UART1 렌즈 프로토콜(미구현) 직전에서 정지
- ✅ 셔터 대기 레벨 수정(카메라가 S1/S2를 누른 채로 부팅하고 있었음): 촬영 모드에서 MENU·휠 가운데 버튼 동작, 셔터·줌 레버 연결 (`docs/RE_ENGINE_TICK.md` §12)
- ⏳ SD 있음: 이미지 DB 자동 복구 뷰(AUTOREPAIR)가 빈 화면, 메뉴의 포맷도 멈춤 — 둘 다 SD 명령을 내지 않음, 조사 중
- ✅ 업데이터 모드: 에뮬레이션 NAND에 `/setting/updater/mode`가 있으면 1단계 로더가 부트로더처럼 업데이터 코어(nflasha1)로 부팅 → 루트 셸·UFP USB 대기까지 (`docs/RE_UPDATER_BOOT.md`)
- ⏳ 카메라 프런트엔드: 센서 라이터(P2S, IRQ 38)·프런트엔드 프레임 IRQ·AIF 채널 그룹(IRQ 40–50) 모델 → 캡처 오류 해소, 라이브뷰는 아직 검정(YC 출력 요청 없음, `docs/RE_SENSOR_ISP.md` §7)
- ✅ SA DSP 코어: null 모델(`cxd90014_sa.c`)로 센서 결함표 작업 완료 → 엔진이 모니터링 상태 도달, 촬영 화면에 노출 표시(셔터/조리개 눈금). 라이브뷰는 YC 상태 2 전에서 정지(`docs/RE_LIVEVIEW_REQUEST.md` §9), PLAY·SD 동작: 카드 없음 재생 화면, 카드가 있으면 이미지 DB 생성 대화상자 → 카드에 DB 생성(첫 SD 쓰기) (`docs/RE_PLAYBACK_SWITCH.md`)
- ✅ 라이브뷰 경로: SYSV를 마스터 V 클럭으로 바꾼 뒤 펌웨어가 패널 YUV 레이어를 켜고 ch51을 매 프레임 설정 (`docs/RE_LIVEVIEW_PIPELINE.md`). ch51은 8비트 휘도 평면(1024×516)을 패널로 스케일; 채우는 ISP 모델이 아직 없어 렌즈 캡을 닫은 것처럼 검정; `-global cxd90014-aif.lv-source=bars|ramp|file:PATH.pgm`이면 리사이즈 출력 시점에 그 버퍼로 테스트 휘도 프레임을 기록(디버그, `docs/RE_LIVEVIEW_IMAGE.md`)
- ✅ USB 디바이스 (`docs/RE_USB_DEVICE_CONTROLLER.md` §9, `docs/RE_USB_HOST_PATH.md` §9): F_USB20HDC + HDMAC + VBUS 글루 모델과 TCP 가상 케이블("A5KU") → 카메라가 열거됨(MTP 054C:08D3), `pmca-console info -d socket`이 ILCE-5000 / 1.10 출력, `install -f <apk>`로 앱 설치 → 애플리케이션 목록에 표시되고 실행됨(`docs/RE_USB_APP_INSTALL.md`), 업데이터 코어가 054C:03E2로 열거되어 PMCA 업데이터 셸 실행. SWITCH_MODE 후 실기처럼 전원이 꺼지고(서브마이컴 전원 끄기 핸드셰이크, DDR 컨트롤러, p0.10 전원 끄기 라인) 서브마이컴이 전원을 다시 넣어 업데이터로 부팅 → `pmca-console updatershell -d socket`이 수동 리셋 없이 업데이터 셸까지 도달(두 번째 PMCA 실행은 `-m ILCE-5000`, `docs/RE_REBOOT.md`). 머신 옵션 `poff=auto|restart|off`(기본 `auto`)로 전원 끄기 라인의 동작을 선택: `auto`는 CPU가 slot-A TX 프레임에 보낸 끄기 데이터를 따름(+0x15 = 0x20 → 전원 재투입, 그 외에는 꺼진 상태 유지 = VM suspended, `info status` → `paused (suspended)`; 전원 레버 `O`, 자동 전원 끄기 후에는 `S`/`M`/`P`도, 또는 HMP `system_wakeup`으로 콜드 부팅하여 다시 켜짐, `docs/RE_REBOOT.md` B.7).
- ⬜ 센서/ISP(라이브뷰), 렌즈 시리얼 프로토콜, 오디오

## 사용
```sh
./provision.sh            # 최초 1회: 빈 NAND 포맷 + NAND.BIN 기록 → state/a5000.nand
./run.sh -monitor stdio   # 실기 방식 부팅, QEMU 창 = 카메라 패널
./run.sh -M rtc-lost=on   # 머신 옵션: rtc-lost=on = 시계 백업 소실(첫 부팅 지역/날짜/시간 설정), lens=none|ekit = 빈 마운트(기본)/킷 렌즈(전원+핸드셰이크만); 시계 = QEMU RTC(-rtc base=...)
```

### 키 배정 (QEMU 창)
| 호스트 키 | 카메라 버튼 | 에뮬레이션 경로 |
|---|---|---|
| Enter (키패드 Enter 포함) | 컨트롤 휠 가운데 (확인) | ADC 유닛 1 ch 1 (ch 0x0D) 래더, 레벨 45 |
| ↑ / ← / ↓ / → | 컨트롤 휠 위 / 왼쪽 / 아래 / 오른쪽 | 같은 래더, 레벨 182 / 356 / 520 / 686 |
| Delete / Backspace (Mac: ⌫) | 삭제(휴지통) | ADC 유닛 1 ch 0 (ch 0x0C) 래더, 레벨 45 |
| `M` | MENU | 서브마이컴 slot A 프레임 바이트 28 비트 2 |
| `P` | 재생 | 서브마이컴 slot A 프레임 바이트 28 비트 1 |
| `R` | MOVIE (동영상 녹화) | GPIO P10.10, active low |
| `O` | 전원 레버 (슬라이드 스위치: 누를 때마다 OFF / ON 전환, 시작 시 ON) | 서브마이컴 slot A 프레임 바이트 28 비트 0 (1 = ON); 카메라가 꺼진 동안 레버 ON = 전원 켜짐 |
| `S` | 셔터 반누름 (S1, 키 0x1F1) | GPIO P0.1 + P0.11 low (active low) |
| `Shift+S` 또는 `Space` | 셔터 완전 누름 (S1 + S2, 키 0xA7) | 추가로 GPIO P1.5 + P0.13 low |
| `T` / `W` | 줌 레버 망원 / 광각 (저속) | ADC 유닛 1 ch 3 (ch 0x0F), 레벨 478 / 400 (값 1 / 0x8001) |
| `Shift+T` / `Shift+W` | 줌 레버 망원 / 광각 (고속) | 같은 채널, 레벨 162 / 301 (값 2 / 0x8002) |
| — | 컨트롤 휠 회전 | 아직 미연결 (av-cam JogTpu 카운터) |

HMP 모니터에서도 입력 가능: `sendkey ret`, `sendkey down`, `sendkey m`, …
QEMU 자체 단축키: Ctrl+Opt+G = 마우스 해제, Ctrl+Opt+1 / 2 = 패널(console 0) / 디버그 프레임버퍼 화면 전환.
첫 부팅 대화상자가 뜬 뒤 약 20초 동안 누른 키는 펌웨어가 무시합니다(입력 서비스가 아직 비활성).

### USB (가상 케이블)
USB 디바이스 컨트롤러(`cxd90014-usbhdc`, `docs/RE_USB_DEVICE_CONTROLLER.md`)가 카메라의 실제 USB 가젯을 "A5KU" 프로토콜(`docs/RE_USB_HOST_PATH.md` §5)로
TCP에 내보낸다. TCP 클라이언트가 접속하면 케이블이 꽂힌 것(VBUS on), 접속이 끊기면 케이블을 뽑은 것이다. 클라이언트가 없으면 카메라는
지금처럼 케이블 없음 상태다. 게스트 재부팅 중에도 케이블은 꽂힌 채 유지된다.
```sh
./run.sh -M usb-socket=127.0.0.1:7643        # 또는 -chardev socket,id=usb0,host=127.0.0.1,port=7643,server=on,wait=off -global cxd90014-usbhdc.chardev=usb0
```
호스트 쪽: `tools/pmca-socket/`의 `socket` 백엔드를 붙인 Sony-PMCA-RE (해당 README 참고):
```sh
python3 pmca-console.py info -d socket       # A5K_USB=host:port (기본 127.0.0.1:7643), A5K_USB_TIMEOUT=ms, A5K_USB_DEBUG=1
```

### 게스트 셸 (트랙 B)
순정 UI가 도는 상태에서 게스트 안의 루트 셸(`docs/RE_GUEST_SHELL.md`). `A5K_SHELL=<port>`는 머신 옵션 `console=ttyAM1`을 켠다:
순정 cmdline에서 `console=ttynull` 토큰 하나만 `console=ttyAM1 amba2.console=1`로 바뀌고, 순정 시스템이 쓰지 않는 UART2가 TCP
소켓으로 나온다. 그러면 순정 init이 그 포트에서 자체 루트 `ash`를 띄우고 UI는 평소대로 뜬다. 옵션이 없으면 cmdline은 그대로다.

> **에뮬레이터 전용 개발 도구.** 실기에는 접근 가능한 UART가 없어서 실기의 CFW·배포는 전부 USB로 한다. 이 셸은 에뮬레이터
> 안에서 살펴보고 반복 작업할 때만 쓰고, 실제 배포 흐름은 USB 가상 케이블(`pmca-console`)로 시험한다.
```sh
A5K_SHELL=46310 ./run.sh                      # = -M console=ttyAM1,shell-socket=127.0.0.1:46310 -serial null
python3 tools/a5kshell.py --port 46310        # 대화형 (Ctrl-] 종료); --job-control 이면 Ctrl-C 동작
python3 tools/a5kshell.py --port 46310 push hello            # -> /android/a5k/hello (tmpfs, 실행 가능), md5 확인
python3 tools/a5kshell.py --port 46310 run '/android/a5k/hello; echo rc=$?'
python3 tools/a5kshell.py --port 46310 pull /android/a5k/out.bin out.bin
```
- 전송은 순정 busybox만 쓴다(`stty raw` + `cat`, 약 50 KB/s). md5로 확인한다. `/android` 밖으로의 push는 거부한다(`--force`
  필요). `/android`는 tmpfs라서 push한 파일은 전원이 꺼지면 사라진다.
- 기타 머신 옵션: `console=ttynull|ttyAM1|ttyAM0`, `debug-console=on`(= `console=ttyAM1`; `run.sh`에서는 `A5K_DEBUG_CONSOLE=1`
  이면 stdio로). `shell-socket=` 없이 쓰면 UART2는 세 번째 `-serial`이다(`-serial null -serial null -serial tcp:...`).
  `console=ttyAM0`(`A5K_DEBUG_CONSOLE=ttyAM0`)은 초기 부팅 로그 전용: UART0는 av-cam 모니터 포트라서 UI가 뜨지 않는다.
- 키 입력 없이 약 5분이 지나면 펌웨어의 유휴 절전이 카메라를 끈다(전원 레버 `O`, `S`/`M`/`P` 또는 HMP
  `system_wakeup`으로 다시 켤 때까지 VM suspended). 셸 세션과 `/android/a5k`는 사라진다.
  `a5kshell.py`는 이를 알려준다(종료 코드 3). HMP로 가끔 무해한 키를 보내거나, 그 시간 안에 작업한다.

## 펌웨어 · 법적 고지
- 이 저장소에는 **Sony 펌웨어·바이너리·기기 덤프가 들어 있지 않습니다.** 실행하려면 **본인 소유 기기**에서 직접 추출한 NAND 덤프와 Sony가 공개 배포한 공식 업데이터에서 얻은 파일이 필요합니다(`data/README.md`). 이 파일들을 저장소나 이슈에 올리지 마세요.
- 이 프로젝트는 Sony Group Corporation과 **무관**하며 Sony가 승인·후원하지 않습니다. "Sony", "α", "ILCE", "BIONZ"는 각 소유자의 상표입니다.
- 문서는 상호운용성(에뮬레이션) 목적의 역설계 관찰 기록입니다. 배터리 인증 등 보호 기능의 분석·우회는 범위 밖입니다.
- 실기기 작업은 본인 책임입니다. 이 에뮬레이터는 실기에 아무것도 쓰지 않습니다.

## 라이선스
- QEMU 코드(`qemu-overlay/`, `patches/`)와 `tools/`·`provision/`·스크립트: **GPL-2.0-or-later** (`LICENSE`, QEMU와 동일)
- 문서(`docs/`, `*.md`): **CC BY 4.0**
- QEMU 자체는 원 저작권자의 라이선스(`qemu/COPYING`, `qemu/LICENSE`)를 따릅니다.
