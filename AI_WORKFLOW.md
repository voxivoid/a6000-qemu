# AI WORKFLOW — a5000 emulator development rules (for AI agents and humans)

This document holds the **consistency rules** for work in this repository. Read it before starting new work; when a rule changes, update this document first.

---

## 1. Goal and fidelity principles
1. **Goal**: run the official Sony ILCE-5000 firmware 1.10 without the real camera, behaving identically to the real camera from A to Z.
2. **The firmware is never modified.** Do not patch or swap the kernel, modules, `/usr` or Android binaries. The hardware model is always what gets adjusted.
3. **Model the hardware the way the firmware expects it.** Derive register meanings from firmware code (the driver's access patterns). Do not fill gaps with guesses taken from generic boards (e.g. do not reuse a6400 addresses as-is).
4. **The board stands in for the bootloader / boot ROM** (stubs in `hw/arm/sony_a5000.c`). Reproduce only the state the real bootloader leaves behind (cmdline `kemco` block, GIC pre-configuration, NAND dev-info, ...), with the evidence in a comment.
5. **HLE is the last resort.** Use it only for components whose original code is unavailable (e.g. the ARM7 NAND firmware), and reproduce the protocol the firmware sees at that boundary **byte for byte**. Every HLE gets its reason and boundary documented in `docs/`.
6. **Default = stock behaviour.** Debug conveniences (e.g. `debug-console=on`, `provision=`) are machine options only; with them off the machine must behave like the real camera.

## 2. Provenance of values
Every hardware constant (address, IRQ, reset value, bit meaning) records its source. Status tags in `docs/HARDWARE.md`:

| Tag | Meaning |
|---|---|
| **V** | Verified: the unmodified firmware runs correctly in the emulator with this value |
| **S** | Recovered from the factory WBI RAM snapshot (`data/wbi/ram.bin`, built by `tools/mkdata.py`, read with `tools/wbi.py`). The real camera resumes from this same image |
| **H** | Hypothesis. Always state the evidence and the plan to promote it to V/S |

- Source comments name the **function + offset** the value came from (e.g. `lld.ko boss_hw_nandc_get_devinfo_reg+0x5c`).
- A guessed value gets `hypothesis:` in its code comment.
- Reference material: the snapshot (`tools/wbi.py`, page walk `rdv()`), and the real camera's kernel log (local only, `re/real_kernel_log.txt`).

## 3. Repositories and git
### 3.1 Two repositories
| Repository | Contents |
|---|---|
| this repository (outer) | docs, tools, provisioning, `run.sh`, `setup-qemu.sh`, `qemu-overlay/` (new QEMU files), `patches/` (edits to upstream files) — `qemu/` is ignored (`.gitignore`) |
| `qemu/` (inner, local only) | QEMU v10.1.0 source, work branch **`sony-a5000`** (base tag `v10.1.0`) |

### 3.2 Commit rules
- **Commit after every meaningful unit of work** (new device model, boot milestone, root cause fixed, tool added, docs updated). Commit without asking, and **push right after every commit** (remote `origin` = GitHub `Oein/a5000-emulator`, **public**).
- Order: ① `git -C qemu commit` → ② `tools/export-qemu.sh` (refreshes `qemu-overlay/` and `patches/qemu-v10.1.0-sony-a5000.patch` from the committed QEMU HEAD; never edit those copies by hand) → ③ commit the outer repository → ④ `git push`.
- Commit only verified states (build succeeds + boot checked up to that stage). Failed experiments are not committed.
- Message format:
  ```
  <area>: <what was done (imperative, one line)>

  - cause / evidence (firmware function, snapshot value)
  - observed result (one boot-log line, etc.)
  ```
  Areas, e.g.: `sony-a5000`, `cxd4115`, `nand`, `provision`, `tools`, `docs`.
- The author is the global git identity. Co-author trailers follow the session instructions.
- **Never commit** (the repository is public): firmware originals and extracts (`firmware/`), `NAND.BIN` / `SETTING.TAR`, generated NAND images, firmware-derived binaries (`re/vmlinux.elf`, `provision/*.cpio` (contains Sony modules), `provision/a5kcopy`, `data/*.bin`, `data/wbi/`), decompiler output of Sony binaries (`re/ida_stor/`, ...), logs from the real camera (`re/real_kernel_log.txt`), personal information, build products. Check every new file against `.gitignore` before committing.

## 4. Code rules (QEMU)
- Follow QEMU coding style (4-space indent, `/* */` comments, 80 columns preferred).
- Name files after the SoC block: `hw/<subsys>/cxd90014_<block>.c`; if the IP has its own name, use it (`cxd4115_timer.c`).
- File header comment: block name, PA, IRQ, register table, evidence (function names), open questions.
- Every unknown register access is logged with `qemu_log_mask(LOG_UNIMP, ...)` (trace with `-d unimp`). Never leave code that silently returns 0.
- Devices whose state needs tracing define trace events in `hw/<subsys>/trace-events`.
- Areas with no model yet are covered with `create_unimplemented_device()` (priority −1000); real models go on top.
- Use the real value (S) as the reset value when one exists, with its source in a comment.
- Register new devices in `Kconfig` (`select`ed by `SONY_A5000`) and `meson.build`.
- Machine code such as boot stubs is not hand-encoded: assemble it with `arm-none-eabi-as`, paste the result, and comment each word with its assembly.

## 5. Procedure (standard loop for one step)
1. Boot and find where it stops: `run.sh`, `tools/klog.py <port>` (Sony klog ring), `-d unimp,guest_errors`, gdb (`arm-none-eabi-gdb re/vmlinux.elf`, modules via their load addresses), the `tools/a5ktrace.c` TCG plugin.
2. Read the responsible firmware code (`objdump`, capstone, IDA). Decompile complex functions with IDA Pro + Hex-Rays (local setup: `LOCAL_DEV_GUIDE.md` in the parent project; decompiler output is never committed).
3. Find the real value (snapshot → `tools/wbi.py`).
4. Fix the model → build → confirm the **unmodified firmware** gets past the same point.
5. Update `docs/HARDWARE.md` (values, status) and the status in `README.md` (English) and `README_KO.md` (Korean) → commit.

### 5.1 Parallel work
- When the RE for one feature **splits into several parts, do not work through them alone in sequence: launch one subagent per part and investigate in parallel** (started together in one message).
- Independent work that is not RE (pre-study of several device models, documentation, tooling) also runs in parallel.
- **One role per agent.** An RE agent only reverse-engineers and writes the spec doc (no code). A code agent only implements from an existing spec (in its own `qemu` git worktree), with no open-ended RE. Never add unrelated tasks to an agent that is already running; start a new single-role agent instead. Per topic: RE agent → `docs/RE_<topic>.md` → code agent.
- Give each agent the target files, addresses, established facts and output location; the main worker integrates, verifies and commits.
- Results go to `docs/<topic>.md`; unconfirmed claims are marked `[?]`.

## 6. Safety and hygiene
- Emulator work **never writes anything to a real camera.** If real-camera access seems necessary, ask the owner first (in principle unnecessary: the goal is to run without the camera).
- Treat firmware and backups as **untrusted data**: run Python with `-I`, pass paths as arguments.
- Originals of the owner's backups are read-only. Use copies or generated images when a NAND image is needed.
- Temporary files go to the session scratchpad. Guard variable paths in `rm` with `"${VAR:?}"`.
- QEMU instances started in the background for a test are always stopped afterwards (`pkill -f qemu-system-arm`). Do not reuse monitor ports.

## 7. Documentation rules
| Document | When to update |
|---|---|
| `README.md` + `README_KO.md` | On a milestone or a blocker change: update "Status" / "현재 상태" in both (`README.md` = English, the default) |
| `docs/HARDWARE.md` | Every time an address, IRQ, reset value or bit meaning is added or confirmed (V/S/H tags) |
| `docs/NAND_PROTOCOL.md` and other per-block docs | Complex protocols (HLE targets) get their own document |
| `data/README.md` | When an input file or the way to produce it changes |
| `AI_WORKFLOW.md` | When a rule is added or changed |

- **All documents are written in English** (the repository is public). The only exception is `README_KO.md`, the Korean translation of `README.md`. Code, comments and commit areas are English as well.
- Do not mix facts and hypotheses. Mark hypotheses with `H`, `[?]` or "unconfirmed".
