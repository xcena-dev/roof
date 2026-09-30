# Does a 64-byte store reach uncached CXL memory as one transaction?

2026-09-23, TURIN-CRB.

## Question

`MOVDIR64B` is the one store the ISA defines as a single 64-byte write.<br>
An AVX-512 `vmovdqa64` store carries no such promise.<br>
A design that publishes a 64-byte record with one store to an uncached mapping rests on that store reaching memory whole.<br>
The question is whether a zmm store does so on this platform, or leaves the core as several partial writes.

## Environment

| | |
|---|---|
| Board, BIOS | AMD ONYX reference board, AMI ROXT1006C (2025-06-24) |
| CPU | AMD engineering sample 100-000001249-10, family 26 model 1 (Zen 5), 16 cores, 32 threads, one socket, `avx512f` and `movdir64b` present, governor `performance`, SMT on |
| Kernel, OS | Linux 6.18.7-061807-generic, Ubuntu 24.04.3 LTS, g++ 13.3.0 |
| CXL device | XCENA `20a6:0100` rev 01, firmware 1.0.8, link 32 GT/s x8, no CPMU block |
| CXL region | `region0`, ram, 1-way, 107 GiB, `/dev/dax0.0` devdax |
| Mappings | one 2 MiB uncached mapping and one write-back mapping of the device, both from user space |
| CPUs | writer CPU 2, reader CPU 6 |
| Counters | host data fabric (`amd_df`, raw event codes, no published event list) |

## Method

- **Timing.** 200000 stores per kind over 64 lines round-robin, wall time per line.
- **Tearing.** The writer stores one sequence number into all eight qwords of a line and repeats.<br>
  The reader loads the line and counts a read whose qwords differ as torn.<br>
  2 seconds per pair.
- **Fabric requests.** Every `amd_df` raw event code is swept.<br>
  Per code, the counter delta is taken over 20000 lines of each store kind and each load kind.<br>
  A code that counts 1 per 64-byte store, 8 per scalar line and 0 at idle is a request counter.
- **Store kinds.** `8x movq`, `vmovdqa64`, `movdir64b`.
- **Load kinds.** `vmovdqa64`, `8x movq`.

## Results

Timing, ns per line.

| Memory | `8x movq` | `vmovdqa64` | `movdir64b` |
|---|---:|---:|---:|
| uncached | 1333.3 | 114.4 | 6.4 |
| write-back | 3.1 | 0.6 | 6.4 |

Tearing on the uncached mapping, 2 seconds, zmm reader.

| Writer | Reads | Torn |
|---|---:|---:|
| `8x movq` | 4768000 | 4151433 |
| `vmovdqa64` | 4767000 | 0 |
| `movdir64b` | 4768000 | 0 |

Data-fabric counters, delta per 20000 lines to the uncached mapping.

| Raw config | Idle | st `8x movq` | st `vmovdqa64` | st `movdir64b` | ld `vmovdqa64` | ld `8x movq` | Reads as |
|---|---:|---:|---:|---:|---:|---:|---|
| `0x30000ff03` | 130 | 161679 | 20145 | 20009 | 20288 | 162388 | every request |
| `0x30000ff01` | 65 | 160838 | 20072 | 20004 | 145 | 1193 | write requests |
| `0xe0000ffcb` | 132 | 1714 | 154 | 10 | 40300 | 322430 | read requests, two per read |
| `0x30000ff07` | 132 | 1688 | 20149 | 20009 | 20296 | 162420 | full-line requests |
| `0x30000ff15` | 0 | 0 | 0 | 155780 | 0 | 0 | posted writes |

## Conclusion

- A zmm store to uncached CXL memory leaves this host's fabric as one full-line write request, the same as `movdir64b`.<br>
  Eight scalar stores leave as eight partial writes.
- A zmm load leaves as one read request, and eight scalar loads as eight.
- A zmm reader saw no torn line from a zmm or `movdir64b` writer, and 87 % torn from a scalar writer.

## Limits

- The observers are another core and the data fabric of the same host, not the device.
- Event meanings are inferred from the counts.
- One run per cell, one Zen 5 sample.
