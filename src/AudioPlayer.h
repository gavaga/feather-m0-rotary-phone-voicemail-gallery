#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

#include <stdint.h>
#include <Adafruit_ZeroDMA.h>

class AudioPlayer_ {
public:
    static AudioPlayer_& getInstance();
    AudioPlayer_(AudioPlayer_&) = delete;

    bool init(uint32_t bits);

    bool is_playing() const { return _is_playing; }

    void start(uint32_t sample_rate, const int16_t *samples, uint32_t sample_count);
    bool ready();
    void enqueue(const int16_t *samples, uint32_t sample_count);
    void stop();

private:
    static void _static_dma_callback(Adafruit_ZeroDMA*);

    AudioPlayer_();
    bool _allocate_dac_dma();
    void _setup_dac_dma(const int16_t *samples, uint32_t num_samples);
    void _start_timer();

    void _handle_dma_callback(Adafruit_ZeroDMA*);
    
    Adafruit_ZeroDMA _dma_dac;
    DmacDescriptor *_dmac_dac_tx;

    volatile const int16_t* _audio_samples_ptr;
    volatile uint32_t _next_num_samples;
        
    volatile bool _stop_playing = false;
    volatile bool _is_playing = true;
};

extern AudioPlayer_ &AudioPlayer;

#endif // AUDIO_PLAYER_H_