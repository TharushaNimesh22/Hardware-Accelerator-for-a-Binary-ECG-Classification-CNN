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

## 2026-11-20: CNN model (binary beat classifier, final training run)

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
| Decision-bias calibration | ABN logit offset = −2.750 (tuned on validation: val F1 0.879 vs 0.500 without offset; folded into fc2 bias) |
| Model version | Final run of 2026-11-20. `export_binary/` must come from this same run. Re-run the step 1 check and rebuild the firmware if the export was regenerated |

### Float model on DS2 (unseen patients)

| Metric | Value |
|---|---|
| Sensitivity (Se, ABN recall) | 0.481 |
| Specificity (Sp, N recall) | 0.998 |
| Positive predictivity (+P, ABN precision) | 0.966 |
| F1 (ABN) | 0.642 |
| Accuracy | 0.941 |
| Confusion matrix [[TN, FP], [FN, TP]] | [[44127, 91], [2827, 2616]] |
| Detected as ABN per AAMI subtype | V: 79.0 %, F: 14.2 %, S: 1.0 % |

### int8 integer model on DS2

| Metric | Value |
|---|---|
| int8 vs float agreement | 99.95 % |
| Sensitivity (Se, ABN recall) | 0.479 |
| Specificity (Sp, N recall) | 0.998 |
| Positive predictivity (+P, ABN precision) | 0.966 |
| F1 (ABN) | 0.640 (float 0.642, difference −0.0017) |
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
| Program RAM | 32 KB (step 2.0–2.1) → 64 KB (from step 2.2) |
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
| Regression simulation | n/a | Identical to step 2.1 (`hw_sw_match,YES`, 69 beats) |

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

## 2026-11-21: Step 2.3: CNN software self-test on PicoRV32

| Item | Value |
|---|---|
| Test vectors | 2 (1 N, 1 ABN) from DS2, `make_test_vectors.py ... 2` |
| Result | **2/2 PASS**: logits and class bit-exact with the Python integer model |
| Cycles per beat (avg / min / max) | 16,277,439 / 16,277,359 / 16,277,519 |
| Time per beat @ 50 MHz | 325.5 ms |
| Cycles per MAC | ≈ 111 (146,624 MACs per beat) |
| CPU config | PicoRV32 RV32IM, `ENABLE_MUL` (sequential multiplier), GCC 15.2.0 `-O2` |
| Simulation wall time | 9 min 11 s (ModelSim 2020.1) |
| Note | Vector 1 (true ABN) is predicted N by both the Python and RISC-V models (a model error, not an implementation error) |

## 2026-11-21: Step 2.4: PicoRV32 with DSP multiplier (ENABLE_FAST_MUL)

| Item | ENABLE_MUL (sequential) | ENABLE_FAST_MUL (DSP) |
|---|---|---|
| CNN self-test | 2/2 PASS | 2/2 PASS (identical logits) |
| Cycles per beat (avg) | 16,277,439 | **10,891,005** (1.49× faster) |
| Time per beat @ 50 MHz | 325.5 ms | 217.8 ms |
| Cycles per MAC | ≈ 111 | ≈ 74 |
| PicoRV32 logic cells / registers / DSP | 2,712 / 1,109 / 0 | 2,444 / 921 / 8 |
| Total LEs / registers / DSP | 7,114 / 4,167 / 2 | 6,850 / 3,979 / 10 |
| Fmax (Slow 1200 mV 85 °C) | 55.6 MHz (slack 2.014 ns) | 53.55 MHz (slack 1.325 ns) |
| Simulation wall time (2 beats) | 9 min 11 s | 5 min 49 s |

**Decision:** ENABLE_FAST_MUL is the official CPU configuration. **CNN software baseline = 10,891,005 cycles/beat.**
Note: a ~10× faster multiply only gives 1.49× on the CNN, because most cycles are instruction overhead
(loads, address arithmetic, loop control, ~3–4 CPI). This motivates a dedicated MAC datapath.

## 2026-11-21: Step 2.5: Full pipeline in software (Pan-Tompkins HW → CNN SW), real MIT-BIH data

| Item | Value |
|---|---|
| Record | MIT-BIH 233 (DS2 test patient), first 8 s, 1,600 samples @ 200 Hz, lead MLII |
| Command | `ecg_to_hex.py --mitdb 233 --mitdb-dir ..\model\mitdb --seconds 8` |
| Pan-Tompkins | HW 1,632 cycles vs SW 772,909 cycles (473×), hw_sw_match YES, 10 beats detected |
| CNN beats classified | 3 (`CNN_MAX_BEATS = 3`; beats need a previous and a next beat) |
| Bit-exact check (`verify_cnn_beats.py`) | **3/3 PASS** (RR features, logits, class == Python integer model) |
| Classification vs annotations | N→N, **V→ABN**, N→N (3/3 correct) |
| CNN cycles per beat (avg) | 10,888,152 (218 ms @ 50 MHz) |
| Simulation wall time | 9 min 00 s |
| Note | Rule-based rhythm summary reports "possible AF"; record 233 has frequent PVCs. The heuristic is misled by ectopic beats in an 8 s window, while the CNN's per-beat labels identify the cause (abnormal beats) |

## 2026-11-21: Step 3.0: Shared-memory interface (CNN accelerator)

| Item | Value |
|---|---|
| Spec | `cnn/docs/interface_spec.md` (single 128 KB shared memory, port A CPU / port B accelerators) |
| Weight block (option C, loaded once) | 5,154 words = 20,616 bytes at 0x0_C000 |
| Descriptors | 8 words per beat at 0x1_1800 (n_R, 4 RR, logits, cycles, status) |
| ECG ring buffer | 32 KB at 0x1_8000, 16,384 samples (2 × int16 per word), mask 0x1FFF |
| Golden shared-memory model | 32/32 beats bit-exact with the notebook |
| Layout check (ModelSim, 30% random port stalls) | PASS (4,096 window samples via ring addressing, incl. wrap) |
| Expected transfers | LOAD_WEIGHTS 5,154 reads (once); per beat 69.5 reads + 3 writes |
| Reference: weights streamed per beat (option B) | +5,154 reads per beat (≈ 74× more per-beat traffic) |

## 2026-11-21: Step 3.1: MAC and requantisation units

| Item | Value |
|---|---|
| MAC unit | int8 × int8 → int32 accumulate, 1 MAC/clock, bias load via `clear` (`cnn/rtl/mac_unit.v`) |
| Requant unit | (acc·M + 2^(sh−1)) >> sh, clamp; modes ReLU-int8 / int8 / int16; 2-stage pipeline, 1 value/clock (`cnn/rtl/requant.v`) |
| Test A (MAC, 2,000 random sequences of 1–600 products) | 0 errors |
| Test B (requant, 50,000 random cases, all modes, sh 1–47) | 0 errors |
| Test C (real model: 4,864 cases: input/RR quant, conv1, fc1, fc2) | 0 errors |
| Largest accumulator in the model | 53,825 (< 2^16); shifts 17–26 |
| Test sensitivity check | Planted bugs (no rounding; broken sign bit) detected by A/B/C |

## 2026-11-21: Step 3.2: CNN accelerator front end

| Item | No stalls | 30 % random grant stalls |
|---|---|---|
| Bad weight-block address | magic error detected | magic error detected |
| LOAD_WEIGHTS (once) | 5,154 reads, 5,159 cycles | 5,154 reads, 7,374 cycles |
| q0 (128) + qrr (4), 32 beats | bit-exact | bit-exact |
| Window reads | 16 beats × 64 words, 16 × 65 (even/odd start, ring wrap) | same |
| Shared-memory reads per beat | 69.5 (= golden model) | 69.5 |
| Front-end cycles per beat | 212 | 242 |

Local parameter memory: dual-port M9K, 5,632 words (loaded once). Weight transfer happens once per power-up,
not per beat.

## 2026-11-21: Step 3.3/3.4: Convolution engine (Conv1d + ReLU + MaxPool2), 1 MAC/clock

| Layer | Shape | MACs/beat | Cycles/beat | Cycles/MAC | Result (32 beats, ModelSim) |
|---|---|---|---|---|---|
| conv1 | 1×128 → 8×64, k=7 | 7,168 | 7,206 | 1.005 | PASS |
| conv2 | 8×64 → 16×32, k=5 | 40,960 | 41,030 | 1.002 | PASS |
| conv3 | 16×32 → 32×16, k=5 | 81,920 | 82,054 | 1.002 | PASS |
| Total | n/a | 130,048 | 130,290 | 1.002 | n/a |

Design: one generic engine for all conv layers; output-stationary (accumulator in a register, no
partial sums stored); requant + ReLU + max-pool fused into the write path (pre-pool map never stored);
weights from the local parameter memory (no shared-memory traffic during convolution).
Overhead: 4 setup cycles per output channel + pipeline drain.
Note: a testbench localparam-width bug (`CIN*LEN` evaluated in 8 bits by ModelSim) was found and fixed;
the RTL was unaffected.

## 2026-11-21: Step 3.5: Fully connected engine (fc1, fc2, argmax)

| Layer | Shape | MACs/beat | Cycles/beat | Cycles/MAC | Result (32 beats, ModelSim) |
|---|---|---|---|---|---|
| fc1 | [p3 512 | qrr 4] = 516 → 32, ReLU int8 | 16,512 | 16,646 | 1.008 | PASS |
| fc2 + argmax | 32 → 2 logits (int16) | 64 | 78 | 1.219 | PASS, 32/32 classes |

Design: one FC engine for both layers (mode = ReLU-int8 / int16); RR features stored at positions
512–515 of conv3's output buffer (no concatenation copy); weights read strictly sequentially;
argmax = (logit1 > logit0).
Sum of all verified blocks: ≈ 147,230 cycles per beat vs 10,891,005 in software (≈ 74× expected).

## 2026-11-21: Step 3.6: Complete CNN accelerator, end to end (ModelSim)

| Metric | No stalls | 30 % random grant stalls |
|---|---|---|
| Shared memory after batch vs golden model | identical | identical |
| LOAD_WEIGHTS (once) | 5,154 reads, 5,158 cycles | 5,154 reads, 7,375 cycles |
| Transfers per beat | 69.5 reads + 3 writes | 69.5 + 3 (1,088 stall cycles / 32 beats) |
| Cycles per beat (end to end) | 147,235 | 147,269 |
| Cycles per MAC | 1.004 | 1.004 |
| Speed-up vs software (10,891,005) | 74.0× | 74.0× |

Time per beat @ 50 MHz: 217.8 ms (SW) → 2.94 ms (HW). Shared-memory port used 72.5 / 147,235 cycles (0.05 %).
RUN_BATCH before LOAD_WEIGHTS correctly rejected (error flag).

## 2026-11-21: Step 3.7: CNN accelerator FPGA cost (stand-alone, Quartus Prime Lite 22.1std, EP4CE115F29C7)

| Resource | Used | % |
|---|---|---|
| Logic elements | 4,312 | 4 % |
| Registers | 1,900 | n/a |
| Memory bits | 198,730 | 5 % (weight memory 180,224 single TDP copy + 2 × 8,192 act. buffers + 2,080 window) |
| M9K blocks | 36 / 432 | 8 % |
| Embedded multiplier 9-bit elements | 16 / 532 | 3 % (4 used as 9×9 multipliers) |
| Fmax (Slow 1200 mV 85 °C) | 61.53 MHz | slack +3.749 ns @ 50 MHz |

Interface ports as virtual pins (157). 0 errors.

### Resource utilisation by entity

| Module | Logic cells | Registers | Memory bits | M9K | DSP elements (9×9) |
|---|---|---|---|---|---|
| cnn_accel (sequencer, registers, counters, buffer muxes) | 728 | 383 | 0 | 0 | 0 |
| Activation buffer A (1,024 × 8) | 0 | 0 | 8,192 | 1 | 0 |
| Activation buffer B (1,024 × 8) | 0 | 0 | 8,192 | 1 | 0 |
| cnn_conv (conv1–3 engine) | 1,025 | 419 | 42 | 1 | 7 (3) |
| cnn_fc (fc1–2 engine) | 948 | 360 | 0 | 0 | 5 (1) |
| cnn_frontend (weight load, descriptor, window, quantisation, weight memory) | 1,611 | 738 | 182,304 | 33 | 4 (0) |
| **Total** | **4,312** | **1,900** | **198,730** | **36** | **16 (4)** |

### RAM summary (Fitter)

| Memory | Mode | Depth × width | Notes |
|---|---|---|---|
| Weight memory `pmem` (cnn_frontend) | **True Dual Port** | 5,632 × 32 | single copy ✔; 32 of the 33 front-end M9K |
| Window buffer `wbuf` (cnn_frontend) | Simple Dual Port | 65 × 32 | 1 M9K |
| Activation buffer A | Simple Dual Port | 1,024 × 8 | 1 M9K |
| Activation buffer B | Simple Dual Port | 1,024 × 8 | 1 M9K |
| cnn_conv shift register (`altshift_taps`) | Simple Dual Port | 3 × 14 | pipeline tag delay mapped to RAM by Quartus; 1 M9K |

### Notes / optimisation opportunities
* The weight memory needs only 180,224 bits (≈ 20 M9K of capacity) but uses 32 M9K: its depth of 5,632
  is not a power of two, so Quartus builds an 8,192-deep array. Splitting it into a 4,096-deep and a 1,536-deep part
  would save about 10 M9K.
* Quartus moved a 3-deep pipeline delay in cnn_conv into an M9K (`altshift_taps`). Turning off
  "Auto Shift Register Replacement" keeps it in registers and frees 1 M9K.
* Fmax 61.5 MHz > SoC Fmax 53.5 MHz: the accelerator will not limit the system clock.

### Step 3 summary (P = 1 MAC)
147,235 cycles/beat (1.004 cycles/MAC), 74.0× vs software (10,891,005 cycles/beat);
69.5 reads + 3 writes per beat on the shared memory, weights 5,154 reads once;
cost 4,312 LE + 1,900 registers + 36 M9K + 16 DSP elements; Fmax 61.5 MHz.

## Step 5.1 – Merge the memories (single 128 KB shared memory)
- Change: program RAM + ECG ROM -> one 128 KB true dual-port shared memory
  (port A = CPU, port B = Pan-Tompkins). Image built by tools/make_mem_image.py
  (program 0x0000, weight block 0xC000, record header 0x12800, ECG ring 0x18000).
- Software CNN now reads weights from the shared weight block (fw/beatcnn_shm.c).
- Record 233, first 8 s (N = 1600): Pan-Tompkins hw_cycles = ____, hw_sw_match = YES
- CNN beats (software): 529 N, 614 ABN, 762 N — 3/3 bit-exact vs Python -> PASS
- Software CNN: cnn_cycles_avg = ____ cycles/beat (step 2.5: ~10.89 M)
  -> slightly slower because the weights are read through a pointer, not as compile-time constants

## Step 5.2 – Pan-Tompkins as a shared-memory master + port-B arbiter
- Pan-Tompkins reads the packed ECG ring itself (2 samples/word) and writes R-peaks to the
  shared-memory peak list (word 0x4800). Internal 512-entry peak RAM and 0x3000_1000 window removed.
- New rtl/shm_arbiter.v: M0 = Pan-Tompkins (priority), M1 = CNN (step 5.3).
- Stand-alone tb_pt (record 233, 8 s, N = 1600, 10 peaks):
  | run | stall | cycles | reads | writes | stalls | result |
  |---|---|---|---|---|---|---|
  | SRC=0 | 0 %  | 1609 | 800 | 10 | 0    | PASS |
  | SRC=0 | 30 % | 1617 | 800 | 10 | 321  | PASS |
  | SRC=1 | 30 % | 1623 | 800 | 10 | 351  | PASS |
  | SRC=0 | 75 % | 3303 | 800 | 10 | 2480 | PASS |
- Shared-memory reads per record: 1600 -> 800 (-50 %); writes = 1 per beat.
- 4-word FIFO hides 30 % grant loss (+0.5 % cycles); pipeline is gap tolerant (exact at 75 %).
- Full SoC: hw_cycles = ____, hw_sw_match = YES, CNN beat 529 bit-exact -> PASS

## Step 5.3 – CNN accelerator in the SoC (arbiter master 1)
- cnn_accel (step 3, unchanged) at 0x4000_0000; ARB_CONFLICTS register at 0x2000_0020.
- LOAD_WEIGHTS started by the CPU, overlapped with the Pan-Tompkins run (arbiter test):
  load_reads = 5154 (weight block moved once), weights_valid = 1, load_check PASS
  load_cnn_cycles = 5968 (≈5158 when alone), load_stalls = 810, arb_conflicts = 810
  (= 800 PT reads + 10 PT peak writes)
- Pan-Tompkins: stalls 0, hw_core_cycles = 1609, hw_cycles = 1647, hw_sw_match YES
  -> fixed priority to PT works; the CNN absorbs all conflicts
- SW CNN beat 529 still bit-exact.

## Step 5.4 – CNN driver: descriptors + RUN_BATCH, every beat on the accelerator
- CPU writes 8-word descriptors (n_R + 4 RR); one RUN_BATCH per ≤64 beats; results read from descriptors.
- Record 233, 8 s: 8 beats (6 N, 2 ABN), 1 batch, descriptors_not_done = 0
- verify_cnn_beats.py: 8/8 bit-exact -> PASS;  SW check: 2/2 match -> hw_sw_cnn PASS
- HW CNN: 147,235 cycles/beat (= step 3.6), total 1,177,883 cycles for 8 beats
- Shared memory (CNN): 555 reads (69.4/beat = 5 descriptor + 64/65 window), 24 writes (3/beat), 0 stalls
- SW CNN: 10,830,891 cycles/beat  -> CNN speed-up 73x
- CPU descriptor set-up: 7,468 cycles (≈930/beat, 0.6 % of the HW CNN time)
- Firmware (gcc): 10,324 bytes (21 % of the 48 KB program region)

## Step 5.5 – Full-system simulation and system performance (MIT-BIH 233)
- 8 s: 8/8 beats bit-exact; 60 s: 98/98 beats bit-exact (verify_cnn_beats.py PASS),
  Pan-Tompkins hw_sw_match YES, SW CNN check 1/1 (CNN_SW_CHECK = 1).

| Quantity | 8 s | 60 s |
|---|---|---|
| Beats detected / classified | 10 / 8 | 100 / 98 |
| CNN batches (≤ 64) | 1 | 2 |
| Pan-Tompkins (HW) cycles | 1,647 | 12,051 |
| Descriptor set-up (CPU) cycles | 7,426 | 230,146 |
| CNN (HW) cycles | 1,177,918 | 14,429,138 |
| CNN cycles per beat | 147,235 | 147,235 |
| **Total cycles (step 5)** | **1,186,991** | **14,671,335** |
| **Total time @ 50 MHz** | **23.7 ms** | **293.4 ms** |
| Faster than real time | 337 x | 204 x |
| Step 2 (HW PT + SW CNN), est. | 86.7 M (1.73 s) | 1,061.7 M (21.2 s) |
| All software, est. | 87.5 M (1.75 s) | 1,067.8 M (21.4 s) |
| **Speed-up vs step 2** | **73.0 x** | **72.4 x** |
| **Speed-up vs all software** | **73.7 x** | **72.8 x** |
| Shared-memory reads PT / CNN | 800 / 555 | 6,000 / 6,817 |
| Shared-memory writes PT / CNN | 10 / 24 | 100 / 294 |
| CNN transfers per beat | 72.4 | 72.6 |
| Weight load (once, at boot) | 5,154 reads | 5,154 reads |

- Time split (60 s): CNN 98.35 %, CPU set-up 1.57 %, Pan-Tompkins 0.08 %.
- Weights loaded once: CNN traffic 72.6 transfers/beat vs ≈5,227 if streamed per beat (−98.6 %).
- Batching: cycles/beat identical in batch 1 and 2 (no per-batch overhead visible).
- Set-up cost grows to ≈2,350 cycles/beat once the 32-beat RR history is full
  (re-summing + divisions) -> candidate optimisation (running sums) for step 4.
- Model behaviour (identical in HW and Python): 17/17 ABN predictions are true V beats;
  11 V and 2 S beats predicted N (recall limitation of the model, not of the hardware).

  ## Step 5.6 – Quartus compile of the complete SoC (EP4CE115F29C7, Quartus 22.1std Lite)
- PicoRV32 + 128 KB shared memory (4 TDP lanes) + shm_arbiter + Pan-Tompkins + CNN + UART
- 0 critical warnings; Fmax (Slow 1200mV 85C) = 52.5 MHz, setup slack +0.951 ns @ 50 MHz -> PASS

| Resource | Used | Device |
|---|---|---|
| Logic elements | 11,881 | 114,480 (10 %) |
| Registers | 6,282 | |
| Memory bits | 1,249,354 | 3,981,312 (31 %) |
| M9K | 166 | 432 (38 %) |
| 9-bit multipliers | 26 | 532 (5 %) |
| Pins | 36 | 529 (7 %) |

| Block | LEs | Regs | Mem bits | M9K | DSP |
|---|---|---|---|---|---|
| Shared memory (sm0..sm3, True Dual Port) | 179 | 16 | 1,048,576 | 128 | 0 |
| cnn_accel | 4,372 | 1,898 | 198,730 | 36 | 16 |
| pan_tompkins_accel | 4,626 | 3,273 | 0 | 0 | 2 |
| picorv32 | 2,402 | 922 | 2,048 | 2 | 8 |
| shm_arbiter | 101 | 34 | 0 | 0 | 0 |
| UART + glue | 285 | 135 | 0 | 0 | 0 |

- CNN accelerator unchanged vs standalone (step 3.7: 4,312 LE, 36 M9K, 16 DSP, 61.5 MHz);
  system Fmax set by the CPU side (step 2.4: 53.19 MHz -> 52.5 MHz with arbiter + 128-M9K port B).
- Pan-Tompkins: ~2,400 registers are filter delay lines (76 x 32 bit) -> step-4 candidate
  (shift-register RAM / narrower widths).
- Image in the .sof: firmware SWCHECK=-1, MIT-BIH 233 first 60 s (12,000 samples).
- RAM Summary: sm0..sm3 = True Dual Port, Single Clock, 32768 x 8, 32 M9K each, init from mem/shm_b*.hex (.hdl.mif);
  CNN pmem = True Dual Port 5632 x 32 (32 M9K) -> shared memory and weight memory are real block RAM.
