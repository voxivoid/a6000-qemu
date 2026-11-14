# 엔진 프레임 틱 → 서브마이컴 → UI 기동 (2026-11-14)

UI 첫 프레임까지 막혀 있던 4개의 관문과 각각의 근거. 주소는 av-cam.bin 1.10(VA = PA − 0x40000000),
커널 3.0.27 심볼(`re/kernel_syms.txt`), 모듈은 로드 주소 기준(아래 표).

## 1. 카메라 타이밍 이벤트 블록 0xF3C30000 (IRQ 83 / 84) — `hw/misc/cxd90014_evsel.c`
task 65(엔진 main, `43764494`)는 끝까지 실행된다(gdb로 모든 호출 통과 확인). 마지막에
`436AC4CC(29, 1, {cb = 4376C1E4})`로 **엔진 프레임 루틴을 이벤트 소스 29에 등록**하고 끝난다.
`436AC4CC`는 8슬롯 표에 넣고 하드웨어를 프로그래밍한다:

| off | 의미 | 근거 |
|---|---|---|
| +0x04 / +0x08 | 그룹 A 상태 / W1C | IRQ 83 핸들러 `436AC410` (`436AC7C8`/`436AC7A0`) |
| +0x0C / +0x10 | 등록 시 bit k 클리어 [?] / bit k 세트 = 채널 활성 | `436AC784` |
| +0x20+4k | 이벤트 시점 카운터 캡처 → 콜백 인자 | `436AC7D4` |
| +0x40+4k | 채널 k의 소스 ID | `436AC600` |
| +0x60+4n | 소스별 모드(항상 1) | `436AC644` |
| +0xE0 | 자유 카운터 | `436AC5F4` |
| +0xE4 / +0xE8 / +0xEC | 0 / 0x17 / run | `436AC3A4` → 24 MHz/(0x17+1) = **1 MHz** [가설] |
| +0xF4 / +0xF8 / +0xFC | 그룹 B(비교기 10개) 상태 / 활성(0x3FF) / W1C | IRQ 84 핸들러 `436AC440` |
| +0x110+8k / +0x114+8k | 비교기 k arm(1)/disarm(0) / 비교값 | `436AC81C`; 핸들러가 콜백 전에 disarm → 1회성 |

런타임 값(gdb): 엔진 설정 구조체 `0x43D87254` +0x24 = **0x8257 = 33367 µs (29.97 fps)**, +0x2C = 0x104A(1/8 프레임).
1 MHz 가설과 정확히 맞는다. 소스 29 → `4376C1E4`(프레임 시작), 28 → `4376F181`, 30 → `4376C028`, 23 → `4376A07C`.
소스 ID가 하드웨어의 어떤 신호인지는 미상 [?] → 센서가 없으므로 **활성 채널 전부를 엔진 프레임 주기마다 발생**.

## 2. 서브마이컴 slot A 허가 플래그 0x43D887AE
엔진 틱 `4376ABFC`는 플래그 바이트 `0x43D887AE`가 1일 때만 `43A3DDC0`(서브마이컴 송신)을 부른다.
- `4376C1E4`(프레임 시작): `AC(0x43D887AC) = byte_43D872D8 (=4)`, `AD = 0`, `AE = 0`, 그리고
  `436AC384(1, capture + cfg[0x2C], 1, cb = 4376CD90)` → **비교기로 1/8 프레임 뒤 "int_HS" 예약**.
- `4376CD90`(int_HS): `AD++`; `AD+1 >= AC`이면 `AE = 1`; 7회까지 자신을 다시 예약.

→ 비교기 + µs 카운터 모델로 `4376CD90` 16k회, `43A3DDC0` 2k회, **CPU slot A 교환(`4369BED0`) 1.5k회** 발생.

## 3. GIC SPI 타깃 = 부트로더 값 (RCU 정지 → umount 정지 → i1 정지)
증상: i1(`THREAD_INFRA1`)이 커널 `sys_umount → rcu_barrier → mutex`에서 D 상태, 부팅 스크립트의
`umount`(pid 130)가 `rcu_barrier → wait_for_completion`에서 영구 대기.
- 진단: `show_state`/`sched_show_task`를 gdb `call`로 실행하고 커널 `log_buf`(0x4037C760: len, buf; log_end 0x40395290)를 덤프.
- trace(`gic_acknowledge_irq`): **IRQ 29(TWD)는 쓰이지 않고**, 틱 타이머 IRQ 155/156/157이 전부 CPU0에서 처리.
- Sony `irq_select_affinity`(0x4004F0D8)는 **GICD_ITARGETSR의 현재 바이트로 친화도를 정한다**(커널이 타깃을 쓰지 않음).
  부트 스텁이 모든 SPI를 CPU0로 보내서 CPU1~3에는 틱이 없음 → RCU 콜백이 영원히 실행되지 않음.
- 수정: 부트 스텁이 **실기 GICD 덤프(`re/device/hwdump/GICD.BIN` +0x820)의 ITARGETSR**를 그대로 기록
  (틱 153/155/156/157 → CPU0/1/2/3, SYSV 98·VPH 101/102 → CPU1, 83/84 → CPU2). 0인 바이트(실기에서 미사용)만 CPU0.
- 결과: i0/i1/i2 전부 종료, `83 E wait`(관문 A 해제), `094_main()` 종료.

## 4. LDEC 하드웨어 LZ77 디코더 (메모리 모드) — `hw/misc/cxd90014_ldec.c`
증상: `Appframework`가 SIGABRT → 크래시 핸들러가 im.elf 전 스레드를 SIGSTOP(커널 `send_signal` 후킹으로 확인).
직전 stderr: `error_code = 5` = libObj `sub_5334CC`의 `ioctl(/dev/ldec, 1)` 실패(EIO). UI 이미지 리소스 해제용.
ldec.ko(initrd) 분석:
- 레지스터 0xF2903000(IRQ 171): +0x04 1=hold/0=start, +0x0C 출력 페이지 수, +0x10 출력 PA, +0x24 abort,
  +0x28/+0x2C/+0x30 = 0x7B/4/0x7B(초기화), 핸들러가 읽은 상태를 +0x30에 기록(ack [?]), **+0x34 상태 1=OK, 4=EIO**.
- 입력: BOSS DMAC(ch 3, cfg 0x11100003)이 페이지 단위로 **0xF0020000 창**(64 KiB, 랩)에 스트림을 밀어 넣음.
- 포맷 = 커널 lib/lz77(NAND ldec 경로와 동일). 하드웨어 종료 조건은 미상 [?] → "받은 바이트가 완전한 청크로
  끝나고 출력이 마지막 페이지에 도달"하면 완료(libObj가 pages = ceil(bytes/4096)로 지정).

## 5. 현재 상태
- UI 스택(Rigel WindowServer `w=640 h=480 buffer num=2`, appFw)이 **프레임을 렌더링 중**: GPU 작업(P3D/PPF/PSC)과
  완료 IRQ 76이 반복되고 PPF가 640×480 RGB565 버퍼 0x8F758000/0x8F7EE000로 전송. Android(zygote, SurfaceFlinger)도 기동.
- 그러나 버퍼는 전부 0: UI는 P3D(3D 그리기)로 그리는데 **P3D 명령은 실행하지 않고 완료 신호만** 준다.
- VPH AIF(ch51~58)는 아직 프로그래밍되지 않음 → av-cam VDF가 패널 OSD 레이어를 열지 않는 이유는 미조사.

## 모듈 로드 주소 (순정 로드 순서, 매 부팅 동일)
osal_utm 0x3F045000, **osal_uipc 0x3F049000**, utimer 0x3F056000, backup 0x3F05F000, osal_ulogio 0x3F072000,
input 0x3F07E000, udate 0x3F087000, indctr 0x3F08D000, i2c 0x3F092000, sircs 0x3F097000, dmm 0x3F0C7000,
upm 0x3F31A000, **grm_gles 0x3F346000**(sugilite_ioctl +0x8E8, ISR +0x644), hdmi 0x3F34B000, cec 0x3F372000,
mmc_drv 0x3F3DC000, liro 0x3F400000. (gdb `module_alloc_update_bounds` 반환 0x4008E034 + .text 앞부분 매칭)

## 6. 패널 출력(VPH OSD)이 열리지 않는 지점 (추가 조사)
libObj 사용자 코드 브레이크: libObj 베이스는 실행마다 다름 → 같은 gdb 세션에서 커널 구조체로 구한다
(`find_task_by_vpid`로 comm "im.e" 찾기 → task+0xCC = mm → mm+0 = 첫 VMA; VMA {+4 start, +8 end, +12 next, +72 pgoff};
크기 0xE0E000·pgoff 0인 VMA = libObj 텍스트).
- Rigel flush `sub_5DB588`은 계속 호출되지만(100 s에 32회) `sub_5DC9FA` → `sub_5D4EE4` → `sub_5D3BEC` → `sub_5D3F0C`
  (= `sub_5D86EC(&unk_EEE630)`) 가 0 → "VDF 실행 상태 아님" 경로: `vdf_if_port_send_break`(type 1) + `send_exec`(type 3)만
  osal 0x80003B로 보내고(커널에서 42회 확인) **`vdf_if_port_send_update`(OSD 레코드)는 한 번도 보내지 않는다**.
- 그 상태 플래그는 CmdQueue(`sub_5D4DF4`, 0x80003B 수신)의 `sub_5D3A94`가 **DataflowInfra 메시지 id 4096**(`sub_5D4830`: 0x101/0x102/
  0x8100/0x8101/0x8102 …)을 받아 갱신한다 → 카메라 앱의 dataflow 핀 연결(촬영 모드/라이브뷰 시작)에 의존 [?].
  센서/ISP 라이브뷰가 모델링되지 않은 것과 연결될 가능성이 높다(미확정).
- 즉 화면에 픽셀이 나오려면 (a) P3D 실행 (b) VDF dataflow 연결 → OSD 레코드 → VPH AIF 프로그래밍, 둘 다 필요.

## 7. 앱이 "정상 동작" 상태로 가지 않는 정황 (추가 조사, 미해결)
- 0x80003B(libObj dmpNative CmdQueue)로 보내는 쪽은 im.elf 자신뿐(초기화 type 2/4/6/8/9 + 매 flush break/exec).
  OSD 핀 connect(DataflowInfra id 4096)는 어디서도 오지 않는다. 0x61003C = av-cam **AVBBC** 명령 큐(libObj `ObjAvbbcCmn`).
- SD: 카드 감지(0xF2915010 = 0x0C010000)·SDHCI 모델은 있음. 그러나 `mcmnd_core_checkinsertedmedia`(mcmn_drv 0x3F3958C4)가
  한 번도 불리지 않음 → 사용자 공간이 미디어 감지를 시작하지 않는다. 서브마이컴 task 48은 부팅 초기에 front-media
  notify(0x28017C)를 1회 보냄: payload `02 00 00 07 00 00 00`(byte3 = 7). mcmn 콜백은 등록된 사용자 큐(dword_AD6C)가
  없으면 버린다 [?, 등록 이전에 도착했을 가능성].
- UPM → im.elf 전원 상태 통지 큐(0x920169, im.elf가 0x920021/0x92016E로 등록)로 가는 메시지가 관측되지 않음.
  UPM 서비스(0x9241C2/0x9243D8 배터리 레벨, 0x924126 전원 상태, 0x924223 발열)만 task 48에서 갱신된다.
- 가설: 앱의 기동 상태 전이는 부트 사유/전원 상태(hand-off 페이지의 스냅샷 값: boot factor 7, loader mode 12)와
  서브마이컴 상태에 달려 있다. 다음 단계 = appFw(Appframework) 상태 머신이 어떤 이벤트를 기다리는지 특정.
- 실험: hand-off 페이지 boot factor(0x8002104C)를 7 → 1(upm `k_upm_setup_boot_factor`의 전원키 경로)로 바꿔도
  (`A5K_HANDOFF=<file>`, run.sh) VPH/SD/GPU 동작에 변화 없음.
- appFw 상세 로그 스위치 `byte_F13D88`(libObj, `testmode_debugPrint` → osal 0x8F0139 텍스트 메시지 +20)를 커널
  `access_process_vm`(0x400C5AB4)을 gdb `call`로 불러 im.elf 메모리에 1로 기록(26 s, 50 s 시점 모두 성공) → 이후
  **앱 로그 0건**: Appframework 메인 루프는 poll()에서 유휴, 다음 이벤트(전원 ON 계열로 추정)를 기다린다.
  (다른 프로세스 메모리 쓰기 절차: `__kmalloc(16,0x20)`로 커널 버퍼 → `access_process_vm(task, va, kbuf, n, write)`.)

## 8. 정정 + 패널 OSD 개통 (hwtimer)
- gdb 브레이크포인트 `commands … c`는 여러 vCPU가 동시에 걸리면 "target is running"으로 세션이 끊겨 **이벤트를 놓친다**.
  §6·§7의 "보내지 않는다/등록 안 됨" 일부는 이 때문에 틀렸다. 이후 추적은 TCG 플러그인 `tools/a5ktrace.c`로 한다
  (`-plugin tools/liba5ktrace.dylib,words=N,pc=0x…,r0=0x… -d plugin -D log`, 빌드: README).
- 플러그인 재측정: 앱(0x920168)·im.elf(0x920169) 모두 UPM에 등록 → UPM active(0x94016A로 0x92015C) / ObjRenderer가 OSD 핀
  연결(0x290119) / VDF 응답 191회 / **Appframework가 `vdf_if_port_send_update` 2회**(패널, 640×480, 16 bpp, PA 0x8F7EE000).
- 그런데 VPH 태스크(task 43, `43904274`)는 `43AC2FD0`을 2번만 실행하고 `dly_tsk(11/12)`(`sub_4355D498`)에서 영구 정지:
  liro `dly_tsk` → 커널 `hwtimer_schedule_timeout` → 만료 콜백 `hwtimer_process_timeout`이 한 번도 불리지 않음.
- 원인: hwtimer0..3(타이머 ch5..8, CTRL **0x1132**)은 커널의 CPU별 시계 겸 비교 타이머(`hwtimer_intr`가 +0xC를 읽어 만료 비교).
  모델이 RELOAD(bit8) 일치 시 카운터를 0으로 리셋해 시계가 만료 시각에 도달하지 못했다. **bit5 = 일치 후에도 계속 계수**로 수정 [가설].
- 결과: VPH 태스크가 매 프레임 실행, **패널 OSD 레이어 설정**(0x144 = 0x03000232, ch53 = 0x8F7EE000 / 0x500 B/line / 480 lines).
  남은 검정 화면의 원인은 버퍼 내용(P3D 미실행) 하나.

## 9. SD 카드 감지 (서브마이컴 슬롯 스위치)
- mcmn_drv 계측(a5ktrace): front-media notify(0x28017C)는 사용자 큐 등록보다 먼저 오지만 `mcmnd_rm_setosalidformedianotify`가
  등록 시 캐시 상태를 재전송하므로 유실은 아님. 내용 `02 00 00 07 00 00 00` = 슬롯 3개 모두 "카드 없음".
- 출처: task 48의 slot-A 필드 추출 `439161D4`/`439162A8`/`4391637C`(슬롯 0/1/2) → 상태 구조체 `byte_43E5581E`+25..27
  → `43916660`이 상태 테이블(+18 유효 마스크, +19..21 슬롯) → `43916150`이 0x28017C로 송신.
  슬롯 0 = slot A **바이트 2 bit4 = 카드 있음**, bit0 = 두 번째 플래그 [? 잠금] (설정 모드 ≤1 경로; 모드 2는 바이트 5 사용).
- 스냅샷 프레임은 0 → `-drive if=sd`가 있으면 보드가 그 비트를 세우고 CRC 재계산(`a5k_submicro_card_inserted`).
  결과: libInfraMediaDetect → `checkinsertedmedia` → SDHCI로 카드 초기화(CMD0/8/41/2/3/9/7/16/51/6/30/42) → FAT 읽기(CMD17/18).
  아직 기록(CMD24/25) 없음: 첫 부팅 "지역/날짜/시간 설정" 대화상자에서 키 입력을 기다리는 중.

## 10. 첫 화면 이후 (진행 중)
- GPU P3D 실행(서브에이전트 2개: `RE_GPU_P3D_CMD.md`, `RE_GPU_P3D_SHADER.md`) + 패널 OSD RGBA4444 → 순정 첫 부팅 화면
  "지역/날짜/시간을 설정합니다. [확인][취소]"가 QEMU 콘솔 0에 실기와 같은 모양으로 표시.
- 버튼 프런트엔드(`a5k_key_event`): Enter → GPIO P10.10 low. "확인"을 누르면 UI가 반응하여 화면이 바뀐다.
  그러나 다음 프레임은 PSC 지우기 + PPF 전송만 있고 P3D 그리기가 없다(빈 화면) → 다음 화면(지역 설정)이 그려지기 전
  앱이 다른 무언가(모드 전환 완료 등)를 기다리는 것으로 보임 [?]. 같은 시점부터 0xF3C10xxx(CEC 블록) 폴링이 급증.

## 11. "확인" 뒤 검정 화면 = 버튼 배선 오류 (MOVIE 버튼이 눌렸음) → 첫 부팅 설정 완주
### 11.1 원인
§10의 버튼 프런트엔드는 Sony 키 코드의 의미를 잘못 붙였다. 호스트 Enter가 누른 GPIO P10.10은 가운데 버튼이 아니라
**MOVIE(녹화 시작/정지) 버튼**이다. 그래서 첫 부팅 대화상자가 "취소" 경로로 닫히고, 앱은 바로 촬영 모드로 들어갔다.
- 키 이름의 근거는 libObj의 `{코드, "KEY_..."}` 표다(파일 오프셋 0xEE1078부터, 8바이트 항목).
  0x0002 POWER, 0x0003 UP, 0x0015 LEFT, 0x0030 DOWN, **0x01AC ENTER**, 0x01AF RIGHT, **0x01AB MENU**,
  **0x01F0 STARTSTOP(MOVIE)**, 0x01F7 PLAY, 0x01F8 DELETE, 0x1BF SLIKEY_ZOOM, 0x70A6 PANEL_ARM_DET,
  0x7094 FLASH_POPUP, 0x70D5 UM_CONNECTOR_POWER.
- event0 코드는 libInputServant 표(0x3364, `{sony, linux}`)를 따른다. 0x03→103, 0x30→108, 0x1AF→106, 0x15→105,
  0x1AC→232, 0x1AB→514. 두 번째 표(0x3394)에는 0x1F0→229, 0x1F1→102, 0x1AB→158가 있다.
- 따라서 input.ko 키 표(`RE_INPUT_I2C_POWER.md` §7)를 실제 버튼에 대응시키면 다음과 같다.
  ADC ch 0x0D: 0..91 **ENTER** / 92..273 UP / 274..439 LEFT / 440..599 DOWN / 600..772 RIGHT.
  ADC ch 0x0C: 0..91 DELETE. GPIO P10.10: MOVIE. GPIO P5.2: 패널 180° 회전 검출(active low).
  front CAkey1: bit0 POWER(active low), bit1 PLAY, bit2 MENU, bit5 UM_CONNECTOR_POWER, bit9 FLASH_POPUP(active low).
  예전 매핑은 LEFT=가운데, M=UP, RIGHT=LEFT, UP=DOWN, DOWN=RIGHT, Enter=MOVIE였다.
- 앱 쪽 증거(a5ktrace 함수 추적, §11.4)는 다음과 같다. 키를 누른 직후 M_CLOCK 전이 함수 `sub_40696C`가 상태 4에서 내부 이벤트 3을 받아
  action 8 = `ActionSetForceSetting`(`sub_407254`)을 실행했다. 이 action은 기본 날짜를 설정하고
  `setClockForceSettledToBackup`(Backup 0x1070140 = 1)을 기록한다. 그 뒤 `[M_ROOT] getDateInfo() Clock Force Settled`가 찍히고
  `viewStlrec.uxc`가 열린다(촬영 화면). 지역 설정 화면(`viewAreaSet.uxc`)은 한 번도 열리지 않았다.

### 11.2 수정 (`hw/arm/sony_a5000.c` `a5k_key_event`)
| 호스트 키 | 하드웨어 | Sony 코드 |
|---|---|---|
| Enter / KP_Enter | ADC 유닛 1 ch 1(=0x0D) 레벨 45 | 0x1AC ENTER |
| ↑ / ← / ↓ / → | 같은 채널 182 / 356 / 520 / 686 | 0x03 / 0x15 / 0x30 / 0x1AF |
| Delete / Backspace | ADC 유닛 1 ch 0(=0x0C) 45 | 0x1F8 DELETE |
| R | GPIO P10.10 low | 0x1F0 MOVIE |
| M / P | 서브마이컴 slot A **바이트 28 bit2 / bit1** (CRC 재계산) | 0x1AB MENU / 0x1F7 PLAY |

- 래더 채널마다 누르고 있는 키를 기억한다. 다른 키를 뗄 때는 레벨을 1023으로 돌리지 않는다.
- MENU와 PLAY의 경로: av-cam 필드 파서 `43AC73AE`가 slot A의 b28 | b29<<8을 읽어 task-48 상태 0x43ED43B0+24(CAkey1)에 반영한다.
  0x280132 sync 핸들러 `43915890`(`43AC7408`)가 이 값을 msg[8..23]에 복사하고, input.ko가 이를 폴링한다.
  스냅샷의 CAkey1 값은 0x0201이다. M을 누르고 있는 동안 0x83ED43C8이 0x0205로 바뀌고, 0x7A0020에 `{0x1AB, 0, 1}`과 `{0x1AB, 0, 0}`이 전달되는 것을 확인했다.
  첫 대화상자에서 MENU를 누르면 "취소"와 같이 처리되어 PlayMemories 안내로 넘어간다. 실기 동작과 같다.

### 11.3 첫 부팅 설정 진행 결과 (무수정 펌웨어, sendkey)
1. 대화상자 "지역/날짜/시간을 설정합니다." → Enter → **지역 설정**(세계 지도, "동경/서울", GMT +9.0, 2014-01-01 00:00 AM)
2. Enter → **날짜/시간 설정** 목록(서머타임 끔 / 날짜/시간 설정되지 않음 / 날짜 형식 년-월-일 / [확인])
3. ↓, Enter → 날짜/시간 편집기. ↑/→로 2026-11-14로 바꾸고 Enter, ↓↓로 [확인]을 고른 뒤 Enter
   (목록으로 돌아왔을 때 "날짜/시간" 오른쪽 값이 비어 있다. RTC(서브마이컴 시계)는 아직 모델에 없다 [?])
4. **PlayMemories Home 안내**("www.sony.net/pm", [확인]) → Enter → 촬영 모드.
   - 주의: 대화상자는 InputService `toActive`보다 먼저 그려진다. 표시 직후 약 20 s 안에 누른 키는 버려진다.
     gdb가 VM을 멈춘 상태에서 보낸 `sendkey`도 잃는다.
5. 촬영 모드 결과는 SD 카드 유무에 따라 다르다.
   - **SD 없음**: "렌즈가 장착되었는지 확인하십시오 … '렌즈 없이 촬영'을 '가능'으로…" 경고가 뜬다. 렌즈를 모델링하지 않았으므로 실기와 같은 동작이다.
     Enter를 누르면 **촬영 화면 OSD**(NO CARD, 16:9, 17M, RAW+J, 1080, 배터리 98%, SCN, ±0.0, ISO AUTO, 격자선)가 표시된다.
     라이브뷰는 검정이다(센서/ISP 미구현). 이 상태에서 MENU 이벤트(0x7A0020 0x1AB)는 전달되지만 메뉴는 열리지 않는다.
     휠 키도 OSD를 바꾸지 않는다 → 카메라 기동(모니터링 시작)이 끝나지 않아 조작이 막힌 것으로 추정 [?].
   - **SD 있음**(macOS에서 포맷한 FAT32, 카메라 DB 없음): StlRec OSD가 한 프레임 그려진다. 미디어 아이콘은 "ERROR"이고,
     패널 버퍼 0x8F7EE000을 덤프해 확인했다. 그다음 루트 뷰가 이벤트 0x13067054를 받아 **view 12**를 StlRec 위에 연다(blog: `type=4 ev=0x1c`).
     StlRec 레이아웃의 위젯 약 250개가 소멸(`Widget::~Widget`, lr 0x7027BF)한 뒤로는 화면이 빈다(PSC fill + PPF만 있음).
     view 12 = **AUTOREPAIR**(viewUnified8 `ViewAutoRepairToInstance`, 이미지 DB 자동 복구)로 추정한다 [?].
     근거는 viewUnified2의 통합 IdSoTable 등록 순서다. 이 순서로 STLREC=131, INITIAL_SETTING=61이 관측값과 일치한다.
     view는 `setModelRequest(true)`로 M_REPAIR 응답(예상 시간 등)을 기다리는 것으로 보인다. 이후 SD 명령은 하나도 나가지 않는다
     (부팅 때의 CMD17/18/13/30만 있고 CMD24/25는 없다). CPU는 유휴 상태다 → 복구 모델이 기다리는 응답이 무엇인지는 미확정.
     키는 루트 뷰(103)만 받고 10 s 타이머만 건다.

### 11.4 도구 (조사 방법, 다음 작업자용)
- `tools/a5ktrace.c`에 옵션을 추가했다.
  - `str=N` / 프로브별 `s=N`: r1+N의 문자열을 출력한다(osal 텍스트 메시지 +20, `do_sys_open` 파일명 +0).
  - `x=V`: r0 제외 필터(반복 가능). `comm=NAME`: 태스크 이름 접두어 필터.
  - **`fn=PATH`, `fncap=N`**: 실행 중에 다시 읽는 사용자 공간 함수 프로브 목록이다(한 줄에 `hex주소 [라벨]`).
    파일 mtime이 바뀌면 플러그인이 `qemu_plugin_reset`으로 재계측한다. libObj의 7만 개 함수 전부를 걸어도 동작한다.
- libObj 베이스: gdb에서 `find_task_by_vpid`로 comm "im.elf"를 찾는다(pid 100..200). 그 VMA 중 크기가 0xE0E000이고 pgoff가 0인 것이 libObj다.
  appFw 상세 로그 스위치 `byte_F13D88`은 `access_process_vm`으로 1을 쓴다. 이 둘을 한 스크립트로 처리할 수 있다.
  상세 로그는 osal 0x8F0139 +20에 실린다. 레벨 1(오류)은 스위치와 관계없이 항상 출력된다.
- appFw **blog**(osal 0x8F013A, 메시지 +0 = 0x00830002): +4 카테고리, +8 길이, +24 데이터.
  카테고리 4 = 뷰·모델 이벤트 시퀀스(`blogEventSequence`: `{arg2, vm(1=view, 2=model), type, …, eventId BE, dest, queue}`;
  type 1=생성, 2=종료, 3=활성/비활성, 4=push), 7 = TopologyManager, 14 = AppSystem 메인 루프(sleep 타임아웃, updateWidget, draw).
  `blogEventSequence`로 화면 전환 순서를 그대로 따라갈 수 있다.
- 한 번(run10: `-trace sdcard_*` + gdb `call`) av-cam task 66에서 NULL 참조 oops가 났다(`4376FA2x`, `ldr r0,[r0,#24]`).
  이어서 "scheduling while atomic"으로 시스템이 멈췄다. 같은 절차를 gdb 없이 반복했을 때는 재현되지 않았다 → 디버거 간섭으로 추정 [?].

### 11.5 남은 일
- SD 카드가 있을 때의 AUTOREPAIR 정지: M_REPAIR → ObjSalvage/MprIf 비동기 명령의 응답 경로를 추적해야 한다(`[M_REPAIR] ActionGetRepairTime*`).
- 촬영 모드에서 메뉴와 휠이 반응하지 않는 문제: 카메라 기동(CAMUSER_* / av-cam 센서 경로) 완료 이벤트가 필요한지 확인해야 한다.
- RTC(날짜 표시 공란), 줌 레버(ADC 0x0F 다단), 셔터(S1/S2 경로 미확인) 입력.
