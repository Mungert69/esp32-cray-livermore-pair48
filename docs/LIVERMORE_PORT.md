# ESP32-S3 Livermore Loops 1-14

## Canonical implementation

The benchmark implementation lives in the ESP-IDF component at
`components/livermore/`:

- `livermore_esp32.c` contains arithmetic modes, timing, checksum validation,
  and the Pair48-versus-Accurate48 element audit.
- `livermore_kernels_1_14.inc` contains the parameterized kernel bodies,
  initialization, LFK repeat-loop behavior, and checksum slices.
- `CMakeLists.txt` registers the component.

`main/livermore_main.c` is the app entry point. `main/CMakeLists.txt` selects
the Livermore component when `CONFIG_BENCHMARK_TYPE_LIVERMORE` is enabled.
There are no separate `main/livermore_*.c` copies; use the component files as
the single source of truth.

## Validation status

The firmware includes the Netlib `SIGNEL` setup, Kernel 13 P initialization,
Kernel 14 IQRANF stream, correct LFK inner-repeat state semantics, and original
`SUMO` checksum slices. The seed-256 IQRANF test vector passes. Accurate48
agrees with the published checksums for all 14 kernels across all three spans.
K12 has the largest relative residual (1.473e-9) because its small checksum is
cancellation-sensitive. The program still runs one timing trial, not the
standard seven-run driver, so performance remains provisional.

After the performance table, the program compares Pair48 and Accurate48
element by element over each kernel's checksum-contributing slices. It reports
checksum-relative error, differing bit representations, differing numerical
values, maximum absolute and relative element error, worst flattened slice
index, and the high/low word deltas there. Identical aggregate checksums do not
prove identical element results.

See `BENCHMARK_FINDINGS.md` for the benchmark summary and historical Cray
references, and `main/asm/PAIR48_DEV_LOG.md` for chronological change
rationale and detailed board validation results.

## Build and run

Select Livermore in the project Kconfig, then run the usual ESP-IDF build and
flash commands. The current integration uses `components/livermore`; do not
copy another Livermore implementation into `main/`.
