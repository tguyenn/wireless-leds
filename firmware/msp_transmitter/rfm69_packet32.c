/* RFM69HCW fixed-length 32-byte packet test.
 * TX radio (NSS = PB0) sends one 32-byte packet; RX radio (NSS = PB1)
 * receives it. Both radios share SPI0. Run by calling main_packet32().
 *
 * Packet on air (datasheet 5.5.2.1, fixed length format):
 *   3 preamble + 2 sync + 32 payload + 2 CRC = 39 bytes, ~65 ms at 4.8 kb/s.
 * The payload is written to and read from the FIFO with SPI burst access
 * (datasheet 5.2.1): one address byte, then 32 data bytes with NSS held low.
 * 32 bytes fits in the 66-byte FIFO, so no refilling is needed during TX.
 */
#include "rfm69.h"

#define PACKET32_LENGTH 32
#define TX_TIMEOUT_MS 500 // PacketSent: ~70 ms expected
#define RX_TIMEOUT_MS 500 // PayloadReady after PacketSent: ~0 ms expected

static const char packet32Message[PACKET32_LENGTH + 1] =
    "Fixed 32 bit length packet test_";
_Static_assert(sizeof(packet32Message) - 1 == PACKET32_LENGTH,
               "test message must be exactly 32 bytes");

/* Burst FIFO access: write (address | 0x80) or read (address & 0x7F). */
static void fifoBurst(uint32_t cs, bool write, uint8_t *data, unsigned length)
{
    uint32_t budget = 100000;
    DL_GPIO_setPins(GPIOB, TX_CS | RX_CS);
    delay_cycles(CPUCLK_FREQ / 1000000); // >= 1 us NSS high/setup/hold
    DL_GPIO_clearPins(GPIOB, cs);
    delay_cycles(CPUCLK_FREQ / 1000000);
    (void)spiByte(write ? (REG_FIFO | 0x80) : REG_FIFO);
    for (unsigned i = 0; i < length; ++i) {
        if (write)
            (void)spiByte(data[i]);
        else
            data[i] = spiByte(0x00);
    }
    while (DL_SPI_isBusy(SPI_0_INST)) {
        if (--budget == 0) fatal("\r\nERROR: SPI0 busy (FIFO burst)\r\n");
    }
    delay_cycles(CPUCLK_FREQ / 1000000);
    DL_GPIO_setPins(GPIOB, cs);
}

static void printPayload(const char *label, const uint8_t *data)
{
    uartText(label);
    uartText("\r\n\"");
    for (unsigned i = 0; i < PACKET32_LENGTH; ++i) {
        uint8_t c = data[i];
        (void)uartByte((c >= 0x20 && c < 0x7F) ? c : '?');
    }
    uartText("\"\r\n");
}

static void configurePacket32(uint32_t cs)
{
    configureRadio(cs, false); // shared FSK settings, fixed length, CRC on
    writeReg(cs, REG_PAYLOAD_LENGTH, PACKET32_LENGTH);
    if (readReg(cs, REG_PAYLOAD_LENGTH) != PACKET32_LENGTH)
        fatal(cs == TX_CS ? "\r\nERROR: TX PayloadLength readback\r\n" :
                            "\r\nERROR: RX PayloadLength readback\r\n");
}

static void sendPacket32(const uint8_t *payload)
{
    // Fill the FIFO in standby; TX starts as soon as Tx mode is entered.
    fifoBurst(TX_CS, true, (uint8_t *)payload, PACKET32_LENGTH);
    setMode(TX_CS, MODE_TX);
    if (!waitFlagFor(TX_CS, REG_IRQ2, PACKET_SENT, TX_TIMEOUT_MS))
        fatal("\r\nERROR: TX PacketSent timeout\r\n");
    setMode(TX_CS, MODE_STANDBY);
}

static void receivePacket32(uint8_t *payload)
{
    // PayloadReady is only raised when the CRC is good (CrcAutoClearOff = 0).
    if (!waitFlagFor(RX_CS, REG_IRQ2, PAYLOAD_READY, RX_TIMEOUT_MS)) {
        uint8_t irq1 = readReg(RX_CS, REG_IRQ1);
        uint8_t irq2 = readReg(RX_CS, REG_IRQ2);
        uartText("\r\nERROR: RX PayloadReady timeout IRQ1=0x"); uartHex(irq1);
        uartText(" IRQ2=0x"); uartHex(irq2);
        fatal("\r\n");
    }
    // Rx -> Standby keeps the FIFO (datasheet Table 20), and stops auto-restart.
    setMode(RX_CS, MODE_STANDBY);
    fifoBurst(RX_CS, false, payload, PACKET32_LENGTH);
}

void main_packet32(void)
{
    uint8_t received[PACKET32_LENGTH];

    radioBoardInit();
    delay_cycles(CPUCLK_FREQ / 50); // radio power-on reset settle time
    configurePacket32(TX_CS);
    configurePacket32(RX_CS);

    // Repeat the test forever; LED1 toggles once per passing packet.
    for (;;) {
        setMode(RX_CS, MODE_RX); // listen before transmitting
        sendPacket32((const uint8_t *)packet32Message);
        printPayload("Message Sent:", (const uint8_t *)packet32Message);

        receivePacket32(received); // leaves RX radio in standby
        printPayload("Message Received:", received);

        for (unsigned i = 0; i < PACKET32_LENGTH; ++i) {
            if (received[i] != (uint8_t)packet32Message[i])
                fatal("ERROR: received payload does not match sent payload\r\n");
        }

        DL_GPIO_togglePins(GPIOA, HEARTBEAT_LED);
        delay_cycles(CPUCLK_FREQ / 2); // 0.5 s between packets
    }
}
