/*
  ESP32 I2S → UDP example (32‑bit, 44.1 kHz)
  Streams 32‑bit I2S samples (1024 samples ≈ 4096 bytes) via UDP,
  no response required.
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>

// Wi‑Fi credentials
const char* WIFI_SSID = "One Pound Fish";
const char* WIFI_PASS = "PineapplewithacapitolP";

// your server IP and UDP port
const char* SERVER_IP   = "192.168.1.89";
const uint16_t SERVER_PORT = 5003;

// I2S pins and port
#define I2S_WS   19
#define I2S_SD   23
#define I2S_SCK  18
#define I2S_PORT I2S_NUM_0

// samples per packet → 1024 samples at 44.1 kHz = ~23.2 ms, 4 bytes each = 4096 bytes
#define BUFFER_SAMPLES 1024
int32_t sBuffer[BUFFER_SAMPLES];

WiFiUDP udp;

// ----------------------------------------------------------------------------
void i2s_install() {
  i2s_config_t cfg = {
    .mode               = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate        = 44100,
    .bits_per_sample    = i2s_bits_per_sample_t(I2S_BITS_PER_SAMPLE_32BIT),
    .channel_format     = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags   = 0,
    .dma_buf_count      = 16,              // increased headroom
    .dma_buf_len        = BUFFER_SAMPLES,  // must match packet size
    .use_apll           = false
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
}

void i2s_setpin() {
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = -1,
    .data_in_num  = I2S_SD
  };
  i2s_set_pin(I2S_PORT, &pins);
}

// ----------------------------------------------------------------------------
void audioSenderSetup() {
  Serial.begin(115200);
  delay(500);

  // Connect to Wi‑Fi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi‑Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println(" connected!");
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

  // Start UDP
  udp.begin(WiFi.localIP(), SERVER_PORT);

  // Start I2S
  i2s_install();
  i2s_setpin();
  i2s_start(I2S_PORT);
}

// ----------------------------------------------------------------------------
void audioSenderLoop() {
  // Read one I2S buffer
  size_t bytesRead = 0;
  if (i2s_read(I2S_PORT, sBuffer, sizeof(sBuffer), &bytesRead, portMAX_DELAY) != ESP_OK
      || bytesRead == 0) {
    Serial.println("I2S read error");
    return;
  }

  // Send via UDP (fire-and-forget)
  unsigned long t0 = millis();
  udp.beginPacket(SERVER_IP, SERVER_PORT);
  udp.write((uint8_t*)sBuffer, bytesRead);
  udp.endPacket();
  unsigned long dt = millis() - t0;

  // Log size & send time
  Serial.printf("Sent %u bytes (~%.1f ms) in %lums\n",
                unsigned(bytesRead),
                bytesRead / 4.0f / 44100.0f * 1000.0f,
                dt);
}
