#include <Arduino.h>
#include <sdios.h>

#include <Adafruit_NeoPixel.h>

#include <SPI.h>
#include <SdFat.h>

#include <Adafruit_ZeroDMA.h>

#include "io.h"
#include "WavePlayer.h"

#include "Dialer.h"

#include "AudioPlayer.h"

// DECLARATIONS
void fatal(const char *message, uint8_t r, uint8_t g, uint8_t b, uint16_t blink_delay);

void initSD();

void play(const char *filename);
void start_playing(const char *filename, bool loop);
bool tick();

// DEFINITIONS
#define CPU_HZ 48000000
#define SAMPLE_RATE 44100

#define GREEN_LED_BUILTIN 8

#define DAC_BITS 10

#define SD_CS_PIN 4
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(12))

#define NUM_SECTORS 4
#define SD_READ_CHUNK_SIZE (NUM_SECTORS * SD_SECTOR_SIZE)

#define DIALER_PIN A1

// GLOBALS
// Adafruit_NeoPixel neopixel_err(1, NEOPIXEL_BUILTIN, NEO_GRB + NEO_KHZ800);

WavePlayer player(SD_SECTOR_SIZE *NUM_SECTORS);

SdFs sd;
FsFile file;

int32_t dial_index = 0;
const char *dialtone_filename = "dialtone.wav";
const char *ring_filename = "ring.wav";
const char *ring_special = "ringremix.wav";

char number_filename[7] = "00.WAV";

// ------------------------------------------------------------------------------
void setup()
{
    // configure error LEDs
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(GREEN_LED_BUILTIN, OUTPUT);
    // neopixel_err.begin();
    // neopixel_err.clear();
    // neopixel_err.show();

    // configure serial
    Serial.begin(115200);
    // wait for serial monitor to connect
    while (!Serial) {
        yield();
    }

    initSD();

    if (!Dialer.init(DIALER_PIN))
    {
        fatal("FATAL: Failed to initialize Dialer", 255, 0, 0, 500);
    }

    // configure DAC
    if (!AudioPlayer.init(DAC_BITS)) {
        fatal("FATAL: Failed to initialize AudioPlayer", 255, 0, 0, 500);
    }

    // loop dialtone until interrupted by dialing
    start_playing(dialtone_filename, true);
}

void loop() {
    if (AudioPlayer.is_playing() && AudioPlayer.ready())
    {
        tick();
    }

    uint32_t dialed_number;
    if (Dialer.check_dialed(&dialed_number))
    {
        cout << F("Dialed: ") << dialed_number << endl;
        number_filename[dial_index++] = (char)('0' + (dialed_number % 10));

        AudioPlayer.stop();

        if (dial_index == 2)
        {
            dial_index = 0;
            start_playing(number_filename, false);
        }
    }
}

void play(const char *filename, bool loop)
{
    start_playing(filename, loop);

    while (AudioPlayer.is_playing())
    {
        // wait until ready for a tick
        while (!AudioPlayer.ready())
            yield();

        if (!tick())
        {
            cout << F("Finished playback") << endl;
            break;
        }
    }
}

void start_playing(const char *filename, bool loop)
{
    AudioPlayer.stop();

    cout << F("Playing file: ") << filename << endl;
    // open the file
    if (!file.open(filename, FILE_READ))
    {
        fatal("File doesn't exist", 255, 0, 0, 1000);
    }

    if (!player.init())
    {
        fatal("Error initializing waveplaer", 255, 0, 0, 500);
    }

    if (player.status() == WavePlayerStatus::ERROR)
    {
        fatal("Error initializing waveplayer", 255, 0, 0, 500);
    }

    cout << F("Initialized WavePlayer") << endl;

    int16_t *samples;
    uint32_t num_samples;
    if (!player.start(&sd, &file, loop, &samples, &num_samples))
    {
        fatal("Error starting wav file", 255, 0, 0, 500);
    }

    cout << F("Starting playback of ") << num_samples << F(" from ") << hex << samples << dec << endl;

    // enqueue the next audio playback
    AudioPlayer.start(SAMPLE_RATE, samples, num_samples);
}

bool tick()
{
    int16_t *samples;
    uint32_t num_samples;
    uint64_t time = micros();
    if (!player.read_and_convert(&samples, &num_samples))
    {
        return false;
    }
    time = micros() - time;
    // cout << F("Read ") << num_samples << F(" samples in ") << time << F(" us") << endl;

    // enqueue the next sample chunk and continue right into the next read
    AudioPlayer.enqueue(samples, num_samples);

    return num_samples > 0;
}

//------------------------------------------------------------------------------
void initSD()
{
    cout << F("SdFat version: ") << SD_FAT_VERSION_STR << endl;

    uint32_t t = millis();
    if (!sd.begin(SD_CONFIG))
    {
        cout << F(
            "\nSD initialization failed.\n"
            "Do not reformat the card!\n"
            "Is the card correctly inserted?\n"
            "Is there a wiring/soldering problem?\n");
        if (isSpi(SD_CONFIG))
        {
            cout << F(
                "Is SD_CS_PIN set to the correct value?\n"
                "Does another SPI device need to be disabled?\n");
        }
        fatal("SD card initialization failed", 255, 0, 0, 500);
    }
    t = millis() - t;
    cout << F("init time: ") << dec << t << " ms" << endl;

    Timeout timeout(1000000ul);
    while (!sd.volumeBegin())
    {
        if (timeout.timed_out())
        {
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

/** Neopixel-based fatal error handler.
 *
 * Print a message over serial then blink the neopixel a particular color.
 */
void fatal(const char *message, uint8_t r, uint8_t g, uint8_t b, uint16_t blink_delay_ms)
{
    Serial.println(message);

    for (bool ledState = HIGH;; ledState = !ledState)
    {
        digitalWrite(LED_BUILTIN, ledState);

        // neopixel_err.clear();
        // neopixel_err.setBrightness(ledState == HIGH ? 255 : 0);
        // neopixel_err.setPixelColor(0, neopixel_err.Color(r, g, b));
        // neopixel_err.show();

        delay(blink_delay_ms);
    }
}