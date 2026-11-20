/* RFM69HCW two-radio demo. Kept in main-blinky.c for the existing CCS build.
 * Register definitions: docs/RFM69HCW-V1.1.pdf, sections 5, 6 and 7.2.2.
 * This is FSK packet mode, not LoRa. One ASCII byte per fixed-length packet.
 */
#include "rfm69.h"

#ifndef RADIO_FREQUENCY_HZ
#define RADIO_FREQUENCY_HZ 915000000UL
#endif

static volatile uint32_t elapsedMs;
/* Inspect these counters in CCS; UART reports received data and a heartbeat. */
volatile uint32_t radioTxPackets;
volatile uint32_t radioRxPackets;
volatile uint32_t radioRxOverruns;

void SysTick_Handler(void)
{
    ++elapsedMs;
}

bool uartByte(uint8_t value)
{
    /* A wedged UART must not prevent radio startup or packet reception. */
    uint32_t budget = 100000;
    while (DL_UART_Main_isTXFIFOFull(UART_0_INST)) {
        if (--budget == 0) return false;
    }
    DL_UART_Main_transmitData(UART_0_INST, value);
    return true;
}

void uartText(const char *text)
{
    while (*text) {
        if (!uartByte((uint8_t)*text++)) return;
    }
}

void uartHex(uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char text[] = {hex[value >> 4], hex[value & 0x0F], '\0'};
    uartText(text);
}

void fatal(const char *message)
{
    DL_GPIO_setPins(GPIOB, TX_CS | RX_CS);
    for (;;) {
        uartText(message);
        DL_GPIO_clearPins(GPIOA, HEARTBEAT_LED);
        delay_cycles(CPUCLK_FREQ / 8);
        DL_GPIO_setPins(GPIOA, HEARTBEAT_LED);
        delay_cycles(7 * (CPUCLK_FREQ / 8));
    }
}

/* Every transmitted SPI byte also receives a byte: always drain RX FIFO. */
uint8_t spiByte(uint8_t value)
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
    return result;
}

uint8_t readReg(uint32_t cs, uint8_t address)
{
    return radioRegister(cs, address & 0x7F, 0);
}

void writeReg(uint32_t cs, uint8_t address, uint8_t value)
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

bool waitFlagFor(uint32_t cs, uint8_t reg, uint8_t mask, uint32_t timeoutMs)
{
    uint32_t start = elapsedMs;
    do {
        if ((readReg(cs, reg) & mask) == mask) return true;
        delay_cycles(CPUCLK_FREQ / 1000);
    } while (elapsedMs - start < timeoutMs);
    return false;
}

static bool waitFlag(uint32_t cs, uint8_t reg, uint8_t mask)
{
    return waitFlagFor(cs, reg, mask, 100);
}

void setMode(uint32_t cs, uint8_t mode)
{
    writeReg(cs, REG_OPMODE, mode); // sequencer on, Listen off
    if (!waitFlag(cs, REG_IRQ1, MODE_READY)) {
        uint8_t version = readReg(cs, REG_VERSION);
        uint8_t actualMode = readReg(cs, REG_OPMODE);
        uint8_t irq1 = readReg(cs, REG_IRQ1);
        uint8_t irq2 = readReg(cs, REG_IRQ2);
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

void configureRadio(uint32_t cs, bool verbose)
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
    if (verbose) {
        uartText(cs == TX_CS ? "TX RegVersion=0x" : "RX RegVersion=0x");
        uartHex(version);
        uartText(" (expected 0x24)\r\n");
    }
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

static void initRadios(void)
{
    /* RST is not controlled by the MCU. On Adafruit #3070, tie breakout RST
     * to GND: its pull-up otherwise holds the radio in reset. Allow power-on
     * reset to settle; power-cycle the radios for a fresh hardware reset. */
    delay_cycles(CPUCLK_FREQ / 50);
    // Probe both even if TX fails, so one log compares both hardware paths.
    bool txBusOK = probeRadio(TX_CS);
    bool rxBusOK = probeRadio(RX_CS);
    if (!txBusOK && !rxBusOK)
        fatal("ERROR: TX and RX SPI tests failed\r\n");
    if (!txBusOK)
        fatal("ERROR: TX SPI test failed (PB0)\r\n");
    if (!rxBusOK)
        fatal("ERROR: RX SPI test failed (PB1)\r\n");
    configureRadio(TX_CS, true);
    configureRadio(RX_CS, true);
    setMode(RX_CS, MODE_RX);
    uartText("RFM69HCW ready; received characters follow:\r\n");
}

static void sendCharacter(uint8_t character)
{
    writeReg(TX_CS, REG_FIFO, character);
    setMode(TX_CS, MODE_TX);
    if (!waitFlag(TX_CS, REG_IRQ2, PACKET_SENT))
        fatal("\r\nERROR: TX PacketSent timeout\r\n");
    ++radioTxPackets;
    setMode(TX_CS, MODE_STANDBY);
}

static void pollReceiver(void)
{
    uint8_t flags = readReg(RX_CS, REG_IRQ2);
    if (flags & FIFO_OVERRUN) {
        ++radioRxOverruns;
        writeReg(RX_CS, REG_IRQ2, FIFO_OVERRUN);
        writeReg(RX_CS, REG_PACKET2, 0x06); // restart RX, keep auto-restart
    } else if ((flags & (PAYLOAD_READY | CRC_OK)) == (PAYLOAD_READY | CRC_OK)) {
        uint8_t character = readReg(RX_CS, REG_FIFO);
        ++radioRxPackets;
        uartText("Received: ");
        (void)uartByte(character);
        uartText("\r\n");
    }
}

void radioBoardInit(void)
{
    // PA0 is open-drain; initialize LED1 off before enabling its output.
    DL_GPIO_setPins(GPIOA, HEARTBEAT_LED);
    DL_GPIO_initDigitalOutput(IOMUX_PINCM1);
    DL_GPIO_enableOutput(GPIOA, HEARTBEAT_LED);
    // Manual active-low GPIO chip selects. Preload high before enabling output.
    DL_GPIO_setPins(GPIOB, TX_CS | RX_CS);
    DL_GPIO_initDigitalOutput(IOMUX_PINCM12); // PB0
    DL_GPIO_initDigitalOutput(IOMUX_PINCM13); // PB1
    DL_GPIO_enableOutput(GPIOB, TX_CS | RX_CS);
    if (SysTick_Config(CPUCLK_FREQ / 1000) != 0)
        fatal("ERROR: SysTick setup\r\n");
}

void main_radio(void)
{
    radioBoardInit();
    uartText("\r\nRFM69 demo: UART ready, starting SPI diagnostics\r\n");
    initRadios();

    static const char message[] = "HELLO";
    unsigned index = 0;
    uint32_t lastHeartbeat = elapsedMs;
    uint32_t lastPacket = lastHeartbeat - 51;
    for (;;) {
        pollReceiver();
        uint32_t now = elapsedMs;
        if (now - lastHeartbeat >= 1000) {
            lastHeartbeat = now;
            uartText("\r\nalive\r\n");
            DL_GPIO_togglePins(GPIOA, HEARTBEAT_LED);
        }
        if (now - lastPacket >= 51) {
            sendCharacter((uint8_t)message[index]);
            index = (index + 1) % (sizeof(message) - 1);
            // A 51-tick gap gives at least 50 ms in standby after each packet.
            lastPacket = elapsedMs;
        }
        delay_cycles(CPUCLK_FREQ / 1000);
    }
}

