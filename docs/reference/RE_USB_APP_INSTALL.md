# USB app install (`pmca-console install -f <apk>`) over the virtual cable

Scope: does the real-camera deployment path for code (Sony-PMCA-RE `install -f <app.apk>`, the way an
OpenMemories-style APK is put on a camera) work end to end in the emulator, and if not, where does it break.
Prerequisites: `docs/RE_USB_HOST_PATH.md` (A5KU cable, `socket` backend, device discovery §3), and
`docs/RE_USB_DEVICE_CONTROLLER.md` §9 (controller model).

Result in one line: **it works end to end with no model changes.** The camera switches to the app-install gadget,
tunnels TLS to PMCA's local market server over USB, downloads the SPK, decrypts and installs the APK, the app shows up
in MENU → Application → Application List, and it launches. No code agent work is needed for this path; §6 lists only
optional follow-ups and the acceptance test to keep as a regression check.

Setup used (2026-11-15): firmware 1.10, emulator binary from QEMU commit `910eae2`, a fresh clone of the project's
pristine NAND per run, no SD card, `-M usb-socket=127.0.0.1:<port>`, `A5K_SHELL=<port>` only to read logcat
(observation aid; the install itself went over USB only). Host: macOS, Sony-PMCA-RE `a82f5ba` + the `socket`
backend (`tools/pmca-socket/`). Test APK: the project's own Track A debug build (package `dev.oein.a5kui`, label
"a5000 UI", 382 154 bytes, minSdk 10, debug-signed). Nothing was downloaded from the internet.

---------------------------------------------------------------------------------------------------------------------

## 1. Runs

| Run | What | Result |
|---|---|---|
| r1 | boot → `install -f` → screendumps → keys | install **completed**; APK in `/data/app`, dex in dalvik-cache, `packages.xml` entry. Later an av-cam oops (§5) — not reproduced |
| r3 | boot → `install -f` (screendumps every 5 s) → MENU → Application → Application List → launch | install **completed**; "a5000 UI" in the Application List; app **started** (its own OSD on the panel) |
| r4 | control, no cable: PLAY (no card) → shutter half press | no oops |
| r5 | boot → `install -f` → shutter half press (r1's sequence) → wait past 400 s | install **completed**; no oops |

r2 was discarded (the session was interrupted while it booted).

## 2. Protocol flow observed

### 2.1 Host side (PMCA)
`pmca/commands/usb.py installCommand` → `installApp` → `pmca/installer.install`:
1. MTP device found (`SonyMtpExtCmdDevice`, 054C:08D3) → `switchToAppInstaller()` = ext-cmd **(5,2)
   NotifyScalarDlmode**.
2. Camera drops the gadget; the backend waits for the next ATTACHED (`A5K_USB_REATTACH_WAIT`) and finds the
   `SonyMtpAppInstallDevice` (VendorExtensionDesc contains `sony.net/SEN_PRXY_MSG:`; ops 0x9488/0x9489/0x948C/0x948D).
3. PMCA starts `LocalMarketServer` (TLS via tlslite, cert `certs/localtest.me.pem`, 127.0.0.1:4443), converts the
   APK into an SPK (`spk.dump`) and builds the XPD (`getXpdResponse('0', baseUrl)`).
4. Proxy-message loop over the four MTP ops: `sendInit` → camera Hello (protocols `TCPT`, `REST`) → REST
   `POST /task/start` (XPD) → the camera's `ProxyConnect` messages open TCP sockets that PMCA connects to its local
   server (host name ignored) → `ProxyData` both ways (TLS records) → `ProxyEnd`/`ProxyDisconnect` → REST
   `/task/progress` (printed) → REST `/task/complete` → `sendEnd`.
5. Market server: first `POST /` (device info) answered with the `dlandinstall` action pointing at
   `https://127.0.0.1/`; `GET /` returns the SPK; final `POST /` stores the result.

PMCA output (r1, identical in r3 and r5):
```
Sony Corporation ILCE-5000 is a camera in MTP mode
Switching to app install mode
Waiting for camera to switch...
Waiting for the emulated camera to reconnect...
Package: dev.oein.a5kui
Version: a5k-2.2-complete
Starting task
Starting communication
Uploading 0% / 100%, Downloading 29% / 64% / 98% / 100%, Installing 0% / 100%, Uploading 0% / 100%
Task completed successfully
```
(one status per line in the real output.)

### 2.2 Camera side (logcat, r3; Android clock, about 7 s from switch to completion)
| Time | Component | Event |
|---|---|---|
| 00:04:04.02 | DAConnectionManagerService | `onChange: state = 2` (USB app-download mode) |
| 00:04:04.07 | ActivityManager | starts `com.sony.scalar.dlsys.scalarausbdlapp/.UsbConnectionActivity` |
| 00:04:04.63 | AppDl (`libUsbAppDlSvr`) | `DLSvr_SetCallback`, `DLSvr_Start` (MTP app-download server) |
| 00:04:04.69 | LibJniUsbDlApp | `onHello` |
| 00:04:04.98 | RestParser | `POST /task/start REST/1.0` (PMCA's XPD) |
| 00:04:05.26 | AppDl | `DLSvr_Connect returns 62` (proxy TCP socket) |
| 00:04:05.51 | LibJniUsbDlApp | `SSL_connect: ... SSL negotiation finished successfully`, `POST / HTTP/1.1` with `{"deviceinfo":{... "name":"ILCE-5000","fwversion":"1.10", ... "battery":98}}` |
| 00:04:05.57 | ActionControl | `{"actions": [{"command": "dlandinstall", "args": "https://127.0.0.1/", ...}]}` |
| 00:04:05.59 | ActivityManager | `DownloadProcessActivity`; second TLS session, `GET / HTTP/1.1` |
| 00:04:06.00 | DownloadProcessActivity | `onDownloadDataInfoReceived filename=app.1.spk size=382428` → `/data/local/tmp/com.sony.scalar.dlsys.scalarausbdlapp/app.1.spk` |
| 00:04:07.3 | ScalarAInstaller `PackageInstallerActivity` | battery level 98, `isBatteryTooHeat` false; `DecryptTask` → `DECRYPT_FINISHED: ret=0` (SPK → `app.apk`) |
| 00:04:09.3 | PackageManager / installd | `Scanning package dev.oein.a5kui`, `New package installed in /data/app/dev.oein.a5kui-1.apk` |
| 00:04:10.1 | ScalarALauncher.Model | `PACKAGE_ADDED`, `mAllAppsList.addPackage dev.oein.a5kui` (`entry.title = a5000 UI`) |
| 00:04:10.17 | InstallAppProgress | `packageInstalled ... returnCode=1` |
| 00:04:10.88 | LibJniUsbDlApp | `POST /task/complete` `{"resultCode":0,"message":"Successfully completed"}`, `onBye code=0` |
| 00:04:11.4-11.7 | DAConnectionManagerService | state 4 → 5; the usbdlapp temp dir is deleted; the camera goes back to normal operation |

Screen: "설치 / 설치 중... 끄지 마십시오." ("Install / Installing... do not turn off") with a spinner while installing
(r3, screendump 10 s after start). After the session the camera is in playback ("메모리 카드 없음. 재생할 수 없음." = no
memory card, cannot play back; no card was inserted), which is stock behaviour after a USB session from playback
[? whether the real camera returns to playback or to shooting].

### 2.3 Launch (r3)
MENU → "애플리케이션" (Application) tile → "애플리케이션 목록" (Application List) shows **a5000 UI**, OpenMemories: Tweak,
ILCE Remote (both already on the owner's NAND clone), PlayMemories Camera Apps, Application Management, Smart Remote
Embedded. Enter on "a5000 UI":
```
ActivityManager: Starting: Intent { act=android.intent.action.MAIN cat=[android.intent.category.LAUNCHER] ... cmp=dev.oein.a5kui/dev.oein.ilceremote.MainActivity }
ActivityManager: Start proc dev.oein.a5kui for activity dev.oein.a5kui/dev.oein.ilceremote.MainActivity: pid=743
DAConnectionManagerService-JNI: resume:dev.oein.a5kui:dev.oein.ilceremote.MainActivity:
CameraSeqHAL / ILCERemote: ... key down scan=207 (PLAY key reaches the app)
```
The panel shows the app's own shooting OSD (mode icon, MF, battery 98 %, "Connecting to saved Wi-Fi...", NEAR/FAR
focus bar, 30", ±0.0, ISO AUTO). The `dalvikvm` "Could not find class" lines are the app's optional newer-body APIs
(`didep.Media`, `scalar.media.MediaPlayer`, ...) that this firmware lacks; the app handles them. They are not
emulator issues.

## 3. What the emulator had to provide (all already present)
* USB: the MTP gadget, the (5,2) mode switch (gadget stop → app-install gadget start → pull-up), bulk IN/OUT and
  interrupt endpoints at the throughput needed for a 382 KB SPK (download finished in under 1 s of emulated time).
* TLS inside the camera (OpenSSL in `libJniUsbDlApp`): handshake succeeded with the guest clock at 1 January (files dated 1980; the clock is not set).
  The camera does not reject PMCA's certificate on date grounds (the same was already true for the file-less
  `install` test).
* Battery: the installer checks level (98 % from the battery model) and temperature (`temperature=1`, not too hot).
  Both pass. A level below the installer's threshold would end in "Error completed (Low battery for install)".
* Storage: `/data` on the emulated NAND (`freespace` 97 MB reported). **No SD card is needed.**
* Persistence: the install writes `/data/app`, `/data/dalvik-cache`, `/data/system/packages.xml` on the NAND clone, so
  the app survives a reboot of that clone [? not re-checked after a reboot in these runs].

## 4. Where it could break (none observed)
| Failure string in `ScalarAUsbDlApp` | Condition | Emulator status |
|---|---|---|
| `Error completed (SSL error)` | TLS handshake / cert | passes |
| `Error completed (Low battery for install)` / `(Battery is too heat for install)` | battery service level / temperature | passes (98 %, temp 1) |
| `Error completed (No storage for download/install)` | `/data` free space | passes |
| `Error completed (USB driver error)` | MTP proxy transport | passes |
| `Error completed (Invalid contents for install)` | SPK decrypt (`libjniaeshelper.so`, `librsaforinstaller.so`) | passes |

## 5. Side observation: one av-cam oops after an install (not reproduced)
r1, after the install the camera was in playback; a shutter half press showed "촬영 모드 전환" (switching to shooting
mode) and the camera froze. The Sony klog (`tools/klog.py`) shows, at 358 s:
```
Internal error: Oops: 17 [#1] PREEMPT SMP
PC is at 0x4394aa4c   LR is at 0x437dee4d   Process liro-kliro_113
Entering exception monitor.
```
This is the same signature as the task-113 NULL `memcpy` source (`[obj+0xC38]`) in `docs/RE_GUEST_SHELL.md` §2 (seen
there with the UART0 console conflict). The HMP screen stays at the last frame and the guest shell disappears.
r5 repeated the same sequence (install → playback → half press, kept running past 400 s) and r4 did PLAY → half
press without a cable: **no oops in either**. So it is intermittent, not caused by the install path, and most likely
the av-cam engine stability family (`docs/RE_ENGINE_STABILITY.md`) [?]. It belongs to an engine-stability RE task,
not to the USB model.

Also noted: when the camera turns itself off by the idle power save, a key press restarts it; sending keys long after
an install without checking the screen can hide such events. (The `sub-micro off data: factor 0, next 0x1f` line in the
QEMU log appears in every boot and is not related.)

## 6. Spec for a code agent

Nothing is missing in the hardware model for this path. No code change is required. Optional items, each small:

1. **Regression test (recommended).** Add the acceptance test below to the USB test notes (README USB section and
   `docs/RE_USB_HOST_PATH.md` §9 table: new row "`install -f <apk>` → works").
2. **README / RE_USB_HOST_PATH status.** RE_USB_HOST_PATH §3.3 still says `install` "belongs in a later milestone"
   and lists the RTC/HTTPS concern as open; both are now answered (works, no date problem). Update the text.
3. **No new machine option is needed.** In particular do not add a battery or date override for this path.

### Acceptance test
Preconditions: fresh clone of the pristine NAND, emulator started with `-M usb-socket=127.0.0.1:<P>`, camera at the
shooting screen (about 3-4 min after start), any APK with minSdk ≤ 10 whose package is not already installed.
1. `A5K_USB=127.0.0.1:<P> python pmca-console.py install -d socket -f <app.apk>` exits 0 and prints
   `Task completed successfully` (expected: within about 1 min wall time; the camera side takes about 7 s of
   emulated time).
2. During the run a screendump shows the installer screen ("Installing... do not turn off", spinner).
3. MENU → Application → Application List shows the app's label.
4. Enter on that entry starts the app (panel shows the app's UI; optional with `A5K_SHELL`:
   `chroot /android /system/bin/logcat -d | grep "Start proc <package>"`).
5. Optional: `ls /android/data/app` shows `<package>-1.apk`.

## 7. Open items
* [?] Persistence of the installed app across a cold boot of the same NAND clone (expected yes; not tested here).
* [?] `install -i` / `install -a <package>` (app-store flow) needs PMCA to download from GitHub on the host; the camera
  side is the same as `-f`, so it is expected to work, but it was not run (no internet download was done).
* [?] Whether a real camera returns to playback or shooting after the USB session (§2.2).
* The intermittent task-113 oops of §5 → engine stability RE.
