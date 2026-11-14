//
// audio_i2s.c - audio output as I2S via PIO and DMA (FS_AUDIO_OUT=I2S).
//
// Drives an external I2S DAC. Two variants, chosen in CMakeLists.txt:
//
// - FS_I2S_MCLK_PIN < 0 (default): no MCLK, for DACs that make their own
//   (MAX98357A, PCM5102A, the PCM5101A Waveshare Pico-Audio). PIO program
//   audio_i2s.pio, BCLK = 32 x Fs on FS_I2S_CLOCK_PIN_BASE, LRCLK on the
//   next GPIO.
// - FS_I2S_MCLK_PIN >= 0: MCLK = 256 x Fs on that pin, for DACs that need
//   it (the CS4344 Waveshare Pico-Audio Rev2.1). PIO program
//   audio_i2s_mclk.pio on two state machines, BCLK = 64 x Fs, and the clock
//   pins the other way round: LRCLK on FS_I2S_CLOCK_PIN_BASE, BCLK on the
//   next GPIO. The MCLK machine runs at exactly 4 x the data machine's
//   divider and both start in sync, so MCLK / LRCLK is exactly 256.
//
// Data on FS_I2S_DATA_PIN in both. One DMA channel copies one 32-bit word
// (a whole L+R frame) per PIO TX FIFO request; the PIO program shifts it out. The DMA
// completion interrupt chains the next buffer from the AudioRing. See
// audio_out.h for the interface contract.
//
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#include "audio_out.h"
#include "audio_ring.h"
#if FS_I2S_MCLK_PIN >= 0
#include "audio_i2s_mclk.pio.h"
#else
#include "audio_i2s.pio.h"
#endif

static AudioRing ring;
static PIO pio;
static uint sm;
static int dma_chan;
static float out_rate;      // Actual frame rate in Hz

// DMA_IRQ_1 handler: the transfer of one buffer has finished; restart the
// channel on the next buffer (or silence). Runs from RAM, on the core that
// called audio_out_init(). Restarting immediately keeps the gap between
// buffers to a few cycles, well within the PIO FIFO's slack.
static void __not_in_flash_func(dma_handler)(void) {
    dma_irqn_acknowledge_channel(1, (uint)dma_chan);
    dma_channel_set_read_addr((uint)dma_chan, audio_ring_next(&ring), true);
}

// See audio_out.h. Claims a free PIO state machine and DMA channel, and
// shares DMA_IRQ_1 with any other user at the highest order priority.
bool audio_out_init(uint32_t sample_rate) {
    uint offset;
#if FS_I2S_MCLK_PIN >= 0
    if (!pio_claim_free_sm_and_add_program(&audio_i2s_mclk_data_program, &pio, &sm, &offset)) return false;
    if (!pio_can_add_program(pio, &audio_i2s_mclk_clock_program)) return false;
    const uint mclk_offset = pio_add_program(pio, &audio_i2s_mclk_clock_program);
    const int mclk_sm = pio_claim_unused_sm(pio, false);
    if (mclk_sm < 0) return false;
    audio_i2s_mclk_data_program_init(pio, sm, offset, FS_I2S_DATA_PIN, FS_I2S_CLOCK_PIN_BASE);
    audio_i2s_mclk_clock_program_init(pio, (uint)mclk_sm, mclk_offset, FS_I2S_MCLK_PIN);

    // MCLK machine: 2 PIO cycles per MCLK period, MCLK = 256 x Fs. The data
    // machine (2 cycles per bit, 64 bits per frame) gets exactly 4 x that
    // divider, which is still representable in 16.8 fixed point.
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    const uint32_t mdiv256 = (uint32_t)(((uint64_t)sys_hz * 256u + sample_rate * 256u) / (sample_rate * 512u));
    const uint32_t ddiv256 = mdiv256 * 4u;
    pio_sm_set_clkdiv_int_frac8(pio, (uint)mclk_sm, mdiv256 >> 8, (uint8_t)(mdiv256 & 0xffu));
    pio_sm_set_clkdiv_int_frac8(pio, sm, ddiv256 >> 8, (uint8_t)(ddiv256 & 0xffu));
    out_rate = (float)sys_hz * 256.0f / ((float)mdiv256 * 512.0f);
    const uint32_t sm_mask = (1u << sm) | (1u << (uint)mclk_sm);
#else
    if (!pio_claim_free_sm_and_add_program(&audio_i2s_program, &pio, &sm, &offset)) return false;
    audio_i2s_program_init(pio, sm, offset, FS_I2S_DATA_PIN, FS_I2S_CLOCK_PIN_BASE);

    // 32 bits per frame, 2 PIO cycles per bit
    const float div = (float)clock_get_hz(clk_sys) / ((float)sample_rate * 64.0f);
    pio_sm_set_clkdiv(pio, sm, div);
    out_rate = (float)clock_get_hz(clk_sys) / (div * 64.0f);
    const uint32_t sm_mask = 1u << sm;
#endif

    // Start on the silence buffer (static, so already zero)
    ring.playing_silence = true;

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    dma_channel_configure((uint)dma_chan, &c, &pio->txf[sm], ring.silence, AUDIO_OUT_FRAMES, false);

    dma_irqn_set_channel_enabled(1, (uint)dma_chan, true);
    irq_add_shared_handler(DMA_IRQ_1, dma_handler, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    dma_channel_start((uint)dma_chan);
    pio_enable_sm_mask_in_sync(pio, sm_mask);
    return true;
}

// See audio_out.h. The int16 L,R pair is already in the word layout the
// PIO program expects, so the buffer is used as is.
int16_t *audio_out_acquire(void) {
    return (int16_t *)audio_ring_acquire(&ring);
}

// See audio_out.h.
void audio_out_submit(void) {
    audio_ring_submit(&ring);
}

// See audio_out.h.
uint32_t audio_out_underruns(void) {
    return ring.underruns;
}

// See audio_out.h.
float audio_out_rate(void) {
    return out_rate;
}
