#include "audio.h"
#include "pins.h"
#include <driver/i2s.h>
#include <math.h>

// INMP441 (and most I2S MEMS mics) deliver 24 significant bits of audio,
// left-justified inside a 32-bit I2S frame (i.e. the real data lives in the
// upper bits, with zeros/garbage in the low bits). Shifting right by this
// much both discards the padding bits and scales the result down to a
// reasonable 16-bit amplitude range.
//
// This value is a starting point, not a precisely derived constant — if
// Build Phase 1 (loopback) testing shows the recording is too quiet, decrease
// this shift (keeps more of the signal); if it's clipping/distorted, increase
// it (attenuates the signal more).
static constexpr int MIC_GAIN_SHIFT = 14;

// Sets up I2S0 as a receive-only peripheral reading from the microphone.
static void configureMicI2S() {
    i2s_config_t cfg = {
        // MASTER means the ESP32 generates the clock signals (BCLK/WS) itself
        // rather than expecting an external device to drive them.
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE_CAPTURE,
        // The mic outputs 32-bit frames even though only 24 bits are
        // meaningful — see MIC_GAIN_SHIFT above for how we account for that.
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        // INMP441 breakouts are typically wired to output on the left channel
        // only (tied via their L/R pin); we only need one channel for mono
        // speech capture anyway.
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        // DMA (Direct Memory Access) buffers let the I2S hardware fill memory
        // in the background without the CPU babysitting every sample.
        // 4 buffers x 256 samples is a modest, commonly-used starting point —
        // more buffers/longer buffers trade a bit of latency for more
        // tolerance against briefly-busy code elsewhere in loop().
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,          // the internal audio PLL clock isn't needed at these modest rates
        .tx_desc_auto_clear = false, // only relevant for TX; irrelevant here since this is RX-only
        .fixed_mclk = 0
    };
    i2s_pin_config_t pinCfg = {
        .bck_io_num = PIN_MIC_BCLK,
        .ws_io_num = PIN_MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE, // this peripheral never transmits
        .data_in_num = PIN_MIC_DATA
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pinCfg);
}

// Sets up I2S1 as a transmit-only peripheral driving the amplifier. Starts
// at SAMPLE_RATE_BLUETOOTH since Bluetooth speaker mode is this device's
// default idle state -- see audioSetPlaybackRate() for switching to
// SAMPLE_RATE_PLAYBACK during a Gemini TTS reply.
static void configureAmpI2S() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE_BLUETOOTH,
        // Gemini's TTS output is already 16-bit PCM, so no bit-depth
        // conversion is needed on this side (unlike the mic's 32->16 shift).
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        // True stereo now (2 MAX98357A boards, one per channel) -- Bluetooth
        // audio is natively stereo; mono Gemini TTS audio gets upmixed to
        // stereo before reaching I2S1 (see audioPlayMonoAsStereo()).
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        // Automatically zero out any leftover DMA buffer content between
        // writes, so silence plays as true silence instead of stale samples.
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pinCfg = {
        .bck_io_num = PIN_AMP_BCLK,
        .ws_io_num = PIN_AMP_WS,
        .data_out_num = PIN_AMP_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE // this peripheral never receives
    };
    i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &pinCfg);
}

void audioInit() {
    configureMicI2S();
    configureAmpI2S();
}

size_t audioReadChunk(uint8_t* buf, size_t chunkBytes) {
    const size_t samplesWanted = chunkBytes / sizeof(int16_t);
    int16_t* out = (int16_t*)buf;
    size_t samplesFilled = 0;

    // Scratch buffer for the raw 32-bit samples coming off the wire, before
    // they get shifted down to 16-bit. Sized to something small and fixed
    // (stack-allocated) rather than proportional to chunkBytes, since we loop
    // over it as many times as needed.
    int32_t rawBuf[128];
    while (samplesFilled < samplesWanted) {
        size_t samplesThisRead = min(samplesWanted - samplesFilled, (size_t)128);
        size_t bytesRead = 0;
        // portMAX_DELAY means "block indefinitely until this many bytes are
        // available" — fine here since recording is meant to block anyway.
        i2s_read(I2S_NUM_0, rawBuf, samplesThisRead * sizeof(int32_t), &bytesRead, portMAX_DELAY);
        size_t samplesRead = bytesRead / sizeof(int32_t);
        if (samplesRead == 0) break; // avoid spinning forever on an I2S error

        // Downconvert each raw 32-bit mic sample to a 16-bit PCM sample.
        for (size_t i = 0; i < samplesRead; i++) {
            out[samplesFilled + i] = (int16_t)(rawBuf[i] >> MIC_GAIN_SHIFT);
        }
        samplesFilled += samplesRead;
    }

    return samplesFilled * sizeof(int16_t);
}

void audioPlayFromBuffer(const uint8_t* pcm, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        // Write in bounded chunks rather than the whole buffer in one call —
        // i2s_write() may not accept an arbitrarily large buffer in a single
        // call, so this loop feeds it piece by piece until everything's sent.
        size_t chunk = min(len - offset, (size_t)2048);
        size_t written = 0;
        i2s_write(I2S_NUM_1, pcm + offset, chunk, &written, portMAX_DELAY);
        if (written == 0) break; // avoid spinning forever on an I2S error
        offset += written;
    }
}

void audioSetPlaybackRate(uint32_t sampleRate) {
    // Changes the sample rate on the fly -- channel format (stereo) stays
    // fixed, so this is far cheaper than a full i2s_driver_uninstall() +
    // configureAmpI2S() reinstall every time playback switches between
    // Bluetooth (SAMPLE_RATE_BLUETOOTH) and a Gemini TTS reply
    // (SAMPLE_RATE_PLAYBACK).
    i2s_set_sample_rates(I2S_NUM_1, sampleRate);
}

void audioPlayMonoAsStereo(const uint8_t* monoPcm, size_t len) {
    const int16_t* monoSamples = (const int16_t*)monoPcm;
    size_t sampleCount = len / sizeof(int16_t);

    // Small fixed-size stack buffer, processed in chunks -- same reasoning
    // as audioReadChunk()'s rawBuf, avoids needing an allocation sized to
    // the caller's chunk length.
    int16_t stereoBuf[256]; // 128 upmixed L+R sample-pairs per chunk
    const size_t samplesPerChunk = 128;

    size_t offset = 0;
    while (offset < sampleCount) {
        size_t n = min(sampleCount - offset, samplesPerChunk);
        for (size_t i = 0; i < n; i++) {
            stereoBuf[i * 2] = monoSamples[offset + i];     // left
            stereoBuf[i * 2 + 1] = monoSamples[offset + i]; // right -- same sample, upmixed mono
        }
        audioPlayFromBuffer((const uint8_t*)stereoBuf, n * 2 * sizeof(int16_t));
        offset += n;
    }
}

void audioPlayTone(uint32_t sampleRate, uint32_t freqHz, uint32_t durationMs) {
    const size_t totalSamples = (size_t)((uint64_t)sampleRate * durationMs / 1000);
    const size_t samplesPerChunk = 128;
    int16_t stereoBuf[samplesPerChunk * 2];

    size_t samplesDone = 0;
    while (samplesDone < totalSamples) {
        size_t n = min(totalSamples - samplesDone, samplesPerChunk);
        for (size_t i = 0; i < n; i++) {
            size_t sampleIndex = samplesDone + i;
            float phase = 2.0f * PI * freqHz * ((float)sampleIndex / (float)sampleRate);
            // Moderate amplitude (not full-scale) -- this is a gentle UI
            // cue, not an alarm.
            int16_t sample = (int16_t)(sinf(phase) * 8000.0f);
            stereoBuf[i * 2] = sample;
            stereoBuf[i * 2 + 1] = sample;
        }
        audioPlayFromBuffer((const uint8_t*)stereoBuf, n * 2 * sizeof(int16_t));
        samplesDone += n;
    }
}
