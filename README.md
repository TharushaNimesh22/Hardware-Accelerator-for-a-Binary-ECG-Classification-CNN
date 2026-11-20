# ECG interpreter accelerator: PicoRV32 RISC-V + Pan-Tompkins on the DE2-115

A recorded ECG sits in on-chip RAM. A RISC-V CPU (PicoRV32, RV32IM) starts a hardware
Pan-Tompkins QRS detector that reads that same RAM directly, so no data gets copied. The CPU then
works out the heart rate, RR intervals, HRV and rhythm, and sends a CSV report to the PC over RS-232.
The firmware also runs the identical algorithm in software. That gives you a check that hardware and
software agree, and a measured speed-up.

```
               +-------------------------------- DE2-115 / Cyclone IV EP4CE115 ---------------------------+
 RS-232  <---- | UART TX/RX  <--+                                                                         |
 (PC)          |                |        PicoRV32 bus (1 wait state)                                      |
               |  PicoRV32  ----+------+-----------------+----------------------+                         |
               |  RV32IM               |                 |                      |                         |
               |               32 KB program RAM   64 KB ECG RAM (dual port)  Accelerator regs + PEAK[]   |
               |                                         | port B               ^                         |
               |                                         +--> LPF->HPF->d/dt->x^2->MWI->peak detect ------+
               |                                              (1 sample / clock, 50 MHz)                  |
               +------------------------------------------------------------------------------------------+
```

## Quick start

1. **Make the ECG memory image** (on your PC, from the project root)
   ```
   pip install numpy scipy wfdb
   python tools/ecg_to_hex.py --mitdb 100 --seconds 80      # MIT-BIH record 100 from PhysioNet
   # or: --csv my_ecg.csv --fs 500 --col 1   |   --synthetic --hr 72  (pip install neurokit2)
   ```
   This writes `data/ecg.hex` (the RAM init file), `data/ecg.npy` and `data/expected_peaks.txt`
   (the golden-model answer). Everything is resampled to 200 Hz. RAM holds up to 16383 samples, about 82 s.
2. **Build the firmware**: `cd fw && make` (riscv64-unknown-elf-gcc), or `make ZIG=1` (`pip install ziglang`).
   This produces `fw/firmware_b0..b3.hex`. A prebuilt copy is included.
3. **Compile in Quartus**: open `ecg_soc.qpf` from the project root, then Compile, then Programmer.
   Memory init files are read with relative paths. If you change the ECG data or the firmware, recompile
   (or use *Processing → Update Memory Initialization File*, then re-run the Assembler).
4. **Connect** a serial cable (or USB-RS232 adapter) to the board's DB9 port. Use 115200 baud, 8N1,
   **no flow control**.
   ```
   pip install pyserial matplotlib
   python tools/uart_receiver.py --port COM5
   ```
   Press **KEY0** to reset/rerun, or send `r`. LEDG0 lights when the hardware run is done, LEDG1 when HW == SW.
   LEDR shows the mean heart rate in binary.

## Example output (from full-system simulation)
```
#PERF
hw_cycles,12039          <- accelerator, including CPU start/poll
sw_cycles,6464696        <- same algorithm in C on PicoRV32
speedup_x,536
hw_sw_match,YES
#BEATS
beat,sample,time_ms,rr_ms,hr_bpm,flag
...
#SUMMARY
beats,69
mean_hr_bpm,72
rmssd_ms,33
rhythm,Normal sinus rhythm
```

## Memory map

| Address | What |
|---|---|
| `0x0000_0000` | 32 KB program/data RAM |
| `0x1000_0000` | 64 KB ECG RAM. Word 0 = N samples, words 1..N = samples (int16, sign-extended). Shared CPU/accelerator |
| `0x2000_0000` | UART: +0 TX (write) / RX (read). +4 status: bit0 tx_busy, bit1 rx_valid |
| `0x2000_0010` / `0x14` | LEDR / LEDG |
| `0x3000_0000` | Accelerator: CTRL(+0, bit0 start), STATUS(+4: busy, done, ovf), SRC(+8), LEN(+C), NPEAKS(+10), CYCLES(+14), SPKI(+18), NPKI(+1C) |
| `0x3000_1000` | PEAK[i] = sample index of beat i (max 512) |

## Accelerator pipeline (integer, bit-exact in Python / Verilog / C)

| Stage | Equation (200 Hz) | Purpose |
|---|---|---|
| Low-pass | `y = 2y[n-1] - y[n-2] + x - 2x[n-6] + x[n-12]` | ~11 Hz cut-off, removes EMG/mains noise |
| High-pass | `(32·u[n-16] - Σ32 u) >> 5` | ~5 Hz cut-off, removes baseline wander |
| Derivative | `(2w + w[n-1] - w[n-3] - 2w[n-4]) >> 3` | QRS slope |
| Square | `(d·d) >> 3` | rectify, emphasise high slopes |
| MWI | `Σ32 q >> 5` (160 ms window) | one smooth bump per QRS |
| Peak detect | adaptive SPKI/NPKI, threshold `NPKI + ¼(SPKI-NPKI)`, 200 ms refractory, **searchback** at 1.66 × RR average with threshold/2 | R-peak decisions |

The first 2 s are a learning phase, so beats in that window are not reported. The reported index is
corrected for the ~38-sample pipeline delay (measured error ±2 samples = ±10 ms).

## Verification
* `sh sim/run_sim.sh` (Icarus Verilog) runs two tests: an accelerator unit test against `data/expected_peaks.txt`, and a full-system run that prints the UART report.
* Tested bit-exact on normal (72 bpm), tachycardia (150 bpm) and two highly irregular synthetic records.
  On synthetic data the sensitivity is ≥98.6% for regular rhythms and PPV is ≈100%.
* Validate on MIT-BIH: compare `results/beats_*.csv` against the `.atr` annotations with `wfdb.processing.compare_annotations` (±150 ms window).

## Limitations and next steps
* The rhythm rules (brady/tachy/ectopic/irregular→possible AF) are simple heuristics, not diagnostic.
* Ideas to extend the accelerator: QRS width/morphology, a T-wave check on the derivative slope, or a small
  int8 1D-CNN beat classifier fed by the R-peak windows. You can also stream long records over UART into the ECG RAM (the CPU port is writable).
* Resource estimate (not yet run through Quartus): roughly 100 of the 432 M9K blocks, a few thousand registers, and a handful of DSP multipliers (CPU MUL + squarer). The chip is far from full.
