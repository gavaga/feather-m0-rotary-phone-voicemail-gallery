#ifndef WAVEPLAYER_H_
#define WAVEPLAYER_H_

#include <stdint.h>
#include <stddef.h>
#include <Adafruit_ZeroDMA.h>
#include <SPI.h>
#include <SdFat.h>

#ifndef USE_DMA
#define USE_DMA 0
#endif

#define SD_SECTOR_SIZE 512

enum class WavePlayerStatus {
    NOTINITIALIZED = 0,
    READY,
    ERROR
};

class Timeout { 
public:
    Timeout(uint64_t delay) : _delay(delay) {
        reset();
    }

    void reset() {
        _start_time = micros();
        _due_time = _start_time + _delay;
    }

    bool timed_out() {
        return micros() >= _due_time;
    }
private:
    uint64_t _delay, _start_time, _due_time;
};

/**
 * @brief DMA-based Wave file player using SdFat raw sector volume APIs and Adafruit_ZeroDMA to read from the Sd card.
 * 
 * Usage:
 * 1. instantiate with some buffer size. This will allocate double that size since we double-buffer
 * WavePlayer player(1024);
 * 
 * file.open("00.wav", FILE_READ);
 * player.open(&file);
 * 
 * when PLAY completes a chunk
 *      - swap buffers, start the next chunk playing
 *      - call start_next_read()
 *      - call CONVERT on the most recently read chunk
 *      - IDLE until PLAY completes this chunk
 */
class WavePlayer {
public:
    WavePlayer(size_t buffer_size);
    ~WavePlayer();

    WavePlayerStatus status() { return _status; };

    bool init();

    // load a WAV and load its first chunk of data
    bool start(SdFs* sd, FsFile* file, int16_t **samples, uint32_t *num_samples);

    /** Do a DMA-based read and simultaneous conversion of the next chunk of data. */
    bool read_and_convert(int16_t **samples, uint32_t *num_samples);

#if USE_DMA
    /** Check if this player owns a specific DMA channel */
    bool owns_dma(Adafruit_ZeroDMA* dma) {
        return dma == &dma_tx || dma == &dma_rx;
    }

    void player_dma_callback();
#endif
private:
#if USE_DMA
    bool _setup_dma();
    void _free_dma();
#endif

    void _swap_buffers();
    /** Find the next contiguous set of at most _max_sectors */
    void _get_next_chunk(uint32_t *sector_out, uint32_t *num_sectors_out);
    void _convert(uint32_t offset, uint32_t count);
    bool _start_read_chunk(uint32_t sector, uint32_t ns);
    void _end_read_chunk();

    uint8_t _id;
    WavePlayerStatus _status = WavePlayerStatus::NOTINITIALIZED;
    uint8_t _max_sectors;
    
    volatile uint8_t _sectors_to_read;
    // # of sectors read in the current DMA transaction
    volatile uint8_t _num_sectors_read;

    SdFs *_sd;
    // current file
    FsFile *_file;
    // file is contiguous
    bool _contiguous;
    // current file position (in sectors)
    int32_t _sector_index;

#if USE_DMA
    /* Wave player uses 2 DMA channels, one for TX and one for RX */
    Adafruit_ZeroDMA dma_tx, dma_rx;

    /* 1 TX descriptor with size SECTOR SIZE + 4 */
    DmacDescriptor *desc_tx;

    /* 3 RX descriptors in a chain/loop 
        0 - 2-byte sector header writing into _dma_tmp_buf
        1 - 512-byte sector writing into the _dma_rx_buf
        2 - 2-byte CRC writing into _dma_tmp_buf
    */
    DmacDescriptor *desc_rx[3];
#endif

    /** 
     * malloc-based RX buffers (actually 1 continguous buffer with an extra pointer)
     * 
     * we ALWAYS use _dma_rx_buf[0] for load + convert, and _dma_rx_buf[1] for playback
     * 0 - Load + Convert
     * 2 - Play
     * */
    uint8_t *_dma_rx_bufs[2];
    size_t _dma_rx_buf_size;

    /* 1-byte TX buf */
    uint8_t *_dma_tx_buf;
    /* 2-byte tmp DMA buffer */
    uint8_t *_dma_tmp_buf;
    /* full RX buf allocaton (double the buf size) */
    uint8_t *_dma_rx_buf;
};

#endif // WAVEPLAYER_H_