#include <Arduino.h>
#include <sdios.h>

#include <Adafruit_NeoPixel.h>

#include <SPI.h>
#include <SdFat.h>

#include <Adafruit_ZeroDMA.h>

#include "io.h"
#include "WavePlayer.h"

#include "Dialer.h"

// DECLARATIONS
void fatal(const char *message, uint8_t r, uint8_t g, uint8_t b, uint16_t blink_delay);

void initSD();

void allocate_dac_dma();
void setup_dac_dma();
void startTimer(int frequencyHz);
void dma_dac_callback(Adafruit_ZeroDMA *dma);

void start_playing(const char *filename, bool loop);
bool tick();
bool should_tick();
void play(const char *filename);

// DEFINITIONS
#define CPU_HZ        48000000
#define SAMPLE_RATE   44100

#define GREEN_LED_BUILTIN 8

#define DAC_BITS      10
#define DAC_CENTER    512
#define DAC_GAIN      8

#define SD_CS_PIN  4
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(12))

#define NUM_SECTORS         4
#define SD_READ_CHUNK_SIZE  (NUM_SECTORS * SD_SECTOR_SIZE)

#define DIALER_PIN A1

// GLOBALS 
// Adafruit_NeoPixel neopixel_err(1, NEOPIXEL_BUILTIN, NEO_GRB + NEO_KHZ800);

WavePlayer player(SD_SECTOR_SIZE * NUM_SECTORS);

SdFs sd;
FsFile file;

Adafruit_ZeroDMA dma_dac;
DmacDescriptor *dmac_dac_tx;

Adafruit_ZeroDMA dma_tx, dma_rx;
ZeroDMAstatus dma_status;
DmacDescriptor *dmac_sd_sector_tx;
DmacDescriptor *dmac_sd_sector_rx[3];

uint8_t dma_buffer_tx[1] __attribute__ ((aligned (32)));
uint8_t dma_buffer_rx[SD_READ_CHUNK_SIZE] __attribute__ ((aligned (32)));
uint8_t dma_buffer_rx_tmp[2] __attribute__ ((aligned (32)));
int16_t pcm_buffer[NUM_SECTORS * (SD_SECTOR_SIZE / 2)] __attribute__ ((aligned (32)));

int32_t dial_index = 0;
const char *dialtone_filename = "dialtone.wav";
const char *ring_filename = "ring.wav";
const char *ring_special = "ringremix.wav";

char number_filename[7] = "00.WAV";

volatile int16_t* audio_samples_ptr;
volatile uint32_t next_num_samples;

volatile uint64_t last_block_time = 0, last_block_duration = 0;
volatile bool rx_is_done = false, tx_is_done = false;
volatile uint8_t num_rx_blocks = 0;
volatile uint8_t dac_block_idx = 0;

volatile bool stop_playing = false;
volatile bool is_playing = true;

// ------------------------------------------------------------------------------
void setup() {
  // configure error LEDs
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(GREEN_LED_BUILTIN, OUTPUT);
  // neopixel_err.begin();
  // neopixel_err.clear();
  // neopixel_err.show();

  Dialer.init(DIALER_PIN);

  // configure serial
  Serial.begin(115200);
  // wait for serial monitor to connect
  while (!Serial) {
    yield();
  }

  // configure DAC
  analogWriteResolution(DAC_BITS);
  initSD();
  allocate_dac_dma();

  // loop dialtone until interrupted by dialing
  start_playing(dialtone_filename, true);
}

void loop() {
  if (is_playing && should_tick()) {
    tick();
  }

  uint32_t dialed_number;
  if (Dialer.check_dialed(&dialed_number)) {
    cout << F("Dialed: ") << dialed_number << endl;
    number_filename[dial_index++] = (char)('0' + (dialed_number % 10));
    
    // interrupt any current playback
    stop_playing = true;

    if (dial_index == 2) {
      dial_index = 0;
      start_playing(number_filename, false);
    }
  }
}

void play(const char *filename, bool loop) {
  stop_playing = false;

  start_playing(filename, loop);

  while (true) {
    // wait until ready for a tick
    while(!should_tick()) yield();

    if (!tick()) {
      cout << F("Finished playback") << endl;
      break;
    }
    
    // DMA will stop automatically if we don't feed it a new command
    if (stop_playing) {
      cout << F("Interrupted playback") << endl;
      break;
    }
  }
}

void start_playing(const char *filename, bool loop) {
  stop_playing = false;

  cout << F("Playing file: ") << filename << endl;
  // open the file
  if (!file.open(filename, FILE_READ)) {
    fatal("File doesn't exist", 255, 0, 0, 1000);
  }

  if (!player.init()) {
    fatal("Error initializing waveplaer", 255, 0, 0, 500);
  }

  if (player.status() == WavePlayerStatus::ERROR) {
    fatal("Error initializing waveplayer", 255, 0, 0, 500);
  }

  cout << F("Initialized WavePlayer") << endl;

  int16_t *samples;
  uint32_t num_samples; 
  if (!player.start(&sd, &file, loop, &samples, &num_samples)) {
    fatal("Error starting wav file", 255, 0, 0, 500);
  }

  cout << F("Starting playback of ") << num_samples << F(" from ") << hex << samples << dec << endl;

  // enqueue the next audio playback
  audio_samples_ptr = samples;
  next_num_samples = num_samples;
  
  startTimer(SAMPLE_RATE);
  setup_dac_dma();

  // actually start playback
  dma_dac.startJob();

  is_playing = true;
}

bool tick() {
  if (stop_playing) {
    dma_dac.abort();
    return false;
  }

  int16_t *samples;
  uint32_t num_samples; 
  uint64_t time = micros();
  if (!player.read_and_convert(&samples, &num_samples)) {
    return false;
  }
  time = micros() - time;
  //cout << F("Read ") << num_samples << F(" samples in ") << time << F(" us") << endl;

  // enqueue the next sample chunk and continue right into the next read
  noInterrupts();
  audio_samples_ptr = samples;
  next_num_samples = num_samples;
  interrupts();

  return num_samples > 0;
}

bool should_tick() {
  // wait for this command to be latched before swapping buffers and moving onto the next read 
  return next_num_samples == 0;
}

//------------------------------------------------------------------------------
void initSD() {
  cout << F("SdFat version: ") << SD_FAT_VERSION_STR << endl;

  uint32_t t = millis();
  if (!sd.begin(SD_CONFIG)) {
    cout << F(
           "\nSD initialization failed.\n"
           "Do not reformat the card!\n"
           "Is the card correctly inserted?\n"
           "Is there a wiring/soldering problem?\n");
    if (isSpi(SD_CONFIG)) {
      cout << F(
           "Is SD_CS_PIN set to the correct value?\n"
           "Does another SPI device need to be disabled?\n"
           );
    }
    fatal("SD card initialization failed", 255, 0, 0, 500); 
  }
  t = millis() - t;
  cout << F("init time: ") << dec << t << " ms" << endl;

  Timeout timeout(1000000ul);
  while (!sd.volumeBegin()) {
    if (timeout.timed_out()) {
      cout << F("Failed to initialize SD Volume") << endl;
      fatal("Volume Begin", 255, 0, 0, 250);
    }

    delay(50);
  }
  
  // file = sd.open(filename, FILE_READ);
  // if (!file) {
  //   cout << F("\nNo such file: ") << filename << endl; 
  //   fatal("File not found", 0, 0, 255, 500);
  // }
}

void setup_dac_dma() {
  dma_dac.abort();

  dma_dac.changeDescriptor(dmac_dac_tx,
    (void*)audio_samples_ptr,
    (void*)(&DAC->DATA.reg),
    next_num_samples);
}

void allocate_dac_dma() {
  while (DAC->STATUS.bit.SYNCBUSY == 1);
  DAC->DATA.reg = 0;
  DAC->CTRLA.bit.ENABLE = 1;
  while (DAC->STATUS.bit.SYNCBUSY == 1);
  
  dma_status = dma_dac.allocate();
  if (dma_status != DMA_STATUS_OK) {
    cout << F("FATAL: Couldn't allocate DMA, status: ") << dma_status << endl;
    fatal("dma.allocate", 255, 0, 0, 500);
  }

  dma_dac.setTrigger(TC5_DMAC_ID_OVF);
  dma_dac.setAction(DMA_TRIGGER_ACTON_BEAT);

  dmac_dac_tx = dma_dac.addDescriptor(
    (void*)(audio_samples_ptr),
    (void*)(&DAC->DATA.reg),
    next_num_samples,
    DMA_BEAT_SIZE_HWORD,
    true,
    false);
  dmac_dac_tx->BTCTRL.reg |= DMAC_BTCTRL_BLOCKACT_INT;
  if (!dmac_dac_tx) {
    fatal("dma.addDescriptor", 255, 0, 0, 500);
  }
  
  dma_dac.loop(true);

  dma_dac.setCallback(dma_dac_callback);
}

void setTimerFrequency(int frequencyHz) {
  uint16_t compareValue = (uint16_t) (CPU_HZ / frequencyHz) - 1;
  TcCount16* TC = (TcCount16*) TC5;
  // Make sure the count is in a proportional position to where it was
  // to prevent any jitter or disconnect when changing the compare value.
  TC->COUNT.reg = map(TC->COUNT.reg, 0, TC->CC[0].reg, 0, compareValue);
  TC->CC[0].reg = compareValue;
  Serial.println(TC->COUNT.reg);
  Serial.println(TC->CC[0].reg);
  while (TC->STATUS.bit.SYNCBUSY == 1);
}

void startTimer(int frequencyHz) {
  REG_GCLK_CLKCTRL = (uint16_t) (GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_ID(GCM_TC4_TC5));
  while (GCLK->STATUS.bit.SYNCBUSY == 1);

  TcCount16* TC = (TcCount16*) TC5;

  // reset the timer
  TC->CTRLA.reg = TC_CTRLA_SWRST;
  while (TC->STATUS.bit.SYNCBUSY == 1);
  while (TC->CTRLA.bit.SWRST);

  // Use the 16-bit timer
  TC->CTRLA.reg |= TC_CTRLA_MODE_COUNT16;
  // Use match mode so that the timer counter resets when the count matches the compare register
  TC->CTRLA.reg |= TC_CTRLA_WAVEGEN_MFRQ;
  // Set the prescaler to 1024
  TC->CTRLA.reg |= TC_CTRLA_PRESCALER_DIV1; //024;
  while (TC->STATUS.bit.SYNCBUSY == 1);

  setTimerFrequency(frequencyHz);

  // Enable the compare interrupt
  TC->INTENSET.reg = 0;
  TC->INTENSET.bit.MC0 = 1;

  TC->CTRLA.reg |= TC_CTRLA_ENABLE;
  while (TC->STATUS.bit.SYNCBUSY == 1);
}

void dma_dac_callback(Adafruit_ZeroDMA *dma) {
  (void) dma;

  // take the next command
  uint32_t num_samples;
  int16_t *samples;
  noInterrupts();
  num_samples = next_num_samples;
  samples = (int16_t*)audio_samples_ptr;
  audio_samples_ptr = 0;
  next_num_samples = 0;
  interrupts();

  dma_dac.abort();

  // actually stop playback if there are no more samples to play
  if (num_samples == 0) {
    is_playing = false;
    return;
  }

  // otherwise enqueue the next set of samples 
  dmac_dac_tx->SRCADDR.bit.SRCADDR = (uint32_t)(samples + num_samples);
  dmac_dac_tx->BTCNT.bit.BTCNT = num_samples;

  dma_dac.startJob();
}

/** Neopixel-based fatal error handler. 
 * 
 * Print a message over serial then blink the neopixel a particular color.
 */
void fatal(const char* message, uint8_t r, uint8_t g, uint8_t b, uint16_t blink_delay_ms) {
  Serial.println(message);

  for (bool ledState = HIGH;; ledState = !ledState) {
    digitalWrite(LED_BUILTIN, ledState);

    // neopixel_err.clear();
    // neopixel_err.setBrightness(ledState == HIGH ? 255 : 0);
    // neopixel_err.setPixelColor(0, neopixel_err.Color(r, g, b));
    // neopixel_err.show();

    delay(blink_delay_ms);
  }
}