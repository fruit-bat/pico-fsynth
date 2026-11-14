//
// audio_i2s.c - I2S audio output on the ESP32-P4. See audio_i2s.h.
//
// Standard (Philips) I2S, 16-bit stereo, BCLK = 32 x Fs. The clock comes
// from the audio PLL (APLL), so 44.1 kHz is exact. MCLK = 256 x Fs is
// output only if a pin is set: PCM510x DACs derive their clocks from BCLK.
//
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "sdkconfig.h"

#include "audio_i2s.h"

static const char *TAG = "audio";

static i2s_chan_handle_t tx;
static volatile uint32_t underruns;

// I2S interrupt: the DMA ring ran dry, so a buffer of zeros was sent.
static bool IRAM_ATTR on_underrun(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx) {
    (void)handle; (void)event; (void)ctx;
    underruns = underruns + 1;
    return false;
}

bool audio_init(uint32_t sample_rate) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = AUDIO_DMA_BUFFERS;
    chan_cfg.dma_frame_num = AUDIO_FRAMES;
    chan_cfg.auto_clear = true;   // Send zeros when no data is queued
    if (i2s_new_channel(&chan_cfg, &tx, NULL) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CONFIG_FSYNTH_I2S_MCLK_PIN,
            .bclk = CONFIG_FSYNTH_I2S_BCK_PIN,
            .ws = CONFIG_FSYNTH_I2S_WS_PIN,
            .dout = CONFIG_FSYNTH_I2S_DIN_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "APLL unavailable, using the default clock");
        std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
        if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) return false;
    }

    const i2s_event_callbacks_t cbs = { .on_send_q_ovf = on_underrun };
    i2s_channel_register_event_callback(tx, &cbs, NULL);

    return i2s_channel_enable(tx) == ESP_OK;
}

void audio_write(const int16_t *frames) {
    size_t written;
    i2s_channel_write(tx, frames, AUDIO_FRAMES * 4, &written, portMAX_DELAY);
}

uint32_t audio_underruns(void) {
    return underruns;
}
