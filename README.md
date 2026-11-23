# Image Transmission

**A TCP-Based Image Transmission System with Error Detection, Noise Simulation and Retransmission**

## Overview

ImageTransmission is a client-server application developed in C that transfers image files over a TCP connection using Windows Socket Programming (Winsock).

Unlike a basic file transfer application, this project simulates transmission errors by introducing artificial noise into image data. It uses CRC32 checksums to detect corrupted data and implements an automatic retransmission mechanism to ensure reliable image delivery.

The project also evaluates transmission quality using Bit Error Rate (BER) and Signal-to-Noise Ratio (SNR) measurements.

## Key Features

- **TCP Communication:** Reliable client-server communication using TCP sockets.
- **Chunk-Based Transmission:** Images are divided into 4096-byte chunks.
- **Noise Simulation:** Artificial transmission errors are introduced through probabilistic bit flipping.
- **CRC32 Error Detection:** Each chunk is validated using CRC32 checksums.
- **Automatic Retransmission:** Corrupted chunks are requested again, with up to 5 retries.
- **Application-Level Handshake:** HELLO/READY exchange before data transmission.
- **PNG Validation:** The server checks PNG signatures, chunk structures and CRC values.
- **BER Analysis:** Measures the percentage of corrupted bits during the first transmission attempt.
- **SNR Analysis:** Evaluates the relationship between signal and noise power.
- **Experiment Logging:** Stores transmission metrics and results in a CSV file.

## Technologies Used

| Technology | Purpose |
|---|---|
| C | Core programming language |
| TCP/IP | Reliable data transmission |
| Winsock2 | Windows socket programming |
| CRC32 | Data integrity verification |
| CSV | Experimental data storage |
| PNG | Image format and structural validation |

## System Architecture

The application consists of two main components:

### Client

- Establishes a TCP connection with the server.
- Performs an application-level handshake.
- Reads the original PNG image.
- Divides the image into 4096-byte chunks.
- Simulates noise and bit corruption.
- Calculates CRC32 checksums.
- Sends packets and handles retransmission requests.
- Calculates BER and SNR metrics.
- Records experimental results in `results.csv`.

### Server

- Listens for incoming TCP connections.
- Accepts the client connection.
- Performs the handshake.
- Receives image chunks.
- Calculates CRC32 checksums to detect corrupted packets.
- Sends OK or RETRY responses.
- Reassembles successfully received chunks.
- Validates the PNG file structure.
- Saves the reconstructed image.

## Transmission Protocol

Each data packet contains a 12-byte header:

| Field | Size | Description |
|---|---:|---|
| Sequence Number | 4 bytes | Identifies the chunk |
| Payload Size | 4 bytes | Specifies the data length |
| Checksum | 4 bytes | CRC32 integrity value |

The packet header is explicitly encoded using network byte order before transmission.

The server responds with a 12-byte message containing a status field and the corresponding sequence number.

### Transmission Workflow

1. Client connects to server.
2. Client sends `HELLO`.
3. Server responds with `READY`.
4. Client sends file metadata.
5. Image is divided into chunks.
6. Noise is optionally introduced into each chunk.
7. Server validates the received chunk using CRC32.
8. Server sends `OK` or `RETRY`.
9. Client retransmits corrupted chunks when necessary.
10. Server reconstructs and validates the PNG image.
11. Experimental results are saved to CSV.

## Noise Simulation

The project uses two levels of probabilistic corruption:

- **Chunk Corruption Probability:** Controlled by `NOISE_PERCENT`.
- **Bit Flip Probability:** Controlled by `INTRA_CHUNK_FLIP_PROB` (10%).

For example, with `NOISE_PERCENT = 20`, each chunk has a 20% probability of being corrupted. Within a corrupted chunk, each bit has a 10% probability of being flipped.

The maximum number of retransmissions is controlled by:

```c
#define MAX_RETRIES 5
```

If a chunk cannot be transmitted successfully after the initial attempt and five retries, the entire transfer is aborted.

## Performance Evaluation

The application calculates two important communication metrics.

**1. Bit Error Rate (BER)**

BER measures the percentage of corrupted bits during the first transmission attempt.

\[
BER = \frac{\text{Flipped Bits}}{\text{Total Bits}} \times 100
\]

**2. Signal-to-Noise Ratio (SNR)**

SNR measures the relationship between signal power and noise power.

\[
SNR(dB)=10\log_{10}\left(\frac{P_{signal}}{P_{noise}}\right)
\]

Higher SNR values indicate a stronger signal relative to noise.

## Experimental Results

The application records the following metrics for each experiment:

| Metric | Description |
|---|---|
| Noise Percent | Configured chunk corruption probability |
| Total Chunks | Number of image chunks |
| Chunks Retried | Chunks requiring at least one retry |
| Total Retransmissions | Total retransmission count |
| Actual BER | Measured bit error rate |
| SNR (dB) | Signal-to-noise ratio |
| Original Checksum | CRC32 of the original image |
| Transfer Status | SUCCESS or FAILED |
| Failed Chunk | Sequence number of the failed chunk, if any |

Experimental results are stored in `results.csv`.

Since noise is generated randomly, the number of retransmissions and the final transfer status may vary between experiments.

## Project Structure

```text
ImageTransmission/
│
├── client.c
├── server.c
├── input/
│   └── image.png
├── output/
│   └── received.png
├── results.csv
└── README.md
```

## Requirements

- Windows operating system
- GCC / MinGW compiler
- Winsock2 library

## Installation and Execution

**1. Clone the repository**

```bash
git clone https://github.com/selin17/ImageTransmission.git
cd ImageTransmission
```

**2. Compile the server**

```bash
gcc server.c -o server.exe -lws2_32
```

**3. Compile the client**

```bash
gcc client.c -o client.exe -lws2_32 -lm
```

**4. Run the application**

Open two separate terminals.

Terminal 1 – Start the server:

```bash
server.exe
```

Terminal 2 – Start the client:

```bash
client.exe
```

The received image will be saved as:

```text
output/received.png
```

Experimental metrics will be appended to:

```text
results.csv
```

## Learning Objectives

This project demonstrates practical concepts in:

- Computer Networks
- TCP Socket Programming
- Client-Server Architecture
- Data Integrity and Error Detection
- Retransmission Mechanisms
- Network Performance Evaluation
- Binary File Transfer
- Experimental Data Analysis

---
