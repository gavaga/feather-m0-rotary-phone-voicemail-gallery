#include <io.h>

#include "WavePlayer.h"

static bool wait_for_sector_start();
static uint8_t spiReceive();
static void wav_tx_dma_callback(Adafruit_ZeroDMA* dma);
static void wav_rx_dma_callback(Adafruit_ZeroDMA* dma);

volatile uint8_t num_dma_callbacks = 0;

static size_t active_player_count = 0;
static WavePlayer* active_players[16];

WavePlayer::WavePlayer(size_t buffer_size)
    : _dma_rx_buf_size(buffer_size)
{
    // buffer size MUST be a multiple of the SD_SECTOR_SIZE (in bytes)
    if (buffer_size % SD_SECTOR_SIZE != 0) {
        _status = WavePlayerStatus::ERROR;
        return;
    }

    _max_sectors = buffer_size / SD_SECTOR_SIZE;

    _dma_rx_buf = (uint8_t*)malloc(buffer_size * 2);
    _dma_tmp_buf = (uint8_t*)malloc(2);

    _dma_tx_buf = (uint8_t*)malloc(1);
    _dma_tx_buf[0] = 0xFF;

    _dma_rx_bufs[0] = _dma_rx_buf;
    _dma_rx_bufs[1] = _dma_rx_buf + buffer_size;

    // register the active player so we can map its DMA callback
    active_players[_id = active_player_count++] = this;
}

WavePlayer::~WavePlayer() {
    free(_dma_tx_buf);
    free(_dma_tmp_buf);
    free(_dma_rx_buf);

#if USE_DMA
    _free_dma();
#endif

    // unmap this player from the active list--we don't care that it was
    active_players[_id] = NULL;
}

bool WavePlayer::init() {
#if USE_DMA
    if (!_setup_dma()) {
        cout << F("WavePlayer: Failed to set up DMA descriptors") << endl;
        _status = WavePlayerStatus::ERROR;
        return false;
    }
#endif

    _status = WavePlayerStatus::READY;
    return true;
}

typedef struct {
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char fmt_[4];
    uint16_t subchunk_1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} WaveFileHeader __attribute__ ((packed));

bool WavePlayer::start(SdFs *sd, FsFile *file, int16_t **samples, uint32_t *num_samples) {
    if (!file->isOpen()) {
        cout << F("WavePlayer: Cannot start file that is not open!") << endl;
        return false;
    }

    if (_status != WavePlayerStatus::READY) {
        cout << F("WavePlayer: Cannot start file, requires state ") << (uint32_t)WavePlayerStatus::READY << F(" but player was in state ") << (uint32_t)_status << endl;
        return false;
    }

    _sd = sd;
    _file = file;
    _sector_index = 0;

    // check if the file is contiguous
    uint32_t b, e;
    _contiguous = _file->contiguousRange(&b, &e);

    cout << F("WavePlayer file is contiguous: ") << _contiguous << endl;

    uint32_t first_sector, num_sectors;
    _get_next_chunk(&first_sector, &num_sectors);

    if (num_sectors == 0) {
        cout << F("WavePlayer: No sectors to read") << endl;
        return false;
    }

    if (!_start_read_chunk(first_sector, num_sectors)) {
        cout << F("WavePlayer: Failed to read chunk, aborting") << endl;
        return false;
    }

    // wait until we have read the first sector of the file
    Timeout timeout(10000ul);
    while (_num_sectors_read < 1) {
        if (timeout.timed_out()) {
            cout << F("WavePlayer: timed out reading first chunk") << endl;
            return false;
        }
        yield();
    }

    cout << F("WavePlayer: read first chunk") << endl;

    WaveFileHeader *header = reinterpret_cast<WaveFileHeader *>(_dma_rx_bufs[0]);
    cout << F("WAVE HEADER") << endl;
    cout << F("  RIFF: ") << header->riff[0] << header->riff[1] << header->riff[2] << header->riff[3] << endl;
    cout << F("  FILE_SIZE: ") << header->file_size << endl;
    cout << F("  WAVE: ") << header->wave[0] << header->wave[1] << header->wave[2] << header->wave[3] << endl;
    cout << F("  FMT: ") << header->fmt_[0] << header->fmt_[1] << header->fmt_[2] << header->fmt_[3] << endl;
    cout << F("  SUBCHUNK_1_SIZE: ") << header->subchunk_1_size << endl;
    cout << F("  AUDIO_FORMAT: ") << header->audio_format << endl;
    cout << F("  NUM_CHANNELS: ") << header->num_channels << endl;
    cout << F("  SAMPLE_RATE: ") << header->sample_rate << endl;
    cout << F("  BYTE_RATE: ") << header->byte_rate << endl;
    cout << F("  BLOCK_ALIGN: ") << header->block_align << endl;
    cout << F("  BITS_PER_SAMPLES: ") << header->bits_per_sample << endl;
    cout << F("  DATA: ") << header->data[0] << header->data[1] << header->data[2] << header->data[3] << endl;
    cout << F("  DATA_SIZE: ") << header->data_size << endl;

    // validate that the file is indeed a RIFF WAV 
    if (header->riff[0] != 'R' || header->riff[1] != 'I' || header->riff[2] != 'F' || header->riff[3] != 'F'
        || header->wave[0] != 'W' || header->wave[1] != 'A' || header->wave[2] != 'V' || header->wave[3] != 'E'
    ) {
        cout << F("WavePlayer: File is not a WAV file") << endl;
        return false;
    }

    if (header->num_channels != 1) {
        cout << F("WavePlayer: File must have 1 channel") << endl;
        return false;
    }

    if (header->sample_rate != 44100) {
        cout << F("WavePlayer: Invalid sample rate ") << header->sample_rate << endl;
        return false;
    }

    if (header->bits_per_sample != 16) {
        cout << F("WavePlayer: Invalid Bit Rate ") << header->bits_per_sample << endl;
        return false;
    }

    uint64_t time = micros();

    // once we've parsed the header, convert the rest of the samples in the header
    _convert(sizeof(WaveFileHeader), (SD_SECTOR_SIZE - sizeof(WaveFileHeader)) / 2);

    // convert any additional sectors read in the DMA
    for (uint32_t i = 1; i < num_sectors; ++i) {
        // wait for this sector to be read
        timeout.reset();
        while (_num_sectors_read <= i) {
            if (timeout.timed_out()) {
                cout << F("WavePlayer: timed out waiting for sector ") << i << endl;
                goto err;
            }

            yield();
        }

        // convert the next sector
        _convert(i * SD_SECTOR_SIZE, SD_SECTOR_SIZE / 2);
    }

    cout << F("WavePlayer: Converted ") << (num_sectors * SD_SECTOR_SIZE - sizeof(WaveFileHeader)) / 2 << F(" samples in ")
        << micros() - time << F(" us") << endl;

    _sector_index += num_sectors;

    // output the start of the appropriate sample buf & number of usable samples at the address for 
    // use with playback DMA
    *samples = reinterpret_cast<int16_t*>(&_dma_rx_bufs[0][sizeof(WaveFileHeader)]);
    *num_samples = (num_sectors * SD_SECTOR_SIZE - sizeof(WaveFileHeader)) / 2;

    return true;

err:
    return false;
}

void WavePlayer::_swap_buffers() {
    uint8_t *tmp = _dma_rx_bufs[0];
    _dma_rx_bufs[0] = _dma_rx_bufs[1];
    _dma_rx_bufs[1] = tmp;
}

void WavePlayer::_get_next_chunk(uint32_t *sector_out, uint32_t *num_sectors_out) {
    if (_contiguous) {
        // if the WHOLE file is contiguous, we can just calculate based on sector indices

        // # of sectors is file size rounded UP
        int32_t total_file_sectors = (int32_t)((_file->fileSize() + (SD_SECTOR_SIZE - 1)) / SD_SECTOR_SIZE);
        *sector_out = _file->firstSector() + _sector_index;
        *num_sectors_out = max(0, min(_max_sectors, total_file_sectors - _sector_index));
    } 
    else {
        // if the file is NOT contiguous, we can only use 
        uint32_t sec_per_cluster = _sd->sectorsPerCluster();
        // # of clusters to advance
        uint32_t numClusters = (_sector_index + sec_per_cluster - 1) / sec_per_cluster;

        _file->seekSet(_sector_index * SD_SECTOR_SIZE);
        uint32_t curCluster = _file->curCluster();

        cout << F("Got next cluster ") << curCluster << endl;

        goto err;

        // *sector_out = _sd->dataStartSector() + 
    }

    return;

err:
    *num_sectors_out = 0;
}

bool WavePlayer::read_and_convert(int16_t **samples, uint32_t *num_samples) {
    // first swap buffers before we start the next read, since we ALWAYS read info _dma_rx_bufs[0]
    _swap_buffers();

    // find which sector to read a contiguous chunk for
    // TODO: deal with partial sectors (e.g. EOF)
    uint32_t sector = 0, ns = 0;
    _get_next_chunk(&sector, &ns);

    // if there are no more sectors left to read in the file, indicate that the playback should stop
    if (ns == 0) return false;

    // start a DMA for that sector
    if (!_start_read_chunk(sector, ns)) {
        cout << F("WavePlayer: Failed to read chunk, aborting") << endl;
        return false;
    }
    
    // convert sector-by sector as the DMA progresses in the background
    Timeout timeout(1000000ul);
    for (uint32_t i = 0; i < ns; ++i) {
        // wait for this sector to be read
        while (_num_sectors_read <= i) {
            if (timeout.timed_out()) {
                cout << F("WavePlayer: timed out waiting for sector ") << i << endl;
                goto err;
            }

            yield();
        }

        // convert the next sector
        _convert(i * SD_SECTOR_SIZE, SD_SECTOR_SIZE / 2);
    }

    _end_read_chunk();

    _sector_index += ns;

    // TODO handle partial sectors, e.g. EOF
    *samples = reinterpret_cast<int16_t*>(_dma_rx_bufs[0]);
    *num_samples = (ns * SD_SECTOR_SIZE) / 2;
    return true;

err:
    return false;
}

void WavePlayer::_convert(uint32_t offset_bytes, uint32_t sample_count) {
    // cout << F("Converting ") << dec << sample_count << F(" samples from offset ") << offset_bytes << endl; 
    int16_t *pcm_samples = reinterpret_cast<int16_t*>(&_dma_rx_bufs[0][offset_bytes]);
    for (uint32_t i = 0; i < sample_count; ++i) {
        pcm_samples[i] = (pcm_samples[i] >> 6) + 512;
    }
}

void WavePlayer::_end_read_chunk() {
    //cout << F("Ending chunk read") << endl;
    //_free_dma();
}

bool WavePlayer::_start_read_chunk(uint32_t sector, uint32_t ns) {
#if USE_DMA
    ZeroDMAstatus dma_status = DMA_STATUS_OK;

    if (!_sd->card()->readStart(sector)) {
        cout << F("WavePlayer: Failed to start read of sector ") << sector << endl;
        goto err;
    }

    if (!wait_for_sector_start()) {
        cout << F("WavePlayer: Failed to get sector start after readStart") << endl;
        goto err;
    }

    cout << F("Starting read of ") << ns << F(" sectors at sector ") << sector << endl;

    // if (sector != _file->firstSector()) {
    //     // read a full sector
    //     cout << hex << uppercase;
    //     for (int i = 0; i < 516 * 4; ++i) {
    //         if (i % 16 == 0) cout << endl;
    //         cout << (uint32_t)spiReceive() << F(" ");
    //     }
    // }

    // point the sector DMA to the END of the first 

    // if (dma_tx.print != DMA_STATUS_OK) {
    //     cout << F("")
    // }
    
    desc_tx->BTCNT.bit.BTCNT = SD_SECTOR_SIZE + 4;

    desc_rx[0]->BTCNT.bit.BTCNT = 2;
    desc_rx[1]->BTCNT.bit.BTCNT = SD_SECTOR_SIZE;
    desc_rx[1]->DSTADDR.bit.DSTADDR
        = (uint32_t)(&_dma_rx_bufs[0][SD_SECTOR_SIZE]);
    desc_rx[2]->BTCNT.bit.BTCNT = 2;

    // how many sectors to read
    _sectors_to_read = ns;
    // clear the # of sectors read
    _num_sectors_read = 0;

    // start the RX job
    dma_status = dma_rx.startJob();
    if (dma_status != DMA_STATUS_OK) {
        cout << F("WavePlayer: Failed to start RX job, status: ") << (uint32_t)dma_status << endl;
        goto err;
    }

    dma_status = dma_tx.startJob();
    if (dma_status != DMA_STATUS_OK) {
        cout << F("WavePlayer: Failed to start TX job, status: ") << (uint32_t)dma_status << endl;
        goto err;
    }
    
    cout << F("WavePlayer: Finished chunk setup, triggering TX") << endl;

    // trigger the TX job which will trigger the RX job until completion
    dma_tx.trigger();

    return true;

err:
    _status = WavePlayerStatus::ERROR;
    return false;
#else
    //cout << F("WavePlayer: Reading ") << ns << F(" sectors from ") << sector << F(" into ") << hex << (uint32_t)_dma_rx_bufs[0] << dec << endl;
    if (!_sd->card()->readSectors(sector, _dma_rx_bufs[0], ns)) {
        cout << F("WavePlayer: Failed to read ") << ns << F(" sectors from sector ") << sector << endl;
        return false;
    }

    _num_sectors_read = ns;
    return true;
#endif
}

#if USE_DMA
void WavePlayer::player_dma_callback() {
    _num_sectors_read++;
    if (_num_sectors_read >= _sectors_to_read) return;

    // advance the sector pointer
    desc_rx[1]->DSTADDR.bit.DSTADDR
        = (uint32_t)(_dma_rx_bufs[0] + SD_SECTOR_SIZE * (_num_sectors_read + 1));

    // restart the jobs to read the next sector
    dma_tx.startJob();
    dma_rx.startJob();

    dma_tx.trigger();
}

void WavePlayer::_free_dma() {
    dma_tx.abort();
    dma_rx.abort();
    dma_tx.free();
    dma_rx.free();
}

bool WavePlayer::_setup_dma() {
    ZeroDMAstatus dma_status = dma_tx.allocate();
    if (dma_status != DMA_STATUS_OK) {
        cout << F("WavePlayer: Couldn't allocate TX DMA, status: ") << dma_status << endl;
        goto err;
    }

    dma_tx.setTrigger(SPI.getDMAC_ID_RX());
    dma_tx.setAction(DMA_TRIGGER_ACTON_BEAT);
    desc_tx = dma_tx.addDescriptor(
        (void*)(&_dma_tx_buf[0]),
        (void*)(SPI.getDataRegister()),
        SD_SECTOR_SIZE + 4,
        DMA_BEAT_SIZE_BYTE,
        false,
        false);
    if (!desc_tx) {
        cout << F("WavePlayer: Failed adding DMAC descriptor to TX channel.") << endl;
        goto err;
    }
    dma_tx.setCallback(wav_tx_dma_callback);

    cout << F("Registered DMA channel ") << (uint32_t)dma_tx.getChannel() << F(" as TX") << endl; 

    // -- Set up RX descriptor
    dma_status = dma_rx.allocate();
    if (dma_status != DMA_STATUS_OK) {
        cout << F("WavePlayer: Couldn't allocate RX DMA, status: ") << dma_status << endl;
        goto err;
    }
    // DMA trigger is SPI receive
    dma_rx.setTrigger(SPI.getDMAC_ID_TX());
    // transfer 1 beat at a time
    dma_rx.setAction(DMA_TRIGGER_ACTON_BEAT);
    desc_rx[0] = dma_rx.addDescriptor(
        (void*)(SPI.getDataRegister()),
        (void*)(&_dma_tmp_buf[0]),
        2,
        DMA_BEAT_SIZE_BYTE,
        false,
        true);
    if (!desc_rx[0]) {
        cout << F("WavePlayer: Failed adding DMAC descriptor 0 to RX channel.") << endl;
        goto err;
    }

    desc_rx[1] = dma_rx.addDescriptor(
        // DMA from the SPI data register
        (void*)(SPI.getDataRegister()),
        // leave dst null for now--we'll set this before we start the job
        (void*)(&_dma_rx_bufs[0][0]),
        // transfer 1 sector
        SD_SECTOR_SIZE,
        // 1 byte at a time
        DMA_BEAT_SIZE_BYTE,
        // do NOT increment src (stay at the spi register)
        false,
        // DO increment dst, so we read into successive bytes
        true);
    if (!desc_rx[1]) {
        cout << F("WavePlayer: Failed adding DMAC descriptor 1 to RX channel.") << endl;
        goto err;
    }

    desc_rx[2] = dma_rx.addDescriptor(
        (void*)(SPI.getDataRegister()),
        (void*)(&_dma_tmp_buf[0]),
        2,
        DMA_BEAT_SIZE_BYTE,
        false,
        true);
    // desc_rx[2]->BTCTRL.reg |= DMAC_BTCTRL_BLOCKACT_INT;
    if (!desc_rx[2]) {
        cout << F("WavePlayer: Failed adding DMAC descriptor 2 to RX channel.") << endl;
        goto err;
    }

    dma_rx.setCallback(wav_rx_dma_callback);

    cout << F("Registered DMA channel ") << (uint32_t)dma_rx.getChannel() << F(" as RX") << endl; 

    return true;

err:
    _status = WavePlayerStatus::ERROR;
    return false;
}

static uint8_t spiReceive() {
  return SPI.transfer(0xFF);
}

static bool wait_for_sector_start() {
    // wait for start sector token
    uint8_t status;

    Timeout timeout(10000ul);
    while ((status = spiReceive()) == 0XFF) {
        if (timeout.timed_out()) {
            return false;
        }
    }

    if (status != DATA_START_SECTOR) {
        return false;
    }

    return true;
}

static void wav_tx_dma_callback(Adafruit_ZeroDMA* dma) {
    (void)dma;
}

static void wav_rx_dma_callback(Adafruit_ZeroDMA* dma) {
    (void) dma;

    num_dma_callbacks++;

    WavePlayer *player = NULL;
    for (uint8_t i = 0; i < active_player_count; ++i) {
        if (active_players[i] == NULL) continue;

        if (active_players[i]->owns_dma(dma)) {
            player = active_players[i];
        }
    }

    if (!player) return;

    player->player_dma_callback();
}
#endif