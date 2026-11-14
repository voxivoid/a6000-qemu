# RE: DMP SMAPH-S30 셰이더 유닛 (명령어 인코딩 · 레지스터 파일 · 업로드)

대상: a5000 GPU(DMP SMAPH-S30, 드라이버명 SUGILITE)의 프로그래머블 셰이더. 구현은
`qemu/hw/display/dmp_shader.c`, 인터페이스는 `qemu/include/hw/display/dmp_shader.h`,
호스트 테스트는 `tools/test_dmp_shader.c`. 커맨드 리스트 형식과 P3D 레지스터 전반은
`docs/RE_GPU_P3D_CMD.md`를 본다.

표기: **[V]** = 실제 펌웨어 프로그램을 인터프리터로 돌려 GLSL 소스가 뜻하는 결과와 일치함을 확인.
**[S]** = 드라이버 코드(정적 분석)로 확인. **[?]** = 가설(근거와 함께 적는다).

---------------------------------------------------------------------------------------------------
## 0. 요약
- 장치에는 GLSL 컴파일러가 없다. `libGLESv2_dmp.so`의 `dmp_glslc_*`는 `libDMPGlslc.so`를 dlopen하는
  스텁뿐이고, 이 라이브러리는 펌웨어 어디에도 없다 [S]. 그래서 순정 UI는 **미리 컴파일된 바이너리만** 쓴다.
  - GLES2: `glProgramBinaryOES(format 0x6000)`. libObj.so 안에 19개가 있다(2D 렌더러 6개, gemini 13개).
  - GLES1: 고정 기능을 흉내 내는 셰이더 패키지 `package_bin_u`(libGLESv1_CM_dmp.so 0x3b810, 8496바이트).
    Rigel 합성(GLES1 컨텍스트)이 이 정점 프로그램을 0번지에 올려 쓴다.
- **하나의 통합 코어**가 정점·프래그먼트 프로그램을 모두 실행한다. 명령 메모리, 오퍼랜드 디스크립터 표,
  float 상수 파일이 하나씩이다 [S].
- 명령은 64비트(32비트 워드 2개)다. PICA200(32비트 명령)과는 **다른 ISA**다. 공통점은 7비트
  디스크립터 인덱스, 마스크/스위즐 디스크립터, 비교 플래그 + 조건 분기, 주소 레지스터 정도다.
- 소스가 있는 프로그램은 모두 기대대로 계산된다 [V]. 기대값은 GLSL 소스, 또는 소스가 없을 때 유니폼 이름으로 정했다.
  여기에는 gemini 정점/프래그먼트 7종, 2D 렌더러 4종, GLES1 패키지의 합성 경로가 들어간다.
  검증 범위 밖의 opcode와 비트는 [?]로 남겼다(§7, §9).

## 1. 업로드 경로 (P3D 레지스터)
근거: `libGLESv2_dmp.so` `pg_link_specify_program`(0x20430), `pg_deploy_program_binary`(0x2144c),
`store_data`(0x203e4), `cnv_update_prg_register`(0xf2e0), `__exe_store_const_reg`(0xc120), 그리고 GLES1
`__gl_shader_binary`(0x1e094). 레지스터 번호는 커맨드 헤더 `reg<<16 | 0xF000` 기준이다.

| reg | 의미 | 근거 |
|---|---|---|
| 0x458 | 코드 적재 주소(명령 단위) | `store_data(cmb,0,0x458, code_ofs+56)` [S] |
| 0x459 | 코드 데이터. 쓸 때마다 워드 +1. 명령 하나는 `w0`, `w1` 순으로 두 번 쓴다 | 같은 함수 루프 [S] |
| 0x460 | 오퍼랜드 디스크립터 주소(0..127) | lo 뱅크 = `swiz_ofs`, hi 뱅크 = `swiz_ofs + 64` [S] |
| 0x461 | 디스크립터 데이터(주소 자동 증가) | [S] |
| 0x450 | float 상수 인덱스 | [S] |
| 0x451–0x454 | x, y, z, w. **IEEE f32 그대로**(PICA의 f24 패킹 없음) | `__exe_store_const_reg`: 헤더 `0x0450F026` + `addr` + 16바이트 `reg->data` [S] |
| 0x440 | `0x000F0004` 또는 `0x000F0F04` [?] | `cnv_update_prg_register`, glClear 리스트 [S] |
| 0x441 | 0 또는 0xFFFFFF00 [?] | 같음 |
| 0x442 | **정점 프로그램 진입점**(명령 인덱스) | glClear 리스트 `0x442=0x38` + 실측 [S] |
| 0x443 | **프래그먼트 진입점**. 기본 깊이 범위이고 polygon offset이 없으면 `+9` 해서 깊이 프롤로그를 건너뛴다 | `cnv_update_prg_register`(`v19 = 9`) [S] |
| 0x445, 0x470–0x477 | 보통 0. 정수/불 유니폼 후보 [?]. 테스트한 프로그램은 쓰지 않는다 | |

- **명령 0..55는 예약 영역**이다. GLES2 프로그램은 `code_ofs + 56`부터 적재된다 [S].
  GLES1 패키지는 0번지부터 947개 명령을 차지한다. 이때 GLES1 프래그먼트 프로그램은 0x1C8(456)에 들어간다(실측).
- GLES2 프래그먼트 프로그램 앞에는 드라이버가 **9명령 깊이 프롤로그**를 붙인다.
  상수 `C.64.9960` / `C.127.10502`(18워드, 디스크립터 0x79–0x7F)이고, `gl_DepthRange` 유니폼이
  `c3`에 들어간다(`fischer_state.depth_range.addr = 3`) [S]. 이 프롤로그는 기본 상태에서는 실행되지 않는다(+9).
- 상수 파일 범위: 실측 인덱스는 0..0xC9이다. GLES1 패키지 상수는 `c200 = (0,1,2,3)`, `c201 = (4,5,6,7)`이고,
  인덱스 표로 쓰인다 [S]. 헤더는 `DMP_SH_NUM_FUNIF = 256`이다.
- 불(bool) 유니폼은 드라이버가 **0.0/1.0 float로 바꿔 상수 파일에 넣는다**(`pg_uniformfv`: 불 타입이면
  `v != 0 → 1.0`) [S]. GLES1 패키지의 기능 분기도 float 상수를 CMP해서 결정한다(§8).

## 2. 프로그램 바이너리 (`glProgramBinaryOES`, format 0x6000)
`pg_deploy_program_binary`의 파서 기준이다 [S]. 모든 정수는 LE이고 **정렬되지 않은** 스트림이다.

```
+0  u16 0xBEAF (바이트 스왑형 0xAFBE도 허용)  +2 u32 format = 0x6000  +6 u8 type  +7 u16 (무시)
type 1 (2D API 모드, pg->is_2d_api_mode = transpose_matrix = 1):
  +9 u32 off[6]  (각 섹션은 u32 길이로 시작)
  sec0: len, vs_entry(+56), vs_in, vs_out, vs_ctx, fs_entry(+56), fs_ctx, gs_entry, gs_out, gs_ctx,
        u32, u32, n, 2n 코드 워드 (0x458 = 56에 적재)
  sec1: len, n, n×(addr, desc)  → 0x460/0x461
  sec2: len, n, n×(addr, x, y, z, w) → 0x450..0x454
  sec3: 유니폼 표 / sec4: 속성 표 / sec5: 링크 상태
type 0 (일반 GLES2):
  +9 u32 off[4];  sec0: len, u8 flags(bit0 one-hot, bit1 prefix-fs-out, bit2 frag_coord_z),
        셰이더 2개(VS, FS) × { u8 in, u8 out, u8 ctx, i32 code_ofs, u32 n, 2n 워드,
        i32 lo_ofs, u32 n, n 디스크립터, i32 hi_ofs(+64), u32 n, n 디스크립터,
        i32 const_ofs, u32 n, 4n 워드 }
  sec1: 유니폼 표, sec2: 속성 표, sec3: 링크 상태
유니폼 항목: i32 namelen, name\0, u32 GL 타입, i32 array_size, i32 addr(상수 레지스터), i32 comp(시작 성분)
```
- 유니폼은 **성분 단위로 패킹**된다. 예: `u_epMatrix` vec2[3]는 c0..c2.xy, `u_texMatrix` vec2[2]는
  c0.zw/c1.zw, `u_minY` float는 c2.w, `yuv2rgb` float[12]는 c8..c19의 .w에 들어간다.
- GLES1 패키지(`"GPSD"`, 버전 0x10000): 워드 2 = 패키지 id, 3 = 코드 워드 수(1894), 4 = 디스크립터 워드 수(214),
  5 = 상수 수이고, 그 뒤에 코드, (addr, desc) 쌍, 상수가 온다. 0x458 = 0에 적재된다 [S].

## 3. 명령어 인코딩
`w0` = 0x459에 먼저 쓰는 워드, `w1` = 두 번째 워드.

| 필드 | 비트 | 의미 | 상태 / 근거 |
|---|---|---|---|
| desc | w0[6:0] | 오퍼랜드 디스크립터 인덱스(0..127) | [V] 깊이 프롤로그가 0x79..0x7F를 순서대로 쓴다. 대부분의 명령이 서로 다른 desc를 쓴다 |
| src2 idx | w0[20:12] | src2 레지스터 번호(9비트) | [V] yuv2rgb c19 = `0x13`, GLES1 c104 / c201(`0x0c9`) |
| src2 file | w0[23:21] | 0 = X, 1 = Y, 2 = V(입력), 4 = U(상수) | [V] (§5) |
| src1 sel | w0[27:24] | bit1 = V(입력), bit0 = Y, bit3 = +a0(상대 주소). 0이면 X | [V] bit3은 [?](GLES1 행렬 팔레트 경로에서만 쓰이며 미검증) |
| src1 idx | w0[31:28] + w1[2:0] | 하위 4비트 + 상위 3비트(×16) | [V] yuv2rgb c16–c18 = w1 bit0. GLES1 `c105..c108` = w1[2:1] = 3 |
| src1 = U | w1[7] | src1을 상수 파일에서 읽는다. 결과의 Y 사본을 쓰지 않는다 | [V] 2D/GLES1 행렬 곱, FS `= filterColor` |
| dst idx | w1[11:8] | 목적 레지스터 | [V] |
| dst Y 제어 | w1[28], w1[5] | §5.3 | [?] 적합 결과 |
| dst = O | w1[19] | 출력 파일에 쓴다(op 0x0c이면 `0x0C8`) | [V] |
| (w1[18]) | | 2D/GLES1 코드에 많다. 효과는 찾지 못했다 [?] | |
| opcode | w1[27:20] | §7 | |
| w1[29] | | 텍스처 결과를 읽는 첫 명령에 붙는다(대기 표시로 추정) [?]. 무시한다 | |
| 분기 조건 | w1[19:12] / w1[11:8] | §8 | [?] |

**NOP** = `w0 = w1 = 0`. 컴파일러는 결과 지연을 NOP으로 메운다(예: RSQ 뒤에 NOP 3개, TEX 뒤에 1개). 인터프리터는 지연을 모델링하지 않는다.

## 4. 오퍼랜드 디스크립터 (0x461)

| 비트 | 의미 | 근거 |
|---|---|---|
| [29:26] | 쓰기 마스크. bit26 = x … bit29 = w | [V] e80f92 `a = tex.a*col.a`는 0x20…(w만) |
| [25:21] | src1 수정자 X | |
| [20:13] | src1 스위즐(성분당 2비트, x가 최하위. 항등 = 0xE4) | [V] yuv2rgb `.wwww` 스칼라 유니폼 |
| [12:8] | src2 수정자 Y | |
| [7:0] | src2 스위즐 | [V] |

수정자(X/Y, 5비트): bit4 = **부정** [V](`xy*2 − 1`의 `−1`).
하위 3비트 값은 다음과 같다.
- 3 = 오퍼랜드를 **ROM 상수 벡터**로 바꾼다 [V].
- 2 = 동차 좌표로 다룬다. w = 1로 하고, op 0x02(합)에서는 w를 뺀다 [V].
  2D 정점 프로그램의 `gl_Position.w = 1`과 gemini의 `dot(vec3)`이 이것으로 맞는다.
- 0/1/4 = 일반 [?]. 4는 쓰지 않는 src2에 흔히 붙고, 1은 효과가 드러난 사례가 없다.

ROM 벡터 = **(0, 0.5?, 1, 2)** [V(x, z, w) / ? (y)]. 확인된 사용처:
- `gl_Position.xy * 2.0 − 1.0`에서 2와 1
- `gl_Position.zw = (0, 1)`
- YUV FS의 alpha = 1
- 2D 정점의 평행이동 행에 곱하는 (1,1,0,1)
- CMP의 0

## 5. 레지스터 파일

| 파일 | 읽기 | 쓰기 | 초기값 | 상태 |
|---|---|---|---|---|
| V 입력 | src1 sel bit1, src2 file 2 | — | VS: 정점 속성, FS: 보간된 VS 출력 k → 입력 k | [V] |
| O 출력 | — | w1[19] | 입력 사본 | [V] VS o0 = 클립 좌표. FS **o1 = 색**, o0 = 깊이(프롤로그) |
| X 일반 | src1 sel 0, src2 file 0 | 기본 | 상수 파일 사본(유니폼과 임시값이 한 파일, 컴파일러가 겹치지 않게 배치) | [V] |
| Y 일반(두 번째 사본) | src1 sel bit0, src2 file 1 | §5.3 | 상수 파일 사본 | [V] 적합 결과 |
| U 상수 | src1 w1[7], src2 file 4 | 읽기 전용 | 0x450..0x454 | [V] |
| a0 | src1 sel bit3 | op 0x11 | 0 | [?] |
| cond[4] | 분기 | op 0x1E | 0 | [?] |

### 5.1 정점 프로그램
- 입력과 출력은 **다른 파일**이다 [V]. GLES1 패키지는 o0를 쓴 뒤에도 V0(위치)를 다시 읽는다.
- GLES1 속성 배치(실측 + 코드): V0 위치, V1 색, V2 텍스처 좌표 0, V3 텍스처 좌표 1, V4 법선,
  V6/V7 행렬 팔레트 가중치/인덱스 [?]. GLES1 출력은 o0 위치, o1 색, o2 tex0, o3 tex1이다.
- gemini(type 0)의 속성 번호는 바이너리 속성 표의 index와 같다(inColor 0, inPosition 1, inUV 2, mtxL2W_0 3,
  mtxL2W_1 4) [V].

### 5.2 프래그먼트 프로그램
- FS 입력 k ≥ 1은 V뿐 아니라 X/Y k에도 들어간다 [V]. 예: `texture2D(tex, varUV)`의 좌표 오퍼랜드는 X2,
  `= varColor`는 X1. 입력 0(위치)은 X0에 넣지 않는다. 2D FS가 X0의 `u_color`를 읽기 때문이다 [V].
- 출력은 입력 사본에서 시작한다. `rgb = varColor.rgb; a = …`는 o1.w만 쓴다 [V].
- FS 안의 출력 쓰기는 같은 번호의 X에도 보인다 [?]. GLES1 텍스처 환경 프로그램이 `o4 = tex` 다음에
  `X0 = X4`, `o1 = X0`을 하기 때문이다.
- 색은 **o1**이다. o0는 깊이 프롤로그(`0x0C8` dst 0)가 쓴다.

### 5.3 X/Y 사본 규칙 (적합 결과) [?]
- X에는 항상 쓴다.
- Y 사본은 쓰지 않는 경우: `w1[7]`가 있을 때, 또는 `w1[28]`이 있고 `w1[5]`가 없을 때.
- 이 규칙을 찾은 방법: X, Y, U 세 파일과 쓰기/읽기 코드 조합을 브루트포스로 돌렸다.
  - 대상: libObj 프로그램 19개(테스트 25항목). 모두를 만족하는 유일한 계열을 골랐다(`scratchpad`의 `solve*.py`).
  - 결정적 사례 1: 2D 정점 `bcd6a4` 59–62. `Y1 = tm1`, `X1 = tc*tm0`, `o1 = Y1 + X1`로 텍스처 행렬 scale + offset이 된다.
  - 결정적 사례 2: 2D `bcda74` 56–58.
- 하드웨어 실체는 아마 파이프라인 포워딩/2-포트 파일일 것이다. 이 비트들의 진짜 의미는 아직 모른다.

## 6. 확인된 데이터 흐름 예 (gemini VS, libObj 0xe7ed54, 34명령)
GLSL(libObj 0xbcf090): `gl_Position.xy = (dot(inPosition, mtxL2W_k) * invScreenSize) * 2 − 1; y *= −1; w = 1`.
- 57–59: 입력 → X3, X1, X0으로 복사한다.
- 61–62: `MUL X4 = V3*X3`, `X3 = V4*X3`.
- 63–64: `op02` 수평합(X=2 → xyz) → X3.x, X3.y.
- 74: `MUL X2 = X4 * U7(invScreenSize)`.
- 75–76: `MUL ×ROM.w(2)`, `ADD −ROM.z(1)`.
- 79: `MUL ×U6.x(−1)`. 상수 섹션의 `c6 = (−1,0,0,0)`이다.
- 78/80: `.zw = ROM.x/ROM.z`.
- 82: `o0 = Y2`.
테스트 값으로 돌리면 `(−0.625, 0.75, 0, 1)`이 나온다. 손계산과 같다 [V].

## 7. opcode (w1[27:20])

| op | 이름 | 의미 | 상태 / 근거 |
|---|---|---|---|
| 0x0C | MOV | dst = src1 | [V] |
| 0x01 | ADD | src1 + src2 | [V] `tex+varColor` |
| 0x03 | MUL | src1 × src2 | [V] `tex*varColor` |
| 0x02 | DP(수평합) | x+y+z(+w, 수정자 2면 w 제외), 브로드캐스트. 행렬 곱은 `MUL` + `0x02` | [V] gemini dot3, GLES1 MVP |
| 0x1C | MAX | | [V] 형태: 조명 `max(N·L, 0)`, 2D `max(Y, minY)` |
| 0x1D | MIN | | [V] 형태: 깊이 프롤로그 clamp, 조명 `min(x, 1)` |
| 0x05 | RSQ | 1/√src1.x | [?] 강함: normalize 패턴 `MUL`, `DP`, `0x05`, NOP×3, `MUL` |
| 0x04 | RCP | 1/src1.x | [?] |
| 0x07 / 0x08 | EX2 / LG2 | | [?] pow 패턴(LG2 → MUL → EX2) 추정 |
| 0x11 | MOVA | a0 = (int) src1 | [?] 뒤의 `?9x` 상대 주소 읽기와 짝을 이룬다 |
| 0x1E | CMP | cond[c] = src1[c] OP src2[c]. OP = w1[18:16] | [?] 3 = GT, 4 = EQ는 GLES1 플래그 검사로 맞췄다. 0 NE, 1 LT, 2 LE, 5 GE는 추정 |
| 0x20 | TEX | dst = sample(unit = w1[16:14], coord = src1) | [V] 형태. unit 필드는 [?] |
| 0x2B | TEX→색 + END | o1 = src1(마스크), 종료 | [?] 2D 텍스처 FS 마지막 명령. 뒤에 END가 없다 |
| 0x24, 0x3D | 조건 분기 | §8 | [?] |
| 0x0B 0x0D 0x0E 0x0F 0x12 0x13 0x18 0x19 0x1A 0x1F 0x26 0x27 0x29 0x2A | 미구현 | GLES1 조명/텍스처 생성/GS 경로에서만 쓰인다 | `LOG_UNIMP` 후 false |

## 8. 제어 흐름 (opcode 0, w1[15:12])

| w1[15:12] | 동작 | 근거 |
|---|---|---|
| 0 (w0=w1=0) | NOP | [V] |
| 1 | CALL w0 | GLES1 0x1B0 서브루틴 호출 반복 [?] |
| 2 | RET | [?] |
| 3 | JMP w0 | 루프 되돌아가기(154 → 0x3A) [?] |
| 4 | END | [V] |
| 0xD | 블록 표지(조건 블록 끝?) → NOP | [?] |
| 5, 6, 0x11 | GS(EMIT 계열?) | 미구현 |

조건 분기 `op 0x24 / 0x3D`, 목표 = w0 [?]:
- w1[19:12]은 성분당 2비트로, x가 최상위 쌍이다. 쌍의 bit1 = 무시, bit0 = 요구 값이며 AND로 묶는다.
  - 0x7F = x만, 0x55 = 전부, 0x6A/0x9A/0xA6/0xA9 = x/y/z/w 하나.
  - 근거: GLES1 조명 타입 스위치(291–299)가 0x6A, 0x9A, 0xA6, 0xA9를 연달아 쓴다.
- w1[10]은 반전으로 가정했다. `0x3D` 계열의 `6a4`가 그렇다.
- 이 가정으로 GLES1 패키지 합성 경로가 맞게 나온다(§9). 다른 경로(조명, 안개, 팔레트)는 검증하지 않았다.

## 9. 검증 (`tools/test_dmp_shader.c`)
빌드/실행 명령은 파일 머리에 있다. 펌웨어 파일은 데이터로만 읽는다. 결과: **25/25 PASS**.
- 단순 GLES2(0xe813c4, 0xe814c4)
  - VS 통과: `gl_Position = a_position`, `v_texCoord`
  - FS: `= filterColor`, `texture2D(s_texture)*filterColor`
- gemini(0xe7ed54 …)
  - VS 위치/색/UV
  - FS 5종: `varColor`, `tex`, `tex*col`, `tex+col`, `rgb=col, a=tex.a*col.a`
  - YUV→RGB FS. 행렬 열이 텍셀 (t.y, t.z, t.x)에 곱해진다. BT.601 계수를 넣으면 텍셀 순서가 (Cr, Y, Cb)인 셈이다.
- 2D 렌더러(0xbcda74, 0xbcd560, 0xbcd6a4)
  - 위치 = `ep0*x + ep1*y + ep2`, w = 1
  - 텍스처 좌표 = `tc*u_texMatrix[0] + u_texMatrix[1]`
  - FS `u_color`, 텍스처
- GLES1 패키지(합성 경로, 실측 유니폼)
  - 위치 = MVP·pos, 색 = V1·c25.w, tex0 통과
  - 드라이버가 생성한 0x1C8 FS(REPLACE) = 텍셀
- 기대값이 "소스 의미"가 아니라 "추정"인 항목: 2D 텍스처 행렬의 의미, GLES1 속성 배치, YUV 텍셀 성분 순서.

## 10. 남은 일 / 미확정
- X/Y 사본 규칙(§5.3), w1[18], w1[28], w1[29]의 실제 의미.
- CMP 모드 0/1/2/5, 분기 조건 하위 니블, CALL/RET/블록 표지.
- 미구현 opcode(§7) — GLES1 조명(`0x0F`, `0x0E`, `0x1A` …), 텍스처 생성, GS(점/선 프로그램 `dmp_point_shader_prog`).
  - 이 경로가 실행되면 `LOG_UNIMP`(`-d unimp`)에 `dmp-shader: opcode at N`이 찍힌다.
- ROM.y 값, TEX unit 필드, 0x2B의 정확한 의미.
- 깊이 프롤로그(op 0x11.8 사용)는 기본 상태에서 건너뛰므로 구현하지 않았다.
