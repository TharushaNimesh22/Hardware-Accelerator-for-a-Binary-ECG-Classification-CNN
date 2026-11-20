
# Hardware Accelerator for Binary ECG Classification Using CNNs

## Project Overview

This project focuses on the design and implementation of a hardware-accelerated system for real-time binary classification of ECG signals using a lightweight 1-D Convolutional Neural Network (CNN).

The system integrates a **PicoRV32 RISC-V processor**, a dedicated **Pan-Tompkins hardware accelerator** for real-time heartbeat (QRS complex) detection, and a **custom-designed CNN hardware accelerator** for ECG beat classification. UART communication is used for data transfer and system interaction.

The primary objective is to develop an efficient hardware architecture for CNN inference, with particular emphasis on **minimizing memory accesses, optimizing computational efficiency, and improving overall inference performance**.

The proposed design is evaluated against a baseline implementation to analyze the performance benefits achieved through hardware acceleration and architectural optimizations.

## System Architecture

The system consists of the following major components:

- **PicoRV32 RISC-V Processor:** Serves as the main processor, coordinating system operations and managing communication between hardware components.
- **Pan-Tompkins Accelerator:** Performs real-time QRS complex detection to identify heartbeats from incoming ECG signals.
- **CNN Hardware Accelerator:** Executes the forward inference operations of the CNN model for binary ECG classification.
- **UART Interface:** Provides serial communication for ECG data transfer, system control, and result retrieval.

## CNN Model Architecture

A lightweight 1-D CNN is initially implemented for binary ECG classification. The model processes two input features:

1. **ECG Beat Shape:** A window containing 128 ECG samples, with the R-peak positioned at index 48.
2. **RR Interval Features:** Four timing-related features representing the temporal characteristics of the heartbeat.

The CNN consists of three convolutional blocks followed by two fully connected layers. The convolutional layers extract morphological features from the ECG waveform, while the RR features are concatenated with the flattened convolutional output before classification.

### Network Architecture

| Layer | Configuration | Output Shape |
|---|---|---|
| Input | ECG beat (128 samples) | 1 × 128 |
| Conv1 + ReLU | 1 → 8, Kernel = 7 | 8 × 128 |
| MaxPool | Pool size = 2 | 8 × 64 |
| Conv2 + ReLU | 8 → 16, Kernel = 5 | 16 × 64 |
| MaxPool | Pool size = 2 | 16 × 32 |
| Conv3 + ReLU | 16 → 32, Kernel = 5 | 32 × 32 |
| MaxPool | Pool size = 2 | 32 × 16 |
| Flatten | - | 512 |
| Concatenation | Flattened features + 4 RR features | 516 |
| FC1 + ReLU | 516 → 32 | 32 |
| FC2 | 32 → 2 | 2 |

**Model specifications:**

- Architecture: 1-D CNN
- Classification: Binary (2 classes)
- Total trainable parameters before BatchNorm folding: 19,978
- Exported weights: 19,832 INT8 weights
- Exported biases: 90 INT32 biases
- Dropout: 0.3 (training only)

Batch Normalization parameters are folded into the convolutional weights and biases during model export, simplifying the inference hardware implementation.

## Hardware Acceleration Objectives

The main focus of this project is the hardware implementation of the CNN inference engine rather than improving the classification accuracy of the neural network.

Key objectives include:

- **CNN Hardware Acceleration:** Develop a dedicated hardware architecture to accelerate the forward inference operations of the CNN.
- **Memory Access Optimization:** Minimize memory accesses through efficient data reuse, optimized data movement, and architectural techniques that reduce unnecessary memory transactions.
- **Computational Optimization:** Explore efficient hardware implementations of convolutional and fully connected operations.
- **Processor–Accelerator Integration:** Integrate the CNN accelerator with the PicoRV32 RISC-V processor for efficient system-level operation.
- **Real-Time ECG Processing:** Integrate heartbeat detection and CNN-based classification into a unified hardware processing system.
- **Performance Evaluation:** Compare the proposed hardware-accelerated implementation against a baseline design using relevant performance metrics.

## Performance Evaluation

The proposed architecture is evaluated against a baseline implementation to investigate the benefits of dedicated hardware acceleration.

The evaluation focuses on:

- Execution latency and inference time
- Number of clock cycles per inference
- Memory access frequency and data movement
- Hardware resource utilization
- Computational throughput
- Overall system performance

The results will be used to identify the effectiveness of different hardware architectural optimizations and their impact on CNN inference performance.

## Technologies and Tools

*Add the tools and technologies used in the implementation.*

- **Processor:** PicoRV32 (RISC-V)
- **Heartbeat Detection:** Pan-Tompkins Algorithm
- **Neural Network:** 1-D Convolutional Neural Network
- **Communication:** UART
- **Hardware Description Language:** SystemVerilog / Verilog
- **Development Tools:** Quartus Prime & ModelSim

## Project Scope

This project explores the design of an efficient hardware acceleration architecture for lightweight neural network inference in biomedical signal processing applications.

The central focus is on architectural optimization, efficient computation, reduced memory access, and processor–accelerator integration, with the ultimate goal of achieving efficient real-time ECG processing.
