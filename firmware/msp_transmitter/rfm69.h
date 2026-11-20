/* Shared RFM69HCW driver interface (implemented in main-blinky.c).
 * Register definitions: docs/RFM69HCW-V1.1.pdf, sections 5, 6 and 7.2.2.
 */
#ifndef RFM69_H
#define RFM69_H

#include <stdbool.h>
#include <stdint.h>
#include "ti_msp_dl_config.h"

#define TX_CS DL_GPIO_PIN_0 // PB0: NSS of the transmitting radio
#define RX_CS DL_GPIO_PIN_1 // PB1: NSS of the receiving radio
#define HEARTBEAT_LED DL_GPIO_PIN_0 // PA0: active-low onboard red LED1

#define REG_FIFO 0x00
#define REG_OPMODE 0x01
#define REG_VERSION 0x10
#define REG_IRQ1 0x27
#define REG_IRQ2 0x28
#define REG_PAYLOAD_LENGTH 0x38
#define REG_PACKET2 0x3D

#define MODE_STANDBY 0x04
#define MODE_TX 0x0C
#define MODE_RX 0x10

#define MODE_READY 0x80   // RegIrqFlags1
#define FIFO_OVERRUN 0x10 // RegIrqFlags2
#define PACKET_SENT 0x08
#define PAYLOAD_READY 0x04
#define CRC_OK 0x02

/* UART helpers */
bool uartByte(uint8_t value);
void uartText(const char *text);
void uartHex(uint8_t value);
void fatal(const char *message); // never returns

/* Board and radio helpers */
void radioBoardInit(void);       // LED, chip selects and 1 ms SysTick
uint8_t spiByte(uint8_t value);  // one full-duplex SPI byte
uint8_t readReg(uint32_t cs, uint8_t address);
void writeReg(uint32_t cs, uint8_t address, uint8_t value);
bool waitFlagFor(uint32_t cs, uint8_t reg, uint8_t mask, uint32_t timeoutMs);
void setMode(uint32_t cs, uint8_t mode);
void configureRadio(uint32_t cs, bool verbose);

/* Demos: call one from main() */
void main_radio(void);    // one-character packets, "HELLO" loop
void main_packet32(void); // single fixed-length 32-byte packet test

#endif
