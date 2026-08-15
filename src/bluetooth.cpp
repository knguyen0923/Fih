#include "bluetooth.h"
#include "pins.h"
#include "audio.h"
#include "motor.h"
#include <BluetoothA2DPSink.h>

static BluetoothA2DPSink a2dp_sink;

// Remembers the last-connected phone's Bluetooth address across a
// bluetoothStop()/bluetoothStart() cycle. BluetoothA2DPSink::end() always
// wipes its own internal "reconnect to last device" memory as part of its
// shutdown sequence (it's designed for "stop for good," not "pause for a
// few seconds"), so that mechanism can't be relied on here -- this project
// tracks the peer itself and reconnects to it explicitly via connect_to()
// instead of passively waiting for the phone to notice the speaker again.
static esp_bd_addr_t lastPeerAddr;
static bool hasLastPeer = false;

// Called by the A2DP library on its own internal FreeRTOS task whenever a
// new block of decoded audio is ready -- interleaved 16-bit stereo PCM at
// SAMPLE_RATE_BLUETOOTH. This runs on a different task than the main
// loop()/button state machine; that's safe here specifically because
// bluetoothStop() fully tears the A2DP sink down before any voice-mode I2S1
// or motor activity starts (see main.cpp's enterVoiceMode()/
// exitVoiceMode()), so this callback and the Gemini-TTS playback path are
// never live at the same time -- no locking needed between them.
static void onAudioData(const uint8_t* data, uint32_t len) {
    audioPlayFromBuffer(data, len);
    motorUpdateFromPcm(data, len);
}

void bluetoothStart() {
    audioSetPlaybackRate(SAMPLE_RATE_BLUETOOTH);
    // `false` -- don't let the library also drive its own I2S output;
    // audio.cpp already owns I2S1, so this just wants the raw PCM callback.
    a2dp_sink.set_stream_reader(onAudioData, false);
    a2dp_sink.start(BT_DEVICE_NAME);

    if (hasLastPeer) {
        // Proactively reconnect to the phone we were just talking to,
        // rather than passively waiting for it to notice the speaker is
        // available again -- meaningfully faster in practice, since phones
        // don't always re-scan/reconnect promptly on their own.
        a2dp_sink.connect_to(lastPeerAddr);
    }
}

void bluetoothStop() {
    if (a2dp_sink.is_connected()) {
        memcpy(lastPeerAddr, *a2dp_sink.get_current_peer_address(), sizeof(esp_bd_addr_t));
        hasLastPeer = true;
    }
    // release_memory=true -- fully disables the Bluetooth controller
    // (esp_bt_controller_disable(), not just the current A2DP connection).
    // The default (false) leaves the controller enabled, which would keep
    // contending with WiFi even while "disconnected" -- this is what
    // actually frees the radio, which the whole point of this function is
    // to do (see main.cpp). Confirmed by reading the library's own source
    // (.pio/libdeps/esp32/ESP32-A2DP/src/BluetoothA2DPCommon.cpp) rather
    // than assumed -- the full teardown this triggers costs real time (on
    // the order of seconds, per that same source), which is why main.cpp
    // plays a short tone right after this returns: an otherwise-silent
    // multi-second gap reads as the device being broken, not as "listening
    // now."
    a2dp_sink.end(true);
}
