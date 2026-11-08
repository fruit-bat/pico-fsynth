//
// I2S output via PIO and DMA (e.g. MAX98357A, PCM5102A).
//
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#include "audio_out.h"
#include "audio_ring.h"
#include "audio_i2s.pio.h"

static AudioRing ring;
static PIO pio;
static uint sm;
static int dma_chan;
static float out_rate;

static void __not_in_flash_func(dma_handler)(void) {
    dma_irqn_acknowledge_channel(1, (uint)dma_chan);
    dma_channel_set_read_addr((uint)dma_chan, audio_ring_next(&ring), true);
}

bool audio_out_init(uint32_t sample_rate) {
    uint offset;
    if (!pio_claim_free_sm_and_add_program(&audio_i2s_program, &pio, &sm, &offset)) return false;
    audio_i2s_program_init(pio, sm, offset, FS_I2S_DATA_PIN, FS_I2S_CLOCK_PIN_BASE);

    // 32 bits per frame, 2 PIO cycles per bit
    const float div = (float)clock_get_hz(clk_sys) / ((float)sample_rate * 64.0f);
    pio_sm_set_clkdiv(pio, sm, div);
    out_rate = (float)clock_get_hz(clk_sys) / (div * 64.0f);

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
    pio_sm_set_enabled(pio, sm, true);
    return true;
}

int16_t *audio_out_acquire(void) {
    return (int16_t *)audio_ring_acquire(&ring);
}

void audio_out_submit(void) {
    audio_ring_submit(&ring);
}

uint32_t audio_out_underruns(void) {
    return ring.underruns;
}

float audio_out_rate(void) {
    return out_rate;
}
