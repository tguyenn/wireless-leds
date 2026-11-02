/* RFM69HCW two-radio demo. Kept in main-blinky.c for the existing CCS build.
 * Register definitions: docs/RFM69HCW-V1.1.pdf, sections 5, 6 and 7.2.2.
 * This is FSK packet mode, not LoRa. One ASCII byte per fixed-length packet.
 */
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "ti_msp_dl_config.h"

#ifndef RADIO_FREQUENCY_HZ
#define RADIO_FREQUENCY_HZ 915000000UL
#endif
#define TX_CS DL_GPIO_PIN_0
#define RX_CS DL_GPIO_PIN_1
#define HEARTBEAT_LED DL_GPIO_PIN_0 // PA0: active-low onboard red LED1
#define REG_FIFO 0x00
#define REG_OPMODE 0x01
#define REG_VERSION 0x10
#define REG_IRQ1 0x27
#define REG_IRQ2 0x28
#define REG_PACKET2 0x3D
#define MODE_STANDBY 0x04
#define MODE_TX 0x0C
#define MODE_RX 0x10
#define MODE_READY 0x80
#define PACKET_SENT 0x08
#define PAYLOAD_READY 0x04
#define CRC_OK 0x02
#define FIFO_OVERRUN 0x10

static SemaphoreHandle_t spiMutex;
static TaskHandle_t rxTaskHandle;
/* Inspect these counters in CCS; UART reports received data and a heartbeat. */
volatile uint32_t radioTxPackets;
volatile uint32_t radioRxPackets;
volatile uint32_t radioRxOverruns;

static void uartText(const char *text)
{
    while (*text) DL_UART_Main_transmitDataBlocking(UART_0_INST, (uint8_t)*text++);
}

static void uartHex(uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    DL_UART_Main_transmitDataBlocking(UART_0_INST, hex[value >> 4]);
    DL_UART_Main_transmitDataBlocking(UART_0_INST, hex[value & 0x0F]);
}

static void fatal(const char *message)
{
    taskDISABLE_INTERRUPTS();
    DL_GPIO_setPins(GPIOB, TX_CS | RX_CS);
    uartText(message);
    for (;;) {}
}

/* Every transmitted SPI byte also receives a byte: always drain RX FIFO. */
static uint8_t spiByte(uint8_t value)
{
    uint32_t budget = 100000;
    DL_SPI_transmitData8(SPI_0_INST, value);
    while (DL_SPI_isRXFIFOEmpty(SPI_0_INST)) {
        if (--budget == 0) fatal("\r\nERROR: SPI0 timeout\r\n");
    }
    return DL_SPI_receiveData8(SPI_0_INST);
}

static uint8_t radioRegister(uint32_t cs, uint8_t address, uint8_t value)
{
    uint32_t budget = 100000;
    uint8_t result;
    /* Priority inheritance bounds the higher-priority TX task's bus wait.
     * Never hold this mutex while waiting for RF events or writing UART. */
    if (xSemaphoreTake(spiMutex, portMAX_DELAY) != pdTRUE)
        fatal("\r\nERROR: SPI mutex\r\n");
    DL_GPIO_setPins(GPIOB, TX_CS | RX_CS);
    delay_cycles(CPUCLK_FREQ / 1000000); // >= 1 us NSS high/setup/hold
    DL_GPIO_clearPins(GPIOB, cs);
    delay_cycles(CPUCLK_FREQ / 1000000);
    (void)spiByte(address);
    result = spiByte(value);
    while (DL_SPI_isBusy(SPI_0_INST)) {
        if (--budget == 0) fatal("\r\nERROR: SPI0 busy\r\n");
    }
    delay_cycles(CPUCLK_FREQ / 1000000);
    DL_GPIO_setPins(GPIOB, cs);
    xSemaphoreGive(spiMutex);
    return result;
}

static uint8_t readReg(uint32_t cs, uint8_t address)
{
    return radioRegister(cs, address & 0x7F, 0);
}

static void writeReg(uint32_t cs, uint8_t address, uint8_t value)
{
    (void)radioRegister(cs, address | 0x80, value);
}

static void printRegister(uint32_t cs, uint8_t address)
{
    uint8_t value = readReg(cs, address);
    uartText(" reg[0x"); uartHex(address);
    uartText("]=0x"); uartHex(value);
}

/* Exercise both data polarities and adjacent odd/even addresses, without
 * requiring the RF oscillator to start. Restore the original sync values. */
static bool probeRadio(uint32_t cs)
{
    const uint8_t addresses[] = {0x2F, 0x30};
    const uint8_t patterns[] = {0x55, 0xAA};
    uint8_t saved[2];
    bool passed = true;
    uartText(cs == TX_CS ? "TX probe:" : "RX probe:");
    printRegister(cs, REG_VERSION);
    printRegister(cs, REG_OPMODE);
    printRegister(cs, 0x03); // bitrate MSB, reset 0x1A
    printRegister(cs, 0x04); // bitrate LSB, reset 0x0B
    printRegister(cs, REG_IRQ1);
    uartText("\r\n");
    if (readReg(cs, REG_VERSION) != 0x24) passed = false;
    for (unsigned i = 0; i < 2; ++i) saved[i] = readReg(cs, addresses[i]);
    for (unsigned p = 0; p < 2; ++p) {
        // Write distinct values before reading either register to expose aliases.
        for (unsigned i = 0; i < 2; ++i)
            writeReg(cs, addresses[i], patterns[p ^ i]);
        for (unsigned i = 0; i < 2; ++i) {
            uint8_t actual = readReg(cs, addresses[i]);
            uartText(cs == TX_CS ? "TX SPI test" : "RX SPI test");
            uartText(" reg=0x"); uartHex(addresses[i]);
            uartText(" wrote=0x"); uartHex(patterns[p ^ i]);
            uartText(" read=0x"); uartHex(actual);
            uartText(actual == patterns[p ^ i] ? " PASS\r\n" : " FAIL\r\n");
            if (actual != patterns[p ^ i]) passed = false;
        }
    }
    for (unsigned i = 0; i < 2; ++i) writeReg(cs, addresses[i], saved[i]);
    return passed;
}

static bool waitFlag(uint32_t cs, uint8_t reg, uint8_t mask)
{
    TickType_t start = xTaskGetTickCount();
    do {
        if ((readReg(cs, reg) & mask) == mask) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((TickType_t)(xTaskGetTickCount() - start) < pdMS_TO_TICKS(100));
    return false;
}

static void setMode(uint32_t cs, uint8_t mode)
{
    writeReg(cs, REG_OPMODE, mode); // sequencer on, Listen off
    if (!waitFlag(cs, REG_IRQ1, MODE_READY)) {
        /* Take all SPI snapshots before disabling interrupts: SPI uses an
         * RTOS mutex. Freeze task output only after releasing the bus. */
        uint8_t version = readReg(cs, REG_VERSION);
        uint8_t actualMode = readReg(cs, REG_OPMODE);
        uint8_t irq1 = readReg(cs, REG_IRQ1);
        uint8_t irq2 = readReg(cs, REG_IRQ2);
        taskDISABLE_INTERRUPTS();
        uartText(cs == TX_CS ? "\r\nERROR: TX ModeReady" :
                               "\r\nERROR: RX ModeReady");
        uartText(" requested=0x"); uartHex(mode);
        uartText(" OpMode=0x"); uartHex(actualMode);
        uartText(" Version=0x"); uartHex(version);
        uartText(" IRQ1=0x"); uartHex(irq1);
        uartText(" IRQ2=0x"); uartHex(irq2);
        fatal("\r\nRadio halted; record this line before resetting.\r\n");
    }
}

static void configureRadio(uint32_t cs)
{
    static const struct { uint8_t reg, value; } settings[] = {
        {0x02, 0x00}, // packet mode, FSK, no shaping
        {0x03, 0x1A}, {0x04, 0x0B}, // 32 MHz / 6667 = 4799.76 bit/s
        {0x05, 0x00}, {0x06, 0x52}, // deviation = 82 * 61.035 Hz = 5 kHz
        {0x0B, 0x00}, // normal modulation-index AFC
        {0x11, 0x50}, // HCW PA1 on PA_BOOST, -18 + 16 = -2 dBm
        {0x12, 0x09}, // 40 us PA ramp
        {0x13, 0x1A}, // OCP enabled, 95 mA; no +20 dBm boost
        {0x18, 0x88}, // 50 ohm input, automatic LNA gain
        {0x19, 0x4B}, // RxBw = 50 kHz, DCC = 010
        {0x1A, 0x42}, // AFC bandwidth = 125 kHz
        {0x1E, 0x0C}, // AFC auto-on and auto-clear
        {0x25, 0x00}, {0x26, 0x07}, // unused DIO, CLKOUT off
        {0x29, 0xE4}, // RSSI threshold -114 dBm
        {0x2A, 0x00}, {0x2B, 0x00}, // no RX timeout
        {0x2C, 0x00}, {0x2D, 0x03}, // three preamble bytes
        {0x2E, 0x88}, // sync on, two bytes, no bit errors allowed
        {0x2F, 0x2D}, {0x30, 0xD4}, // private demo sync word
        {0x37, 0x50}, // fixed length, whitening, CRC, no address filtering
        {0x38, 0x01}, // one-byte payload (no length/address byte)
        {0x3B, 0x00}, // automatic mode transitions off
        {0x3C, 0x80}, // TX starts with one byte in FIFO
        {0x3D, 0x02}, // automatic RX restart, AES off
        {0x5A, 0x55}, {0x5C, 0x70}, // normal PA test settings
        {0x6F, 0x30}, // improved DAGC, AfcLowBetaOn = 0
    };
    uint32_t frf = (uint32_t)(((uint64_t)RADIO_FREQUENCY_HZ << 19) / 32000000UL);
    uint8_t version = readReg(cs, REG_VERSION);
    uartText(cs == TX_CS ? "TX RegVersion=0x" : "RX RegVersion=0x");
    uartHex(version);
    uartText(" (expected 0x24)\r\n");
    if (version != 0x24)
        fatal(cs == TX_CS ? "\r\nERROR: TX RegVersion (check wiring/power)\r\n" :
                            "\r\nERROR: RX RegVersion (check wiring/power)\r\n");
    setMode(cs, MODE_STANDBY);
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i)
        writeReg(cs, settings[i].reg, settings[i].value);
    writeReg(cs, 0x07, (uint8_t)(frf >> 16));
    writeReg(cs, 0x08, (uint8_t)(frf >> 8));
    writeReg(cs, 0x09, (uint8_t)frf);
    writeReg(cs, REG_IRQ2, FIFO_OVERRUN); // discard any stale FIFO contents
    if (readReg(cs, 0x38) != 1 || readReg(cs, 0x37) != 0x50)
        fatal("\r\nERROR: radio register readback\r\n");
}

static void transmitter(void *argument)
{
    static const char message[] = "HELLO WORLD";
    unsigned index = 0;
    (void)argument;
    /* RST is not controlled by the MCU. On Adafruit #3070, tie breakout RST
     * to GND: its pull-up otherwise holds the radio in reset. Allow power-on
     * reset to settle; power-cycle the radios for a fresh hardware reset. */
    vTaskDelay(pdMS_TO_TICKS(20));
    // Probe both even if TX fails, so one log compares both hardware paths.
    bool txBusOK = probeRadio(TX_CS);
    bool rxBusOK = probeRadio(RX_CS);
    if (!txBusOK || !rxBusOK)
        fatal("ERROR: SPI register test failed; radio initialization stopped\r\n");
    configureRadio(TX_CS);
    configureRadio(RX_CS);
    setMode(RX_CS, MODE_RX);
    setMode(TX_CS, MODE_TX);
    uartText("RFM69HCW ready; received characters follow:\r\n");
    xTaskNotifyGive(rxTaskHandle);
    for (;;) {
        // Standby clears PacketSent and prevents starting on a partial FIFO.
        setMode(TX_CS, MODE_STANDBY);
        writeReg(TX_CS, REG_FIFO, (uint8_t)message[index]);
        setMode(TX_CS, MODE_TX);
        if (!waitFlag(TX_CS, REG_IRQ2, PACKET_SENT))
            fatal("\r\nERROR: TX PacketSent timeout\r\n");
        ++radioTxPackets;
        setMode(TX_CS, MODE_STANDBY);
        index = (index + 1) % (sizeof(message) - 1);
        // At least 50 ms of idle time after each completed character packet.
        vTaskDelay(pdMS_TO_TICKS(50) + 1);
    }
}

static void receiver(void *argument)
{
    TickType_t lastHeartbeat;
    const TickType_t heartbeatPeriod = pdMS_TO_TICKS(1000);
    (void)argument;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    lastHeartbeat = xTaskGetTickCount();
    for (;;) {
        uint8_t flags = readReg(RX_CS, REG_IRQ2);
        if (flags & FIFO_OVERRUN) {
            ++radioRxOverruns;
            writeReg(RX_CS, REG_IRQ2, FIFO_OVERRUN);
            writeReg(RX_CS, REG_PACKET2, 0x06); // restart RX, keep auto-restart
        } else if ((flags & (PAYLOAD_READY | CRC_OK)) == (PAYLOAD_READY | CRC_OK)) {
            uint8_t character = readReg(RX_CS, REG_FIFO);
            ++radioRxPackets;
            DL_UART_Main_transmitDataBlocking(UART_0_INST, character);
        }
        /* This task owns normal UART output, so heartbeat bytes cannot
         * interleave with received bytes. TX retains its higher priority. */
        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - lastHeartbeat) >= heartbeatPeriod) {
            lastHeartbeat += heartbeatPeriod;
            uartText("\r\nalive\r\n");
            DL_GPIO_togglePins(GPIOA, HEARTBEAT_LED);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void main_radio(void)
{
    uartText("\r\nRFM69 demo: SPI diagnostics v3; MOSI=PB17; SPI=100kHz; RST unused\r\n");
    // PA0 is open-drain; initialize LED1 off before enabling its output.
    DL_GPIO_setPins(GPIOA, HEARTBEAT_LED);
    DL_GPIO_initDigitalOutput(IOMUX_PINCM1);
    DL_GPIO_enableOutput(GPIOA, HEARTBEAT_LED);
    // Manual active-low GPIO chip selects. Preload high before enabling output.
    DL_GPIO_setPins(GPIOB, TX_CS | RX_CS);
    DL_GPIO_initDigitalOutput(IOMUX_PINCM12); // PB0
    DL_GPIO_initDigitalOutput(IOMUX_PINCM13); // PB1
    DL_GPIO_enableOutput(GPIOB, TX_CS | RX_CS);
    spiMutex = xSemaphoreCreateMutex();
    if (!spiMutex) fatal("ERROR: SPI mutex allocation\r\n");
    if (xTaskCreate(receiver, "RadioRX", configMINIMAL_STACK_SIZE, NULL,
                    tskIDLE_PRIORITY + 1, &rxTaskHandle) != pdPASS ||
        xTaskCreate(transmitter, "RadioTX", configMINIMAL_STACK_SIZE, NULL,
                    tskIDLE_PRIORITY + 2, NULL) != pdPASS)
        fatal("ERROR: task allocation\r\n");
    vTaskStartScheduler();
    fatal("ERROR: scheduler allocation\r\n");
}

