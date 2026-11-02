# RFM69HCW shared-SPI hello-world demo

One MSPM0G3507 LaunchPad controls two RFM69HCW radios on hardware SPI0.
The RFM69HCW supports FSK/OOK, not LoRa; this demo uses FSK packet mode.

## Wiring

| LaunchPad MCU pin | Connection |
| --- | --- |
| PA12 | SPI0 SCLK to SCK on both radios |
| PB17 | SPI0 MOSI/PICO to MOSI on both radios |
| PA13 | SPI0 MISO/POCI from MISO on both radios |
| PB0 | GPIO active-low NSS/CS on transmitter only |
| PB1 | GPIO active-low NSS/CS on receiver only |
| PA0 | Onboard red LED1: toggles with each `alive` heartbeat |
| GND | RST on both Adafruit #3070 breakouts; hold low to release reset |
| Not connected | EN on both Adafruit #3070 breakouts; internally pulled high |
| PA10 | UART0 TX to onboard XDS110 USB serial bridge |
| PA11 | UART0 RX from onboard XDS110 USB serial bridge |
| 3V3 | VIN on both Adafruit #3070 breakouts |
| GND | Common ground to both radios |

DIO0-DIO5 are not needed: the firmware polls radio IRQ registers over SPI.
The confirmed boards are Adafruit RFM69HCW breakouts, product #3070. Their
RST input is pulled HIGH by default, which holds the radio in reset. Because
this firmware does not drive RST, connect each breakout's RST to GND. EN can
remain unconnected: it is pulled high to VIN and enables the regulator.
This differs from leaving RESET floating on the bare RFM69HCW module.
Power off before changing the wiring, then power-cycle both boards and run
the existing v3 firmware again. No new firmware is needed for this wiring fix.
Reference: [Adafruit breakout pinouts](https://learn.adafruit.com/adafruit-rfm69hcw-and-rfm96-rfm95-rfm98-lora-packet-padio-breakouts/pinouts).

Use the MCU signal labels above, not package pin numbers. PB0 and PB1 avoid the LaunchPad crystal pins.
Keep J4 installed to connect LED1 and J19 in the 3.3 V pull-up position (1-2). LED1 starts off and toggles once per heartbeat (one second on, one second off).
Fit antennas appropriate to the modules and use 3.3 V power/logic with local
supply decoupling. TX output is set to -2 dBm for a nearby bench receiver.

Keep J21 and J22 in their XDS_UART positions (1-2), and the J101 UART isolation
jumpers installed. Connect the LaunchPad XDS110 USB port and open **XDS110 Class
Application/User UART** at **115200 baud, 8 data bits, no parity, 1 stop bit,
no flow control**. No separate USB-UART adapter is required.
Board wiring reference: [TI LaunchPad user's guide](https://www.ti.com/lit/ug/slau873c/slau873c.pdf).

## Behavior and configuration

- `blink_led.syscfg` generates SPI0 at 100 kHz, mode 0, 8-bit MSB first, and UART0.
  The SPI MOTO3 setting means hardware CS is disabled; MOSI and MISO remain
  separate. GPIO CS remains low across both address and data bytes.
- `main.c` initializes SysConfig and starts `main_radio()` in `main-blinky.c`.
  The old filename is retained so existing CCS build settings continue to work.
- Both chip selects are manually initialized high before their outputs are enabled.
  PA0, PB0 and PB1 are reserved by the application; do not assign them in
  SysConfig or enable unused-pin termination.
- The higher-priority TX task configures both radios, starts RX, then
  repeatedly transmits exactly `HELLO WORLD` (no NUL or newline). Each character
  is a separate one-byte packet with whitening, two-byte sync and hardware CRC.
- After PacketSent, TX enters standby and delays at least 50 ms (plus up to
  roughly one scheduler tick). Packet airtime is additional: this is a 50 ms gap,
  not a 50 ms start-to-start period. TX mode is re-entered for every packet.
- TX runs at priority 2, RX at priority 1. A priority-inheriting mutex protects
  each SPI transaction. RF waits release the bus and yield; RX polls every 1 ms.
- RX prints actual CRC-validated FIFO bytes, never a local copy of the message.
  The terminal shows a startup banner followed by `HELLO WORLDHELLO WORLD...`.
  The RX task also prints `alive` on its own line every second after initialization, even when no packets arrive. RX automatically rearms after its FIFO is emptied; FIFO overruns are flushed.
- Default frequency: **915 MHz**. Set `RADIO_FREQUENCY_HZ` in `main-blinky.c`
  (or a compiler definition) to match the actual 433/868/915 MHz module variant
  and the frequency permitted for your setup. Both radios use the same setting.
  Bitrate is approximately 4.8 kbit/s, deviation 5 kHz, receive bandwidth 50 kHz.
- RST is neither driven nor sampled. Startup waits 20 ms for power-on reset, then probes both radios before mode changes. Power-cycle both radios when a fresh hardware reset is needed; resetting only the LaunchPad does not reset them. It writes/reads 0x55 and 0xAA in adjacent sync registers (0x2F/0x30), then restores them. Any failed test stops initialization. The 100 kHz SPI rate is a conservative diagnostic setting. ModeReady errors include the requested mode, actual OpMode, Version, IRQ1 and IRQ2. Startup also checks RegVersion and packet-register readback. SPI, ModeReady and
  PacketSent waits are bounded. A failure prints an error and halts; fix the
  wiring/power and reset the LaunchPad. Debug counters are `radioTxPackets`,
  `radioRxPackets`, and `radioRxOverruns`.

Radio register source: `../../docs/RFM69HCW-V1.1.pdf`, sections 5 (SPI/packet
handling), 6 (register map), and 7.2.1 (power-on reset).

## Build and hardware check

Import this CCS project together with
`../freertos_builds_LP_MSPM0G3507_release_ticlang`; build the FreeRTOS library
first, then this application. The existing CCS target name is `blink_led.out`.
The project uses MSPM0 SDK 2.11.00.07 and SysConfig 1.26.2.

Local command using the already-generated Debug makefiles:

```powershell
& C:/ti/ccs2100/ccs/utils/bin/gmake.exe -C firmware/msp_transmitter/Debug -j 2 all
```

Load `Debug/blink_led.out` and run. Confirm the banner and repeating received
text. Disconnect or power down the receiving radio after startup: received
radio text must stop while `radioTxPackets` continues increasing; the `alive` heartbeat continues. A logic analyzer
should show only one NSS low at a time and two 8-bit SPI frames per register
access. Check the RF packet gaps and UART output on real hardware.

Validation performed: SysConfig generation plus TI Arm Clang compilation and
link. Radio communication, USB serial output and timing have not been tested
on physical hardware.





