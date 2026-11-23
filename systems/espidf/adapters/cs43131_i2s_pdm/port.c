/* SPDX-License-Identifier: MIT */
/* CS43131 register programming follows Cirrus DS1155F2 sections 4.13/5.7/7.
 * Public PCM stays s16le stereo/16 kHz. The port converts low rates to a
 * supported DAC clock; PDM input is independently clocked at the ABI rate. */
#include "cs43131_wiring.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "audio_port.h"
#include "audio_codec.h"
#include "aster_platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "aster_audio_output.h"
#include "aster_audio_declick.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <string.h>
const struct device aster_idf_gpio_device = {.role = 3};
const struct gpio_dt_spec aster_idf_audio_pa = {0};
static const struct device control = {.role = 1}, stream = {.role = 2};
static i2c_master_dev_handle_t codec;
static i2s_chan_handle_t tx, rx;
static struct i2s_config tx_config, rx_config;
static bool ready, available, tx_on, rx_on;
static atomic_bool cancelled;
static unsigned ratio = 1, physical_rate;
static int32_t *converted;
static size_t converted_capacity;
static int16_t previous[2];
static bool have_previous;
/* All legacy and native calls share a hardware lock. Never take the legacy
 * player's audio_lock here: its caller already owns that lock. */
static SemaphoreHandle_t hardware_lock;
static atomic_bool native_owner;
static aster_audio_format_t native_format;
static uint64_t native_frames;
static uint32_t native_preroll_frames;
static uint32_t native_errors;
/* With 8 x 128-frame DMA buffers, priming two 512-frame blocks
 * doubles the initial runway without extra allocation. */
enum { NATIVE_PREROLL_TARGET_FRAMES = 768, NATIVE_START_FADE_MS = 5 };
static int native_register(void);
static atomic_uint volume_pending;
static int codec_volume_locked(uint8_t percent);

static void flush_volume_locked(void)
{
    unsigned command = atomic_exchange(&volume_pending, 0);
    if (command && ready) {
        int r = codec_volume_locked((uint8_t)(command - 1));
        if (r) {
            native_errors++;
            ESP_LOGW("cs43131", "queued volume failed: %d", r);
        }
    }
}

static bool lock_hardware(void)
{
    if (!hardware_lock || xSemaphoreTake(hardware_lock, portMAX_DELAY) != pdTRUE)
        return false;
    flush_volume_locked();
    return true;
}

static void unlock_hardware(void)
{
    flush_volume_locked();
    xSemaphoreGive(hardware_lock);
}

static int lock_legacy_hardware(void)
{
    if (!hardware_lock)
        return -ENODEV;
    if (native_owner)
        return -EBUSY;
    /* Also bound the race where native playback acquires the device just
  * after the first ownership check. Never hold the legacy audio_lock
  * waiting indefinitely behind a higher-priority streaming writer. */
    if (xSemaphoreTake(hardware_lock, pdMS_TO_TICKS(5)) != pdTRUE)
        return -EBUSY;
    if (native_owner) {
        xSemaphoreGive(hardware_lock);
        return -EBUSY;
    }
    flush_volume_locked();
    return 0;
}

static int error(esp_err_t r)
{
    return r == ESP_OK            ? 0
           : r == ESP_ERR_TIMEOUT ? -ETIMEDOUT
           : r == ESP_ERR_NO_MEM  ? -ENOMEM
                                  : -EIO;
}

static int reg_read(uint32_t reg, uint8_t *value)
{
    uint8_t address[] = {reg >> 16, reg >> 8, reg, 0};
    return error(i2c_master_transmit_receive(codec, address, sizeof(address), value, 1, 200));
}

static int reg_write(uint32_t reg, uint8_t value)
{
    uint8_t bytes[] = {reg >> 16, reg >> 8, reg, 0, value};
    return error(i2c_master_transmit(codec, bytes, sizeof(bytes), 200));
}

static const struct device *open_control_bus(void)
{
    if (codec)
        return &control;
    gpio_config_t pin = {.pin_bit_mask = 1ULL << aster_cs43131_wiring.reset, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&pin) != ESP_OK)
        return NULL;
    gpio_set_level(aster_cs43131_wiring.reset, 0);
    k_msleep(2);
    gpio_set_level(aster_cs43131_wiring.reset, 1);
    k_msleep(20);
    i2c_master_bus_handle_t bus;
    if (i2c_master_get_bus_handle(aster_cs43131_wiring.i2c_port, &bus) != ESP_OK) {
        i2c_master_bus_config_t config = {.i2c_port = aster_cs43131_wiring.i2c_port,
                                          .sda_io_num = aster_cs43131_wiring.sda,
                                          .scl_io_num = aster_cs43131_wiring.scl,
                                          .clk_source = I2C_CLK_SRC_DEFAULT,
                                          .glitch_ignore_cnt = 7,
                                          .flags.enable_internal_pullup = true};
        if (i2c_new_master_bus(&config, &bus) != ESP_OK)
            return NULL;
    }
    i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                  .device_address = aster_cs43131_wiring.dac_address,
                                  .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bus, &config, &codec) != ESP_OK)
        return NULL;
    return &control;
}

const struct device *aster_idf_audio_i2s(void)
{
    return &stream;
}

bool device_is_ready(const struct device *d)
{
    return d && (d->role != 1 || codec);
}

int gpio_is_ready_dt(const struct gpio_dt_spec *p)
{
    return p && p->port && GPIO_IS_VALID_OUTPUT_GPIO((int)p->pin);
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *p, int mode)
{
    (void)p;
    (void)mode;
    return -ENOTSUP;
}

int gpio_pin_set_dt(const struct gpio_dt_spec *p, int value)
{
    (void)p;
    (void)value;
    return -ENOTSUP;
}

static int codec_volume_locked(uint8_t percent)
{
    if (!ready)
        return -ENODEV;
    if (percent > 100)
        percent = 100;
    /* 0.5 dB/step attenuation; no positive digital gain on headphone output. */
    uint8_t attenuation = percent ? ((100 - percent) * 120 / 100) : 0xff;
    int r = reg_write(0x90002, attenuation);
    if (r)
        return r;
    return reg_write(0x90003, percent ? 0xec : 0xef);
}

static int cs43131_volume(uint8_t percent)
{
    if (!ready)
        return -ENODEV;
    if (percent > 100)
        percent = 100;
    /* UI/remote control never waits for a blocking DMA write. The owner applies
  * the latest value before/after a block; when idle it is applied on open. */
    atomic_store(&volume_pending, (unsigned)percent + 1);
    return 0;
}

static int cs43131_init(uint8_t volume)
{
    if (ready)
        return 0;
    if (!open_control_bus())
        return -ENODEV;
    uint8_t id[3];
    for (unsigned i = 0; i < 3; i++) {
        int r = reg_read(0x10000 + i, &id[i]);
        if (r)
            return r;
    }
    if (id[0] != 0x43 || id[1] != 0x13 || (id[2] & 0xf0) != 0x10)
        return -ENODEV;

    static const struct {
        uint32_t address;
        uint8_t value;
    } settings[] = {{0x20052, 0x06},                  /* 24.576 MHz crystal, 7.5 uA bias. */
                    {0x20000, 0xf6},                  /* XTAL on; HP/ASP/PLL remain off. */
                    {0x1000b, 0x02}, {0x1000c, 0x04}, /* 48k / 32-bit ASP samples. */
                    {0x40018, 0x0c}, {0x40019, 0x0a}, /* I2S slave, one clock delay. */
                    {0x50000, 0},    {0x50001, 0},    {0x5000a, 0x07}, {0x5000b, 0x0f},
                    {0x90000, 0x02}, {0x90001, 0},    {0x90002, 0x30}, {0x90003, 0xef},
                    {0x90004, 0},    {0xb0000, 0x1e}, {0x80000, 0x00}, {0x1000d, 0x03}};

    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
        int r = reg_write(settings[i].address, settings[i].value);
        if (r)
            return r;
    }
    bool crystal = false;
    for (unsigned i = 0; i < 100; i++) {
        uint8_t status;
        int r = reg_read(0xf0000, &status);
        if (r)
            return r;
        if (status & 0x10) {
            crystal = true;
            break;
        }
        k_msleep(2);
    }
    if (!crystal) {
        ESP_LOGE("cs43131", "Crystal not ready");
        return -ETIMEDOUT;
    }
    int r = reg_write(0x10006, 0x00);
    if (r)
        return r; /* Direct 24.576 MHz. */
    k_msleep(1);
    ready = true;
    physical_rate = 48000;
    ESP_LOGI("cs43131", "verified device 43:13:10; 24.576 MHz XTAL, I2S slave");
    return cs43131_volume(volume);
}

static int codec_enable_locked(void)
{
    if (!ready)
        return -ENODEV;
    int r = reg_write(0x10010, 0x99);
    if (r)
        return r;
    r = reg_write(0x80032, 0x20);
    if (r)
        return r;
    r = reg_write(0x20000, 0xb6);
    if (r)
        return r;
    r = reg_write(0x20000, 0xa6);
    if (r)
        return r;
    k_msleep(12);
    r = reg_write(0x80032, 0);
    if (r)
        return r;
    return reg_write(0x10010, 0);
}

static int cs43131_enable(uint8_t volume)
{
    (void)volume;
    int acquired = lock_legacy_hardware();
    if (acquired)
        return acquired;
    int r = native_owner ? -EBUSY : codec_enable_locked();
    unlock_hardware();
    return r;
}

static void cs43131_identify(int error, uint8_t *dac, uint8_t *adc)
{
    if (dac)
        *dac = error ? 0 : 0x31;
    if (adc)
        *adc = 0; /* The PDM microphone has no I2C identity register. */
}

const aster_audio_codec_ops_t aster_audio_codec = {
    .init = cs43131_init,
    .enable = cs43131_enable,
    .set_volume = cs43131_volume,
    .identify = cs43131_identify,
};

void aster_idf_audio_cancel_record(bool v)
{
    atomic_store(&cancelled, v);
}

bool aster_idf_audio_record_cancelled(void)
{
    return atomic_load(&cancelled);
}

int aster_idf_audio_start(void)
{
    if (!hardware_lock)
        hardware_lock = xSemaphoreCreateMutex();
    if (!hardware_lock)
        return -ENOMEM;
    uint8_t a, b;
    int r = aster_plat_audio_probe(&a, &b);
    available = !r;
    if (!r)
        r = native_register();
    return r;
}

bool aster_idf_audio_available(void)
{
    return available;
}

static void shutdown(void)
{
    if (tx_on)
        i2s_channel_disable(tx);
    if (rx_on)
        i2s_channel_disable(rx);
    tx_on = rx_on = false;
    if (tx)
        i2s_del_channel(tx);
    if (rx)
        i2s_del_channel(rx);
    tx = rx = NULL;
    free(converted);
    converted = NULL;
    converted_capacity = 0;
    have_previous = false;
    native_preroll_frames = 0;
    memset(&tx_config, 0, sizeof(tx_config));
    memset(&rx_config, 0, sizeof(rx_config));
}

static int legacy_configure_locked(const struct device *d, enum i2s_dir dir,
                                   const struct i2s_config *config)
{
    (void)d;
    if (!config->frame_clk_freq) {
        shutdown();
        return 0;
    }
    if (config->word_size != 16 || config->channels != 2 || !config->mem_slab ||
        !config->block_size)
        return -EINVAL;
    if (dir == ASTER_I2S_TX) {
        shutdown();
        unsigned rate = config->frame_clk_freq;
        ratio = 1;
        switch (rate) {
        case 8000:
            ratio = 6;
            break;
        case 11025:
            ratio = 4;
            break;
        case 12000:
            ratio = 4;
            break;
        case 16000:
            ratio = 3;
            break;
        case 22050:
            ratio = 2;
            break;
        case 24000:
            ratio = 2;
            break;
        default:
            break;
        }
        rate *= ratio;
        uint8_t code;
        switch (rate) {
        case 32000:
            code = 0;
            break;
        case 44100:
            code = 1;
            break;
        case 48000:
            code = 2;
            break;
        case 88200:
            code = 3;
            break;
        case 96000:
            code = 4;
            break;
        default:
            return -EINVAL;
        }
        int r = reg_write(0x20000, 0xf6);
        if (r)
            return r;
        r = reg_write(0x1000b, code);
        if (r)
            return r;
        physical_rate = rate;
        converted_capacity = config->block_size / 4 * ratio * 2 * sizeof(int32_t);
        converted = heap_caps_malloc(converted_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!converted)
            return -ENOMEM;
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(aster_cs43131_wiring.tx_port, I2S_ROLE_MASTER);
        channel.dma_desc_num = 8;
        channel.dma_frame_num = 128;
        channel.auto_clear_after_cb = true;
        esp_err_t status = i2s_new_channel(&channel, &tx, NULL);
        i2s_std_config_t standard = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
                                     .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                                         I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
                                     .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                                                  .bclk = aster_cs43131_wiring.bclk,
                                                  .ws = aster_cs43131_wiring.ws,
                                                  .dout = aster_cs43131_wiring.dout,
                                                  .din = I2S_GPIO_UNUSED}};
        standard.clk_cfg.clk_src = aster_cs43131_wiring.tx_clock;
        if (rate >= 88200 && standard.clk_cfg.clk_src == I2S_CLK_SRC_DEFAULT)
            standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
        if (status == ESP_OK)
            status = i2s_channel_init_std_mode(tx, &standard);
        if (status != ESP_OK) {
            shutdown();
            return error(status);
        }
        tx_config = *config;
        return codec_enable_locked();
    }
    if (dir != ASTER_I2S_RX)
        return -EINVAL;
    if (rx_on) {
        i2s_channel_disable(rx);
        rx_on = false;
    }
    if (rx)
        i2s_del_channel(rx);
    rx = NULL;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(aster_cs43131_wiring.rx_port, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 128;
    esp_err_t status = i2s_new_channel(&channel, NULL, &rx);
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(config->frame_clk_freq),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = aster_cs43131_wiring.mic_clock, .din = aster_cs43131_wiring.mic_data}};
    if (status == ESP_OK)
        status = i2s_channel_init_pdm_rx_mode(rx, &pdm);
    if (status != ESP_OK) {
        if (rx)
            i2s_del_channel(rx);
        rx = NULL;
        return error(status);
    }
    rx_config = *config;
    return 0;
}

static int legacy_trigger_locked(const struct device *d, enum i2s_dir dir, int trigger)
{
    (void)d;
    if (trigger == I2S_TRIGGER_DRAIN) {
        if (tx_on)
            k_msleep(8 * 128 * 1000 / physical_rate + 2);
        return 0;
    }
    if (trigger == I2S_TRIGGER_DROP || trigger == I2S_TRIGGER_PREPARE) {
        if ((dir & ASTER_I2S_TX) && tx_on) {
            i2s_channel_disable(tx);
            tx_on = false;
        }
        if ((dir & ASTER_I2S_RX) && rx_on) {
            i2s_channel_disable(rx);
            rx_on = false;
        }
        return 0;
    }
    if (trigger != I2S_TRIGGER_START)
        return -EINVAL;
    esp_err_t status = ESP_OK;
    if ((dir & ASTER_I2S_TX) && !tx_on) {
        status = tx ? i2s_channel_enable(tx) : ESP_ERR_INVALID_STATE;
        if (status == ESP_OK)
            tx_on = true;
    }
    if (status == ESP_OK && (dir & ASTER_I2S_RX) && !rx_on) {
        status = rx ? i2s_channel_enable(rx) : ESP_ERR_INVALID_STATE;
        if (status == ESP_OK)
            rx_on = true;
    }
    return error(status);
}

static int legacy_write_locked(const struct device *d, void *block, size_t size)
{
    (void)d;
    if (!tx || !converted || size % 4 || size / 4 * ratio * 8 > converted_capacity)
        return -EINVAL;
    const int16_t *input = block;
    size_t n = 0;
    for (size_t i = 0; i < size / 4; i++) {
        if (!have_previous) {
            previous[0] = input[i * 2];
            previous[1] = input[i * 2 + 1];
            have_previous = true;
        }
        for (unsigned j = 1; j <= ratio; j++)
            for (unsigned ch = 0; ch < 2; ch++) {
                int value = previous[ch] +
                            ((int32_t)input[i * 2 + ch] - previous[ch]) * (int)j / (int)ratio;
                converted[n++] = (int32_t)value * 65536;
            }
        previous[0] = input[i * 2];
        previous[1] = input[i * 2 + 1];
    }
    size_t bytes = n * sizeof(int32_t), wrote = 0;
    esp_err_t status = tx_on ? i2s_channel_write(tx, converted, bytes, &wrote, tx_config.timeout)
                             : i2s_channel_preload_data(tx, converted, bytes, &wrote);
    if (status != ESP_OK)
        return error(status);
    if (wrote != bytes)
        return -EIO;
    /* Start after one complete block; never overfill DMA while preloading. */
    if (!tx_on) {
        status = i2s_channel_enable(tx);
        if (status != ESP_OK)
            return error(status);
        tx_on = true;
    }
    k_mem_slab_free(tx_config.mem_slab, block);
    return 0;
}

static int legacy_read_locked(const struct device *d, void **block, size_t *size)
{
    (void)d;
    if (!rx_on || !rx_config.mem_slab)
        return -ENODEV;
    int r = k_mem_slab_alloc(rx_config.mem_slab, block, pdMS_TO_TICKS(rx_config.timeout));
    if (r)
        return r;
    size_t got = 0;
    esp_err_t status =
        i2s_channel_read(rx, *block, rx_config.block_size / 2, &got, rx_config.timeout);
    if (status != ESP_OK || got % 2) {
        k_mem_slab_free(rx_config.mem_slab, *block);
        *block = NULL;
        return status == ESP_OK ? -EIO : error(status);
    }
    int16_t *pcm = *block;
    for (size_t i = got / 2; i > 0; i--) {
        pcm[(i - 1) * 2] = pcm[i - 1];
        pcm[(i - 1) * 2 + 1] = pcm[i - 1];
    }
    *size = got * 2;
    return 0;
}

/* Legacy entry points cannot reconfigure or tear down a native media session. */
int i2s_configure(const struct device *d, enum i2s_dir dir, const struct i2s_config *c)
{
    if (!c)
        return -EINVAL;
    if (native_owner)
        return -EBUSY;
    int acquired = lock_legacy_hardware();
    if (acquired)
        return acquired;
    int r = native_owner ? -EBUSY : legacy_configure_locked(d, dir, c);
    unlock_hardware();
    return r;
}

int i2s_trigger(const struct device *d, enum i2s_dir dir, int trigger)
{
    if (native_owner)
        return -EBUSY;
    int acquired = lock_legacy_hardware();
    if (acquired)
        return acquired;
    int r = native_owner ? -EBUSY : legacy_trigger_locked(d, dir, trigger);
    unlock_hardware();
    return r;
}

int i2s_write(const struct device *d, void *block, size_t size)
{
    if (native_owner)
        return -EBUSY;
    int acquired = lock_legacy_hardware();
    if (acquired)
        return acquired;
    int r = native_owner ? -EBUSY : legacy_write_locked(d, block, size);
    unlock_hardware();
    return r;
}

int i2s_read(const struct device *d, void **block, size_t *size)
{
    if (native_owner)
        return -EBUSY;
    int acquired = lock_legacy_hardware();
    if (acquired)
        return acquired;
    int r = native_owner ? -EBUSY : legacy_read_locked(d, block, size);
    unlock_hardware();
    return r;
}

static int native_caps(void *ctx, aster_audio_output_caps_t *out)
{
    (void)ctx;
    out->formats = ASTER_AUDIO_PCM_BIT(ASTER_AUDIO_PCM_S16LE) |
                   ASTER_AUDIO_PCM_BIT(ASTER_AUDIO_PCM_S24LE) |
                   ASTER_AUDIO_PCM_BIT(ASTER_AUDIO_PCM_S32LE);
    out->rates = ASTER_AUDIO_RATE_32000 | ASTER_AUDIO_RATE_44100 | ASTER_AUDIO_RATE_48000 |
                 ASTER_AUDIO_RATE_88200 | ASTER_AUDIO_RATE_96000;
    out->channels = ASTER_AUDIO_CHANNEL_BIT(2);
    out->flags = ASTER_AUDIO_OUTPUT_EXCLUSIVE;
    out->max_write_frames = 512;
    return 0;
}

static int native_open(void *ctx, const aster_audio_format_t *format)
{
    (void)ctx;
    if (!lock_hardware())
        return -ENODEV;
    int r = 0;
    if (!ready)
        r = -ENODEV;
    else if (native_owner || tx || rx)
        r = -EBUSY;
    else {
        uint8_t code = 0;
        switch (format->rate) {
        case 32000:
            code = 0;
            break;
        case 44100:
            code = 1;
            break;
        case 48000:
            code = 2;
            break;
        case 88200:
            code = 3;
            break;
        case 96000:
            code = 4;
            break;
        default:
            r = -ENOTSUP;
        }
        if (!r)
            r = reg_write(0x20000, 0xf6);
        if (!r)
            r = reg_write(0x1000b, code);
        if (!r) {
            converted_capacity = 512 * 2 * sizeof(int32_t);
            converted = heap_caps_malloc(converted_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!converted)
                r = -ENOMEM;
        }
        if (!r) {
            i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(aster_cs43131_wiring.tx_port, I2S_ROLE_MASTER);
            channel.dma_desc_num = 8;
            channel.dma_frame_num = 128;
            channel.auto_clear_after_cb = true;
            esp_err_t status = i2s_new_channel(&channel, &tx, NULL);
            i2s_std_config_t standard = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(format->rate),
                                         .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                                             I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
                                         .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                                                      .bclk = aster_cs43131_wiring.bclk,
                                                      .ws = aster_cs43131_wiring.ws,
                                                      .dout = aster_cs43131_wiring.dout,
                                                      .din = I2S_GPIO_UNUSED}};
            standard.clk_cfg.clk_src = aster_cs43131_wiring.tx_clock;
            if (format->rate >= 88200 && standard.clk_cfg.clk_src == I2S_CLK_SRC_DEFAULT)
                standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
            if (status == ESP_OK)
                status = i2s_channel_init_std_mode(tx, &standard);
            r = error(status);
        }
        if (!r)
            r = codec_enable_locked();
        if (!r)
            r = codec_volume_locked((uint8_t)aster_plat_audio_get_volume());
        if (r) {
            shutdown();
            reg_write(0x20000, 0xf6);
        } else {
            native_owner = true;
            native_format = *format;
            physical_rate = format->rate;
            native_frames = 0;
            native_preroll_frames = 0;
            native_errors = 0;
        }
    }
    unlock_hardware();
    return r;
}

static int native_write(void *ctx, const void *pcm, uint32_t frames)
{
    (void)ctx;
    if (!lock_hardware())
        return -ENODEV;
    int r = !native_owner ? -EBADF
                          : aster_audio_pcm_to_s32(pcm, frames, &native_format, converted,
                                                   converted_capacity / sizeof(int32_t));
    if (!r) {
        /* Fade the first 5 ms only. The cursor counts accepted frames,
         * so partial writes do not incorrectly advance the ramp. */
        aster_audio_fade_in_s32_stereo(converted, frames, native_frames,
                                      native_format.rate / (1000u / NATIVE_START_FADE_MS));
        size_t bytes = (size_t)frames * 2 * sizeof(int32_t), wrote = 0;
        esp_err_t status = tx_on ? i2s_channel_write(tx, converted, bytes, &wrote, 1000)
                                 : i2s_channel_preload_data(tx, converted, bytes, &wrote);
        if (wrote % 8 || wrote > bytes) {
            r = -EIO;
        } else if (wrote) {
            if (!tx_on) {
                native_preroll_frames += (uint32_t)(wrote / 8);
                /* Preload can be called repeatedly while TX is READY.
                 * Start when enough samples are primed, or when DMA fills. */
                if (native_preroll_frames >= NATIVE_PREROLL_TARGET_FRAMES ||
                    wrote < bytes) {
                    status = i2s_channel_enable(tx);
                    if (status == ESP_OK)
                        tx_on = true;
                    else
                        r = error(status);
                }
            }
            if (!r) {
                /* Preloaded bytes are accepted even before clocks start. */
                r = (int)(wrote / 8);
                native_frames += (uint32_t)r;
            }
        } else {
            /* Avoid a zero-accept loop when disabled DMA storage is full. */
            if (!tx_on && native_preroll_frames && status == ESP_OK) {
                status = i2s_channel_enable(tx);
                if (status == ESP_OK)
                    tx_on = true;
            }
            r = status == ESP_OK ? -EAGAIN : error(status);
        }
    }
    if (r < 0 && r != -EAGAIN)
        native_errors++;
    unlock_hardware();
    return r;
}

static int native_drain(void *ctx)
{
    (void)ctx;
    if (!lock_hardware())
        return -ENODEV;
    int r = native_owner ? 0 : -EBADF;
    /* Extremely short files may finish before the second PCM block.
     * Start the pending preload before draining so they are audible. */
    if (!r && !tx_on && native_preroll_frames) {
        esp_err_t status = i2s_channel_enable(tx);
        if (status == ESP_OK)
            tx_on = true;
        else
            r = error(status);
    }
    if (!r && tx_on)
        k_msleep(8 * 128 * 1000 / physical_rate + 2);
    unlock_hardware();
    return r;
}

static int native_close(void *ctx)
{
    (void)ctx;
    if (!lock_hardware())
        return -ENODEV;
    int r = 0;
    if (native_owner) {
        /* Mute before stopping clocks. The next open restores the current volume. */
        r = reg_write(0x90003, 0xef);
        if (!r)
            r = reg_write(0x20000, 0xf6);
        /* A failed close retains ownership so the common contract can retry it. */
        if (!r) {
            shutdown();
            native_owner = false;
        }
    }
    unlock_hardware();
    return r;
}

static int native_stats(void *ctx, aster_audio_output_stats_t *out)
{
    (void)ctx;
    if (!lock_hardware())
        return -ENODEV;
    out->rate = native_format.rate;
    out->channels = native_format.channels;
    out->format = native_format.format;
    out->submitted_frames = native_frames;
    out->errors = native_errors;
    unlock_hardware();
    return 0;
}

static int native_register(void)
{
    static const aster_audio_output_ops_t ops = {.size = sizeof(ops),
                                                 .version = ASTER_AUDIO_OUTPUT_API_VERSION,
                                                 .name = "CS43131",
                                                 .caps = native_caps,
                                                 .open = native_open,
                                                 .write = native_write,
                                                 .drain = native_drain,
                                                 .close = native_close,
                                                 .stats = native_stats};
    return aster_audio_output_bind(ASTER_AUDIO_OUTPUT_WIRED, &ops, NULL);
}
