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
