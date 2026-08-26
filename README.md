# minibitx — an experimental test bed

PROJECT STATUS: Still working on receive mode. Transmit mode will come next.

Use sbitx hardware with external SDR software like Quisk or SDR Console.
minibitx runs on the Raspberry Pi board inside your sbitx as an
alternative to the `sbitx` software that came with the radio. It has no
dependency on the sbitx software and expects to run stand-alone.

minibitx controls the sbitx hardware via HAMLIB/rigctl commands and
outputs baseband I/Q data via a subset of HPSDR Protocol 1 and/or USB
audio. It provides no user interface of its own beyond a one-line console
status display — the external SDR application supplies all of the
receive/transmit processing: FFT display, demodulation, filtering, and
audio routing. minibitx started as a trim-down of the much larger sbitx
codebase, keeping only what's needed for an external SDR app to drive the
hardware.

## Building

```
make
```

Produces a single `minibitx` binary from the sources in `src/`. Requires
`libasound`, `libwiringPi`, and the usual `pthread`/`libm`/`libdl` (see
`Makefile` for the exact link line).

## Running

```
./minibitx
```

Brings up the radio hardware, starts the audio and network threads, and
listens for control connections — a rigctld-compatible server on TCP
4532, and an HPSDR Protocol 1 UDP listener. Point your SDR app's HPSDR
client at this Pi's IP, and (optionally) its CAT/rig control at
`127.0.0.1:4532` with rig model "Hamlib NET rigctl" for live retuning.

## Updating

`./update` stashes any local changes, pulls the latest commits, and
reports success — run it from the same directory as this README.

## Documentation

The `docs/` folder has the detailed breakdown of how minibitx works,
organized roughly from the hardware up:

| Doc | Coontent |
|---|---|
| [`docs/00_intro.md`](docs/00_intro.md) | Project scope, status, and a map of the rest of the docs |
| [`docs/01_hardware_init_and_control.md`](docs/01_hardware_init_and_control.md) | GPIO, si5351/I2C, WM8731 codec bring-up |
| [`docs/02_rx_processing_pipeline.md`](docs/02_rx_processing_pipeline.md) | Antenna to baseband I/Q, stage by stage |
| [`docs/03_tx_processing_pipeline.md`](docs/03_tx_processing_pipeline.md) | What TX support exists today and what's still missing |
| [`docs/04_remote_control_and_iq_output.md`](docs/04_remote_control_and_iq_output.md) | rigctld, HPSDR control/IQ, USB Audio Class output |
| [`docs/05_process_and_threading_model.md`](docs/05_process_and_threading_model.md) | Startup sequence and thread structure |
| [`docs/dsp_design_notes/`](docs/dsp_design_notes/) | Proposed-but-not-yet-implemented DSP work (e.g. the anti-alias FIR) |
| [`docs/07_build_and_deployment.md`](docs/07_build_and_deployment.md) | Build, kernel/overlay dependencies, deployment notes |
| [`docs/08_troubleshooting_and_bringup.md`](docs/08_troubleshooting_and_bringup.md) | Hardware bring-up gotchas |
| [`docs/10_external_digital_modes_wsjtx.md`](docs/10_external_digital_modes_wsjtx.md) | Using minibitx with WSJT-X and similar digital-mode apps |
| [`docs/11_general_coverage_sdr_receiver.md`](docs/11_general_coverage_sdr_receiver.md) | Using minibitx as a general-coverage SDR receiver |
| [`docs/12_simple_cw_transceiver.md`](docs/12_simple_cw_transceiver.md) | Building a simple CW transceiver around minibitx |

## Credits

- Inspired by Ashhar Farhan's (VU2ESE) original sbitx code
- Code was based on JJ's 64-bit repository at https://github.com/drexjj/sbitx
