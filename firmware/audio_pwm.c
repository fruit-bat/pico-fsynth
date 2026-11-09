//
// audio_pwm.c - audio output as PWM (FS_AUDIO_OUT=PWM).
//
// One PWM slice: left on channel A (FS_PWM_PIN_L), right on channel B
// (FS_PWM_PIN_R); both pins must belong to the same slice. The PWM period
// is one sample, so the carrier is at the sample rate and the resolution
// is log2(sys clock / sample rate): about 11.7 bits at 150 MHz / 44.1 kHz,
// 12.5 bits at 250 MHz. Each pin needs an RC low pass filter, and the
// carrier is audible-adjacent, so this is a fallback for boards without a
// DAC rather than a hi-fi output.
//
// A DMA channel writes one 32-bit word per PWM wrap to the slice's CC
// register (A level in the low half, B in the high half). The samples are
// converted from signed int16 to compare levels in audio_out_submit(), so
// the DMA can copy them unchanged. See audio_out.h for the interface.
//
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#include "audio_out.h"
#include "audio_ring.h"

static AudioRing ring;
static int dma_chan;
static uint slice;
static uint32_t top;        // PWM wrap value: levels are 0..top
static float out_rate;      // Actual sample rate in Hz: sys clock / (top + 1)
static uint32_t *current;   // Buffer handed out by acquire

// DMA_IRQ_1 handler: chain the next buffer (or silence). Runs from RAM, on
// the core that called audio_out_init().
static void __not_in_flash_func(dma_handler)(void) {
    dma_irqn_acknowledge_channel(1, (uint)dma_chan);
    dma_channel_set_read_addr((uint)dma_chan, audio_ring_next(&ring), true);
}

// See audio_out.h. Fails if the two pins are not channels A and B of one
// slice, or if the sample rate is too low for a 16-bit wrap value. The
// silence buffer holds the mid level (not zero), so an underrun gives
// silence rather than a full-scale step.
bool audio_out_init(uint32_t sample_rate) {
    slice = pwm_gpio_to_slice_num(FS_PWM_PIN_L);
    if (pwm_gpio_to_slice_num(FS_PWM_PIN_R) != slice ||
        pwm_gpio_to_channel(FS_PWM_PIN_L) != PWM_CHAN_A ||
        pwm_gpio_to_channel(FS_PWM_PIN_R) != PWM_CHAN_B) {
        return false;
    }

    const uint32_t sys = clock_get_hz(clk_sys);
    top = (sys + sample_rate / 2) / sample_rate - 1;
    if (top > 65535) return false;
    out_rate = (float)sys / (float)(top + 1);

    gpio_set_function(FS_PWM_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(FS_PWM_PIN_R, GPIO_FUNC_PWM);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&cfg, 1);
    pwm_config_set_wrap(&cfg, (uint16_t)top);
    pwm_init(slice, &cfg, false);

    const uint32_t mid = (top + 1) / 2;
    for (int i = 0; i < AUDIO_OUT_FRAMES; ++i) ring.silence[i] = mid | (mid << 16);
    pwm_set_both_levels(slice, (uint16_t)mid, (uint16_t)mid);
    ring.playing_silence = true;

    // One 32-bit write per sample to the CC register (A low, B high)
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pwm_get_dreq(slice));
    dma_channel_configure((uint)dma_chan, &c, &pwm_hw->slice[slice].cc, ring.silence, AUDIO_OUT_FRAMES, false);

    dma_irqn_set_channel_enabled(1, (uint)dma_chan, true);
    irq_add_shared_handler(DMA_IRQ_1, dma_handler, PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    pwm_set_enabled(slice, true);
    dma_channel_start((uint)dma_chan);
    return true;
}

// See audio_out.h. Remembers the buffer so submit can convert it.
int16_t *audio_out_acquire(void) {
    current = audio_ring_acquire(&ring);
    return (int16_t *)current;
}

// See audio_out.h. Converts the buffer in place from signed int16 pairs to
// PWM compare levels (offset to unsigned, scaled to 0..top-1), then queues
// it. In RAM because it runs once per buffer on the render path.
void __not_in_flash_func(audio_out_submit)(void) {
    // Convert signed 16-bit pairs to PWM compare values in place
    uint32_t *w = current;
    for (int i = 0; i < AUDIO_OUT_FRAMES; ++i) {
        const uint32_t v = w[i];
        const uint32_t l = (uint32_t)((int32_t)(int16_t)(v & 0xffff) + 32768);
        const uint32_t r = (uint32_t)((int32_t)(int16_t)(v >> 16) + 32768);
        w[i] = ((l * top) >> 16) | (((r * top) >> 16) << 16);
    }
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
