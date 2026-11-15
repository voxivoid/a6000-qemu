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
| `tools/` | `mkdata.py`(업데이터에서 `data/` 생성), `a5ktrace.c`(TCG 추적 플러그인: `cc -shared -fPIC -o tools/liba5ktrace.dylib tools/a5ktrace.c -Iqemu/include/qemu $(pkg-config --cflags --libs glib-2.0) -undefined dynamic_lookup`), `mon.py`(HMP), `klog.py`(Sony klog 링 읽기), `a5kshell.py`(게스트 셸 + 파일 push/pull), `wbi.py`(실기 WBI 스냅샷 조회·페이지워크), `sym.py`(커널 심볼), `osdpoke.c`(트랙 B 경로(b) 픽셀경로 PoC: freestanding ARM, `/dev/mem`로 panel-OSD 프레임버퍼에 사각형 쓰기), `a5kinject.c`+`inj_so.c`(트랙 B 경로(b) ptrace 주입기+payload: `__libc_dlopen_mode`로 실행 중 im.elf에 `.so` 로드→im.elf 내부에서 사각형 그리기), `supp_so.c`(트랙 B 경로(b) §4: send_update 훅으로 순정 OSD draw 억제→CPU ~42% 회수+OSD 지속, 리부트 복구), `mdl_so.c`(트랙 B 경로(b) §2: 순정 viewManagerIf::getCurrentFuncEventMode로 엔진/뷰 상태 인프로세스 READ), `gate2_so.c`+`gate_so.c`(트랙 B 경로(b) §4: 가역 스톡 draw 게이트→CPU ~29% 회수·detach-가역), `mset_so.c`(§2: 인프로세스 requestModelExecute 안전호출) |
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
- ✅ 렌즈 시리얼 HLE (`docs/RE_LENS_EMOUNT.md` §9, §10): `lens=none` = 펌웨어의 빈 마운트 대기 상태(BL 상태 0, "렌즈 없음"), `lens=ekit` = 킷 렌즈 MCU 모델(`a5k-lens`) — 렌즈 전원 응답 + 마운트 핸드셰이크 에코 후 **UART1**로 E마운트 시리얼 프로토콜 수행(`cxd90014-mdma`의 새 주변장치-페이스 모드, ch0 RX 링 / ch1 TX). 능력 비트맵(블록 1)·렌즈 정보(7)·시작 정보(9)와 블록 0x0b/0x08/0x0d/0x10/0x0a까지 시작 핸드셰이크 전체를 완료 → 펌웨어가 렌즈를 인식하고 SYNC-TIMEOUT/DETACH 없이 BL 상태 0x05로 안정. 아직 "F--"/초점거리 미표시인 라이브뷰 필드는 주기적 Info1/2 프레임과 렌즈 정보 조리개 바이트에서 오는데 이는 **미해독**(`docs/RE_LENS_EMOUNT.md` §9.8)
- ✅ 셔터 대기 레벨 수정(카메라가 S1/S2를 누른 채로 부팅하고 있었음): 촬영 모드에서 MENU·휠 가운데 버튼 동작, 셔터·줌 레버 연결 (`docs/RE_ENGINE_TICK.md` §12)
- ⏳ SD 있음: 이미지 DB 자동 복구 뷰(AUTOREPAIR)가 빈 화면, 메뉴의 포맷도 멈춤 — 둘 다 SD 명령을 내지 않음, 조사 중
- ✅ 업데이터 모드: 에뮬레이션 NAND에 `/setting/updater/mode`가 있으면 1단계 로더가 부트로더처럼 업데이터 코어(nflasha1)로 부팅 → 루트 셸·UFP USB 대기까지 (`docs/RE_UPDATER_BOOT.md`)
- ⏳ 카메라 프런트엔드: 센서 라이터(P2S, IRQ 38)·프런트엔드 프레임 IRQ·AIF 채널 그룹(IRQ 40–50) 모델 → 캡처 오류 해소, 라이브뷰는 아직 검정(YC 출력 요청 없음, `docs/RE_SENSOR_ISP.md` §7)
- ✅ SA DSP 코어: null 모델(`cxd90014_sa.c`)로 센서 결함표 작업 완료 → 엔진이 모니터링 상태 도달, 촬영 화면에 노출 표시(셔터/조리개 눈금). 라이브뷰는 YC 상태 2 전에서 정지(`docs/RE_LIVEVIEW_REQUEST.md` §9), PLAY·SD 동작: 카드 없음 재생 화면, 카드가 있으면 이미지 DB 생성 대화상자 → 카드에 DB 생성(첫 SD 쓰기) (`docs/RE_PLAYBACK_SWITCH.md`)
- ✅ 라이브뷰 경로: SYSV를 마스터 V 클럭으로 바꾼 뒤 펌웨어가 패널 YUV 레이어를 켜고 ch51을 매 프레임 설정 (`docs/RE_LIVEVIEW_PIPELINE.md`). ch51은 8비트 휘도 평면(1024×516)을 패널로 스케일; 채우는 ISP 모델이 아직 없어 렌즈 캡을 닫은 것처럼 검정; `-global cxd90014-aif.lv-source=bars|ramp|file:PATH.pgm`이면 리사이즈 출력 시점에 그 버퍼로 테스트 휘도 프레임을 기록(디버그, `docs/RE_LIVEVIEW_IMAGE.md`)
- ⏳ 스틸 촬영(레버 OFF RAW 패스, `docs/RE_RAW_DEVELOP.md`): 1단계 로더가 부트로더 로드 테이블(Backup 0xFA0080)의 `\sabin\` 항목(`idt_cam.bin`, `sa_dfdet.bin`)을 적재 → IDT 초기화 성공, 콜드 부트 ZIT 결함 작업 성공; IDT 현상 블록(IRQ 72)·OPD 유닛(IRQ 67–70) 모델 → 레버 OFF 캡처가 RAW 현상 패스(플래그 22 0x40/0x400)와 IRQ 69 OPD 패스를 통과, 이후 태스크 74의 의도적 트랩(`439B193C`, 결과 처리, 미해석)에서 정지
- ✅ USB 디바이스 (`docs/RE_USB_DEVICE_CONTROLLER.md` §9, `docs/RE_USB_HOST_PATH.md` §9): F_USB20HDC + HDMAC + VBUS 글루 모델과 TCP 가상 케이블("A5KU") → 카메라가 열거됨(MTP 054C:08D3), `pmca-console info -d socket`이 ILCE-5000 / 1.10 출력, `install -f <apk>`로 앱 설치 → 애플리케이션 목록에 표시되고 실행됨(`docs/RE_USB_APP_INSTALL.md`), 업데이터 코어가 054C:03E2로 열거되어 PMCA 업데이터 셸 실행. SWITCH_MODE 후 실기처럼 전원이 꺼지고(서브마이컴 전원 끄기 핸드셰이크, DDR 컨트롤러, p0.10 전원 끄기 라인) 서브마이컴이 전원을 다시 넣어 업데이터로 부팅 → `pmca-console updatershell -d socket`이 수동 리셋 없이 업데이터 셸까지 도달(두 번째 PMCA 실행은 `-m ILCE-5000`, `docs/RE_REBOOT.md`). 머신 옵션 `poff=auto|restart|off`(기본 `auto`)로 전원 끄기 라인의 동작을 선택: `auto`는 CPU가 slot-A TX 프레임에 보낸 끄기 데이터를 따름(+0x15 = 0x20 → 전원 재투입, 그 외에는 꺼진 상태 유지 = VM suspended, `info status` → `paused (suspended)`; 전원 레버 `O`, 자동 전원 끄기 후에는 `S`/`M`/`P`도, 또는 HMP `system_wakeup`으로 다시 켜짐, `docs/RE_REBOOT.md` B.7). 웜 오프(유휴 절전, 전원 레버)이고 `sus_error`가 0이면 DDR을 유지하고, 다음 전원 투입에서 실기의 HOT 부팅처럼 실행 중이던 시스템을 그대로 재개한다(같은 화면, uptime 계속, 1단계 없음; CPU 수정: MMU 켜기가 다음 분기부터 명령어 페치에 적용). `-M warm-resume=off`면 예전처럼 콜드 부팅(`docs/RE_WARM_RESUME.md` §7).
- ✅ NFC (`docs/RE_NFC.md` §4): SIO2에 붙은 보드 수준 FeliCa Plug 계열 태그(`a5k-nfc`). RFDET 유휴 high(부팅 시 폰이 있다고 오인하지 않음), 리더 명령 1개당 IRQ 125 한 번(스텁이 125를 엣지 트리거로 설정, H), 호스트 "가상 폰"(`nfc=<chardev>`: `field`, `cmd`, `read`, `tap`, `status`). tap은 펌웨어가 만든 NDEF 태그(AIB + `application/x-sony-pmm` Wi-Fi Direct 레코드 + PlayMemories AAR)를 읽고, 쓰기는 거부(`01A5`), 핸드오버로 PlayMemories Camera Apps가 열린 뒤 Wi-Fi 시작에서 멈춘다(Wi-Fi 모델 없음)
- ✅ HDMI (`docs/RE_HDMI.md` §6): SoC 내장 TX 코어(`cxd90014-hdmi`, 0xF2A24000) + CEC(`cxd90014-cec`, 0xF3C10000), IRQ 없이 폴링. `-M hdmi=on`(기본 off = 케이블 없음, 순정)으로 가상 1080p TV에 케이블을 연결하며, 런타임 핫플러그 가능(`qom-set /machine hdmi on`). 펌웨어가 HPD를 감지하고 TX의 DDC 마스터로 내장 범용 EDID를 읽어(`edid=<file>`로 교체) TX를 설정(PHY on, AVI VIC 16 = 1080p60)한 뒤 **라인** 출력으로 전환한다. 카메라 UI가 **QEMU 콘솔 1**(`screendump FILE vph 1`)에 1920×1080으로 표시되고, 패널은 그대로 콘솔 0. CEC는 논리 주소를 할당받고 가상 TV가 물리주소/전원상태 질의에 응답한다. 함께 확정: SYS_STAT +0x024 bit0은 OMAP4 P_STABLE이 아니라 0으로 읽히는 "busy" 플래그(전원 켜기 셀이 0이 될 때까지 대기). 케이블 없으면 동작 불변, CEC churn 없음
- ⬜ 센서/ISP(라이브뷰), 렌즈 조리개/초점/줌 내용(주기적 Info 프레임, 미해독), 오디오
- ⬜ OSD의 라이브 F값 (`docs/RE_LENS_EMOUNT.md` §11.7, 2026-11-16 실측): 표시 F / SDCom 파라미터 267은 **렌즈 바이트가 아니라** 라이브뷰/AE 경로에서 생성된다. `lens=ekit`는 인식되지만 av-cam은 시작 블록 8개만 요청하고 주기적 블록 5/6은 **한 번도** 요청하지 않음(MENU·셔터에도 불변) → OSD는 `F--` 유지, SDCom `0x1002` connect는 타임아웃(데몬 Scalar 비활성). `a5k-lens`에 라이브뷰/AE 모델링 후를 위한 inert `aperture`/`focal-length` 프로퍼티 추가

## 사용
```sh
./provision.sh            # 최초 1회: 빈 NAND 포맷 + NAND.BIN 기록 → state/a5000.nand
./run.sh -monitor stdio   # 실기 방식 부팅, QEMU 창 = 카메라 패널
./run.sh -M rtc-lost=on   # 머신 옵션: rtc-lost=on = 시계 백업 소실(첫 부팅 지역/날짜/시간 설정), lens=none|ekit = 빈 마운트(기본)/킷 렌즈(전원+핸드셰이크+UART1 시리얼 HLE, `docs/RE_LENS_EMOUNT.md` §9); 시계 = QEMU RTC(-rtc base=...)
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

HMP 모니터에서도 입력 가능: `sendkey ret 300`, `sendkey down 300`, `sendkey m 300`, … 키는 약 300 ms 눌러야 한다: av-cam은 게스트 시간으로 약 43 ms마다만 버튼을 읽어서, 기본 100 ms 입력은 자주 놓친다.
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

### NFC (가상 스마트폰)
보드의 NFC "One-touch" 칩(`a5k-nfc`, SIO2에 붙은 FeliCa Plug 계열 동적 Type 3 태그, `docs/RE_NFC.md` §4)은 항상
존재하며, 안테나에 폰이 없으면(기본) 실기처럼 유휴 상태 태그로 보인다. 호스트의 "폰"은 chardev로 줄 단위 프로토콜을 쓴다:
```sh
./run.sh -chardev socket,id=nfc,host=127.0.0.1,port=7644,server=on,wait=off -M sony-a5000,nfc=nfc
nc 127.0.0.1 7644
tap            # 필드 on → 칩 전원 대기 → 태그 읽기 → 500 ms 유지 → 필드 off ("tap 2000"은 2초 유지)
read           # AIB + NDEF 읽고 해석: aib ..., ndef ..., mime application/x-sony-pmm ssid=... pass=..., aar ..., done
cmd 06 01 80 00   # 리더 명령 하나 [cmd][n][블록 목록] -> resp <hex> | timeout (2초 내 응답 없음)
field on|off   # RF 필드 (RFDET P0.8 low / high)
status         # 전원, SEL, 필드, 마지막 set-parameter 프레임, IRQ 횟수
```
- 태그는 펌웨어가 NFC를 켠 동안(촬영/재생 화면, N 마크 표시)만 응답한다. MENU 안이나 NFC off 상태에서는 칩 전원이
  들어오지 않아 명령이 timeout된다. 응답 내용(Backup의 Wi-Fi Direct SSID/패스프레이즈, PlayMemories 앱 레코드)은
  모델이 아니라 순정 펌웨어가 만든다.
- 마지막 NDEF 블록을 읽으면 펌웨어가 핸드오버한다: UI가 촬영 화면에서 PlayMemories Camera Apps("액세스 지점 검색 중")로
  넘어가고, 아직 Wi-Fi 모델이 없으므로 "연결 가능한 액세스 지점을 찾을 수 없습니다"로 끝난다.
- 모델의 비동기 줄: `ev power on|off`, `ev setparam <hex>`. 트레이스: `-trace 'a5k_nfc_*'` (응답 트레이스에 태그 바이트,
  즉 카메라 Wi-Fi 자격 증명이 들어 있으니 로그는 로컬에만 둘 것).

### HDMI (가상 케이블)
SoC 내장 HDMI 송신기(`cxd90014-hdmi`)와 CEC 컨트롤러(`cxd90014-cec`)는 순정 `hdmi.ko` / `cec.ko`가 IRQ 없이
폴링한다(`docs/RE_HDMI.md`). 기본은 케이블 미연결이라 카메라는 이전과 동일하게 동작한다. `-M hdmi=on`으로 가상
1080p TV에 케이블을 꽂으며, 런타임 핫플러그가 된다.
```sh
./run.sh -M hdmi=on                              # 케이블 꽂은 채 부팅
# 또는 HMP 모니터에서 언제든지:
#   qom-set /machine hdmi on     (뽑기: ... off)
```
- HDMI 화면은 **QEMU 콘솔 1**(`-display` 창: Ctrl+Opt+2; 헤드리스: `screendump FILE vph 1`). 패널은 콘솔 0
  그대로. 케이블이 없으면 콘솔 1은 "신호 없음" 화면을 보여준다.
- 펌웨어는 내장 범용 1920×1080p60 EDID를 TX의 DDC 마스터로 읽는다. `-M hdmi-edid=FILE`(128/256바이트)로 실제
  모니터 EDID를 줄 수 있다. EDID에 CEC 물리주소가 들어 있어 카메라 CEC가 논리 주소를 할당받고 가상 TV가 기본
  질의에 응답한다.

### Wi-Fi (가상 무선 환경)
mmc1의 AR6003 SDIO 모듈(`ar6003-sdio`, `docs/RE_WIFI.md`)은 기본으로 존재한다(`-M wifi=on`, 순정 하드웨어; `wifi=off`면 버스가
비어 Wi-Fi 시작 시 메뉴가 "시스템 에러"를 표시). 순정 커널 드라이버와 유저스페이스는 수정 없이 돌고, 칩 펌웨어는 무선 구간과
상대 장치까지 연기하는 HLE("virtual air")다. netdev가 없어도 카메라는 Wi-Fi를 켜고 스캔할 수 있지만 프레임은 어디로도 나가지
않는다. 무선 환경 전체에 QEMU netdev 하나를 붙인다:
```sh
./run.sh -netdev user,id=w0,net=192.168.77.0/24,hostfwd=tcp:127.0.0.1:8080-:80 -global ar6003-sdio.netdev=w0 \
  -global 'ar6003-sdio.aps=ssid=TestAP,psk=testpass123;ssid=OpenAP,ch=1'
```
- `aps=`는 카메라가 스캔·접속하는 가상 액세스 포인트 목록(`ssid=`, 선택 `psk=` = WPA2-PSK/CCMP 패스프레이즈, `ch=` 1–13,
  `bssid=`; 항목 구분은 `;`, 기본값은 채널 6의 개방형 `QEMU-AP` 하나). 스테이션 모드에서 netdev는 AP 뒤의 네트워크다:
  `-netdev user`면 카메라가 DHCP로 주소를 받고 slirp 게이트웨이(위 예에서 192.168.77.2)로 호스트에 닿는다.
- `ar6003-sdio.mac=`로 모듈 MAC 지정(기본 `02:a5:4b:00:00:01`, 로컬 관리 주소; `-nic user,model=ar6003-sdio`도 되며 이때는
  QEMU가 생성한 MAC).
- AP / 스마트폰 모드(스마트폰으로 보내기): 카메라가 자기 네트워크를 열면 HMP `qom-set /machine/ar6003 client-join
  "SSID,PASSWORD"`(카메라 화면의 SSID/비밀번호; SSID를 비우면 카메라가 연 네트워크에 접속)로, 또는 `-global
  ar6003-sdio.client=auto -global ar6003-sdio.client-psk=PASSWORD`로 자동으로 가상 클라이언트가 접속한다. 이때 netdev 상대가
  그 클라이언트 역할을 한다(MAC을 바꿔 씀). 예: `-netdev user,id=w0,net=10.0.0.0/16,host=10.0.1.200,hostfwd=tcp:127.0.0.1:8080-10.0.0.1:8080`.
- 트레이스: `-trace 'ar6003_*'` (SDIO 명령, BMI, HTC, WMI, 무선 환경, EAPOL).

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
- 셸 소켓은 계속 읽어 줘야 한다: UART 출력은 `qemu_chr_fe_write_all()`로 쓰므로, 연결만 해 두고 읽지 않는 클라이언트가 있으면
  출력하는 vCPU가 (BQL을 쥔 채) 막히고 머신 전체가 멈춘다. `a5kshell.py`를 계속 띄워 두거나(계속 읽음) 연결을 끊는다.
- 기타 머신 옵션: `console=ttynull|ttyAM1|ttyAM0`, `debug-console=on`(= `console=ttyAM1`; `run.sh`에서는 `A5K_DEBUG_CONSOLE=1`
  이면 stdio로). `shell-socket=` 없이 쓰면 UART2는 세 번째 `-serial`이다(`-serial null -serial null -serial tcp:...`).
  `console=ttyAM0`(`A5K_DEBUG_CONSOLE=ttyAM0`)은 초기 부팅 로그 전용: UART0는 av-cam 모니터 포트라서 UI가 뜨지 않는다.
- 호스트 부하: 에뮬레이터를 백그라운드 우선순위(`taskpolicy -b` 등)로 돌리거나 바쁜 호스트에서 여러 개 띄우지 않는다. 게스트
  CPU0이 굶으면 실기에서는 드러나지 않는 펌웨어 경합이 드러난다(커널 oops `PC is at 0x4394aa4c`, 태스크 `liro-kliro_113`;
  미해결, `docs/RE_ENGINE_OOPS.md`).
- 키 입력 없이 약 5분이 지나면 펌웨어의 유휴 절전이 카메라를 끈다(전원 레버 `O`, `S`/`M`/`P` 또는 HMP
  `system_wakeup`으로 다시 켤 때까지 VM suspended). 실기처럼 웜 오프라서 다음 전원 투입은 남아 있는 RAM 이미지에서
  재개한다(HOT 재개, `warm-resume=on`). 디버그 셸도 재개 후 그대로 살아 있다(Sony UART 드라이버 교착을 피하려고 디버그
  콘솔은 `loglevel=4`로 돈다, `docs/RE_UART_RESUME.md`). `-M warm-resume=off`면 콜드 부팅하고 `/android/a5k`는 사라진다.
  HMP로 가끔 무해한 키를 보내거나, 그 시간 안에 작업한다.

## 펌웨어 · 법적 고지
- 이 저장소에는 **Sony 펌웨어·바이너리·기기 덤프가 들어 있지 않습니다.** 실행하려면 **본인 소유 기기**에서 직접 추출한 NAND 덤프와 Sony가 공개 배포한 공식 업데이터에서 얻은 파일이 필요합니다(`data/README.md`). 이 파일들을 저장소나 이슈에 올리지 마세요.
- 이 프로젝트는 Sony Group Corporation과 **무관**하며 Sony가 승인·후원하지 않습니다. "Sony", "α", "ILCE", "BIONZ"는 각 소유자의 상표입니다.
- 문서는 상호운용성(에뮬레이션) 목적의 역설계 관찰 기록입니다. 배터리 인증 등 보호 기능의 분석·우회는 범위 밖입니다.
- 실기기 작업은 본인 책임입니다. 이 에뮬레이터는 실기에 아무것도 쓰지 않습니다.

## 라이선스
- QEMU 코드(`qemu-overlay/`, `patches/`)와 `tools/`·`provision/`·스크립트: **GPL-2.0-or-later** (`LICENSE`, QEMU와 동일)
- 문서(`docs/`, `*.md`): **CC BY 4.0**
- QEMU 자체는 원 저작권자의 라이선스(`qemu/COPYING`, `qemu/LICENSE`)를 따릅니다.
