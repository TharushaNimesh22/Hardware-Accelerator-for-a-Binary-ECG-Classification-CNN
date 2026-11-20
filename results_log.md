# Results Log: ECG Interpreter Hardware Accelerator (DE2-115, PicoRV32)

> One dated section per step: numbers, tool versions and the exact commands used.
> Fields marked _TBD_ still have to be filled in.

---

## Tools and environment

| Tool | Version / setting |
|---|---|
| RISC-V compiler | xPack `riscv-none-elf-gcc` 15.2.0, flags `-O2 -march=rv32im_zicsr -mabi=ilp32 -ffreestanding -nostdlib` |
| Build | `mingw32-make CROSS=riscv-none-elf-` (in `fw/`) |
| RTL simulator | ModelSim - Intel FPGA Starter Edition 2020.1 (`modelsim_ase`) |
| Synthesis / P&R | Quartus Prime Lite 22.1std.0 Build 915 |
| Target device | Cyclone IV E EP4CE115F29C7 (DE2-115) |
| Model training | PyTorch (notebook `MITBIH_BeatCNN_binary_hardware.ipynb`) |

---

## 2026-11-17: CNN model (binary beat classifier)

| Item | Value |
|---|---|
| Dataset | MIT-BIH Arrhythmia DB, inter-patient split (de Chazal): DS1 → train/val, DS2 → test |
| Validation patients | 106, 118, 119, 220 (from DS1) |
| Classes | N (normal) vs ABN (AAMI S + V + F); paced records and Q beats excluded |
| Sampling rate | 200 Hz (resampled from 360 Hz; same as the Pan-Tompkins accelerator) |
| Input | 128-sample beat window (48 before / 80 after the R-peak) + 4 integer RR ratios (×256) |
| RR features | `pre/avg8`, `post/avg8`, `pre/post`, `pre/avg32` (averages over previous intervals only) |
| Standardisation | Global training-set mean/std (folded into input requantisation) |
| Architecture | 3 × [Conv1d → BN → ReLU → MaxPool2] (1→8 k7, 8→16 k5, 16→32 k5) → Flatten(512) + RR(4) → FC(516→32) → ReLU → FC(32→2) |
| Parameters | 19,978 |
| MACs per beat | 146,624 |
| Weight memory (int8) | ≈ 19.5 KB |
| Quantisation | Post-training: int8 weights (per output channel), int8 activations, int32 bias, 16-bit multiplier + shift requantisation, BN folded |
| Decision-bias calibration | ABN logit offset = −2.375 (tuned on validation, folded into fc2 bias) |

### Float model on DS2 (unseen patients)

| Metric | Value |
|---|---|
| Sensitivity (Se, ABN recall) | 0.501 |
| Specificity (Sp, N recall) | 0.982 |
| Positive predictivity (+P, ABN precision) | 0.772 |
| F1 (ABN) | 0.607 |
| Accuracy | 0.929 |
| Confusion matrix [[TN, FP], [FN, TP]] | [[43412, 806], [2717, 2726]] |
| Detected as ABN per AAMI subtype | V: 80.9 %, F: 17.3 %, S: 2.9 % |

### int8 integer model on DS2

| Metric | Value |
|---|---|
| int8 vs float agreement | 99.95 % |
| Sensitivity (Se, ABN recall) | 0.479 |
| Specificity (Sp, N recall) | 0.998 |
| Positive predictivity (+P, ABN precision) | 0.966 |
| F1 (ABN) | 0.640 (float: 0.642, difference −0.0017) |
| Accuracy | 0.941 |
| Confusion matrix [[TN, FP], [FN, TP]] | [[44127, 91], [2837, 2606]] |

---

## 2026-11-20: Step 1: export check on PC

| Item | Value |
|---|---|
| Command | `gcc -O2 -I export_binary check_export.c beatcnn_ref.c -o check_export` then `check_export.exe export_binary\test_vectors` |
| Result | **32 test vectors, 0 mismatches → PASS** (C model bit-exact with Python integer model) |

---

## 2026-11-20: Step 2.0–2.1: firmware build and SoC simulation (no CNN)

### System configuration

| Item | Value |
|---|---|
| CPU | PicoRV32, RV32IM (`ENABLE_MUL`, `ENABLE_DIV`, `BARREL_SHIFTER`, counters). Note: `ENABLE_MUL` = sequential multiplier, no DSP |
| Clock | 50 MHz |
| Program RAM | 32 KB → 64 KB |
| ECG RAM | 64 KB (16K × 32-bit), true dual-port, shared by CPU and accelerator |
| UART | 115200 baud, 8N1 |
| Test record | 60 s synthetic ECG, 12,000 samples at 200 Hz, 72 bpm |

### Pan-Tompkins accelerator vs software (ModelSim, cycle-accurate)

| Item | Cycles | Time @ 50 MHz |
|---|---|---|
| Hardware (incl. CPU start/poll) | 12,036 | 0.24 ms |
| Hardware core only | 12,006 | 0.24 ms |
| Software (same algorithm, C on PicoRV32) | 6,342,612 | 126.9 ms |
| **Speed-up** | **526×** | n/a |
| HW = SW match | YES (69 beats, identical positions) | n/a |
| Interpretation | 69 beats, mean HR 72 bpm (67–78), mean RR 830 ms, RMSSD 33 ms, Normal sinus rhythm | n/a |
| Simulation wall time | 2 min 09 s (≈ 50 k cycles/s) | n/a |

Command: `vsim -c -do sim/run_sim.do` (from the `ecg_soc` root)

---

## 2026-11-20: Step 2.2: CNN compiled into firmware

| Item | Before CNN | After CNN |
|---|---|---|
| `text` (code + const weights) | 4,115 B | 26,900 B |
| `bss` (zeroed buffers) | n/a | 6,404 B |
| Total RAM footprint | n/a | 33,304 B → exceeds 32 KB by 536 B |
| Action | n/a | Program RAM enlarged to 64 KB (`soc.v`, `link.ld`, `start.S`, `makehex.py`) |
| Regression simulation | n/a | `hw_sw_match,YES`, 69 beats |

---

## 2026-11-20: Quartus baseline compile (SoC + Pan-Tompkins, no CNN accelerator)

Quartus Prime Lite 22.1std.0, full compilation successful: 0 errors, 94 warnings, **0 critical warnings**.

### Totals

| Resource | Used | Available | % |
|---|---|---|---|
| Logic elements | 7,114 | 114,480 | 6 % |
| Registers | 4,167 | n/a | n/a |
| Memory bits | 1,058,816 | 3,981,312 | 27 % |
| M9K blocks | 131 | 432 | 30 % |
| Embedded multiplier 9-bit elements | 2 | 532 | < 1 % |
| Pins | 34 | 529 | 6 % |
| PLLs | 0 | 4 | 0 % |

### Timing (Slow 1200 mV 85 °C model)

| Item | Value |
|---|---|
| Fmax (CLOCK_50) | 55.6 MHz |
| Restricted Fmax | 55.6 MHz |
| Setup slack @ 50 MHz | +2.014 ns |

### Resource utilisation by entity

| Module | Logic cells | Registers | Memory bits | M9K | DSP (9-bit) |
|---|---|---|---|---|---|
| PicoRV32 CPU | 2,712 | 1,109 | 2,048 | 2 | 0 |
| Pan-Tompkins accelerator | 4,050 | 2,927 | 8,192 | 1 | 2 |
| Program RAM (4 × 16 KB byte lanes) | 29 | 4 | 524,288 | 64 | 0 |
| ECG RAM (64 KB) | 36 | 2 | 524,288 | 64 | 0 |
| UART RX | 61 | 32 | 0 | 0 | 0 |
| UART TX | 45 | 30 | 0 | 0 | 0 |
| SoC glue / bus logic | 188 | 59 | 0 | 0 | 0 |
| **Total** | **7,114** | **4,167** | **1,058,816** | **131** | **2** |

Memory bit check: 524,288 + 524,288 + 8,192 (peak buffer) + 2,048 (CPU register file) = 1,058,816. ✔

### Observations

1. The Pan-Tompkins accelerator (4,050 LC, 2,927 regs) is larger than the CPU, mostly because its filter delay lines (12 + 32 + 4 + 32 words × 32 bit) are stored in flip-flops. They could be moved into M9K.
2. Memory dominates resource use (27 % of bits, 131/432 M9K), almost entirely the two 64 KB RAMs.
3. The timing margin is small (55.6 MHz vs 50 MHz). Re-check Fmax after adding the CNN accelerator.
4. The PicoRV32 multiplier is sequential (no DSP). Decide whether to enable `ENABLE_FAST_MUL` before measuring the CNN software baseline, for a fair speed-up comparison.

---

## Next: Step 2.3: CNN self-test on the RISC-V (32 test vectors)

| Item | Value |
|---|---|
| Test vectors matched | _TBD_ |
| Software cycles per beat | _TBD_ |
