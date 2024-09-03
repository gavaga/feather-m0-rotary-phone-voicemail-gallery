#include <Arduino.h>
#include <Adafruit_ZeroDMA.h>

#include "io.h"
#include "AudioPlayer.h"

#define CPU_HZ 48000000

static void setTimerFrequency(int frequencyHz);
static void startTimer(int frequencyHz);

AudioPlayer_ &AudioPlayer_::getInstance()
{
    static AudioPlayer_ instance;
    return instance;
}

void AudioPlayer_::_static_dma_callback(Adafruit_ZeroDMA *dma)
{
    AudioPlayer._handle_dma_callback(dma);
}

AudioPlayer_::AudioPlayer_() {}

bool AudioPlayer_::init(uint32_t bits)
{
    analogWriteResolution(bits);

    if (!_allocate_dac_dma())
    {
        cout << F("AudioPlayer: Failed to allocate DAC DMA, aborting") << endl;
        return false;
    }

    cout << F("AudioPlayer initialized") << endl;
    return true;
}

bool AudioPlayer_::_allocate_dac_dma()
{
    while (DAC->STATUS.bit.SYNCBUSY == 1);
    DAC->DATA.reg = 0;
    DAC->CTRLA.bit.ENABLE = 1;
    while (DAC->STATUS.bit.SYNCBUSY == 1);

    ZeroDMAstatus dma_status = _dma_dac.allocate();
    
    if (dma_status != DMA_STATUS_OK) {
        cout << F("FATAL: Couldn't allocate DMA, status: ") << dma_status << endl;
        return false;
    }

    _dma_dac.setTrigger(TC5_DMAC_ID_OVF);
    _dma_dac.setAction(DMA_TRIGGER_ACTON_BEAT);
    
    _dmac_dac_tx = _dma_dac.addDescriptor(
        NULL,
        (void *)(&DAC->DATA.reg),
        0,
        DMA_BEAT_SIZE_HWORD,
        true,
        false);
    _dmac_dac_tx->BTCTRL.reg |= DMAC_BTCTRL_BLOCKACT_INT;
    if (!_dmac_dac_tx) {
        cout << F("FATAL: Failed to add DMA descriptor") << endl;
        return false;
    }

    _dma_dac.loop(true);
    _dma_dac.setCallback(AudioPlayer_::_static_dma_callback);

    return true;
}

void AudioPlayer_::_setup_dac_dma(const int16_t *samples, uint32_t num_samples)
{
    _dma_dac.abort();

    _dma_dac.changeDescriptor(
        _dmac_dac_tx,
        (void *)samples,
        (void *)(&DAC->DATA.reg),
        num_samples);
}

void AudioPlayer_::start(uint32_t sample_rate, const int16_t *samples, uint32_t num_samples)
{
    cout << F("AudioPlayer: Starting playback at sample rate ") << sample_rate << F("Hz") << endl;

    startTimer(sample_rate);
    _setup_dac_dma((const int16_t *)samples, num_samples);

    _next_num_samples = 0;
    _audio_samples_ptr = NULL;
    _stop_playing = false;
    _is_playing = true;

    _dma_dac.startJob();
}

bool AudioPlayer_::ready()
{
    return _is_playing && !_stop_playing && _next_num_samples == 0;
}

void AudioPlayer_::enqueue(const int16_t *samples, uint32_t num_samples)
{
    // do nothing if we've been told to stop playing
    if (_stop_playing)
        return;

    noInterrupts();
    _audio_samples_ptr = samples;
    _next_num_samples = num_samples;
    interrupts();
}

void AudioPlayer_::stop() {
    _stop_playing = true;
    _is_playing = false;
    _dma_dac.abort();
}

void AudioPlayer_::_handle_dma_callback(Adafruit_ZeroDMA *dma)
{
    (void)dma;

    // take the next command
    uint32_t num_samples;
    int16_t *samples;
    noInterrupts();
    num_samples = _next_num_samples;
    samples = (int16_t *)_audio_samples_ptr;
    _audio_samples_ptr = 0;
    _next_num_samples = 0;
    interrupts();

    _dma_dac.abort();

    // actually stop playback if there are no more samples to play
    if (num_samples == 0)
    {
        _is_playing = false;
        return;
    }

    // otherwise enqueue the next set of samples
    _dmac_dac_tx->SRCADDR.bit.SRCADDR = (uint32_t)(samples + num_samples);
    _dmac_dac_tx->BTCNT.bit.BTCNT = num_samples;

    _dma_dac.startJob();
}

AudioPlayer_ &AudioPlayer = AudioPlayer_::getInstance();

static void setTimerFrequency(int frequencyHz)
{
    uint16_t compareValue = (uint16_t)(CPU_HZ / frequencyHz) - 1;
    TcCount16 *TC = (TcCount16 *)TC5;
    // Make sure the count is in a proportional position to where it was
    // to prevent any jitter or disconnect when changing the compare value.
    TC->COUNT.reg = map(TC->COUNT.reg, 0, TC->CC[0].reg, 0, compareValue);
    TC->CC[0].reg = compareValue;
    Serial.println(TC->COUNT.reg);
    Serial.println(TC->CC[0].reg);
    while (TC->STATUS.bit.SYNCBUSY == 1)
        ;
}

static void startTimer(int frequencyHz)
{
    REG_GCLK_CLKCTRL = (uint16_t)(GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_ID(GCM_TC4_TC5));
    while (GCLK->STATUS.bit.SYNCBUSY == 1)
        ;

    TcCount16 *TC = (TcCount16 *)TC5;

    // reset the timer
    TC->CTRLA.reg = TC_CTRLA_SWRST;
    while (TC->STATUS.bit.SYNCBUSY == 1)
        ;
    while (TC->CTRLA.bit.SWRST)
        ;

    // Use the 16-bit timer
    TC->CTRLA.reg |= TC_CTRLA_MODE_COUNT16;
    // Use match mode so that the timer counter resets when the count matches the compare register
    TC->CTRLA.reg |= TC_CTRLA_WAVEGEN_MFRQ;
    // Set the prescaler to 1024
    TC->CTRLA.reg |= TC_CTRLA_PRESCALER_DIV1; // 024;
    while (TC->STATUS.bit.SYNCBUSY == 1)
        ;

    setTimerFrequency(frequencyHz);

    // Enable the compare interrupt
    TC->INTENSET.reg = 0;
    TC->INTENSET.bit.MC0 = 1;

    TC->CTRLA.reg |= TC_CTRLA_ENABLE;
    while (TC->STATUS.bit.SYNCBUSY == 1)
        ;
}