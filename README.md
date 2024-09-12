# Rotary Phone Voicemail Gallery

Dependencies:
- Sdfat - Adafruit Fork
- Adafruit Zero DMA Library

This is a project using a vintage rotary phone and an Adafruit Feather M0 Adalogger to create a "voicemail gallery". Each voicemail is given a 2-digit number which can be dialed on the rotary phone in order to play a specific message, and it is intended to be paired with a contact book which 
contains entries listing all available voicemail numbers.

All audio files are stored on a micro SD card in mono 44.1k 16-bit PCM WAV format. The voicemail audio files must be named with the 2 digits that should be used to listen to
that voicemail, e.g. "13.WAV" will be played when 1 and then 3 is dialed on the rotary phone. 

The code also includes handling for a dial tone which is looped by default when no other audio is playing, a vintage ring sound which is played before each message, and a set of intercept messages which are pieced together to play a message when a number is dialed that doesn't correspond to a specific voicemail.

## SD card reading

This library interfaces with the SD card via SPI using Sdfat - Adafruit Fork. The streaming API was too slow, so I used the more raw `readSectors` API instead which skips all seeking, cacheing, etc. We only read full sectors at a time and keep track of how many sectors we
have read from a file. We only read contiguous chunks of sectors at a time, so before each chunked read we have to find the next appropriate
sector using the raw FAT fable API. This is only necessary for non-contiguous files though, so we have a fast path for contiguous files that just reads contiguous sectors from the start chunk. We could optimize this a bit by storing the sector we last read from so only ever need to traverse a single entry in the FAT table, but that wasn't necessary for this application.

### DMA

While I had some issues with successive reads with this approach I didn't have time to resolve, and it turned out I didn't need to for 
this application, as the `readSectors` approach was fast enough, I was able to set up a DMA procedure that will read some # of sectors from the SD card directly into memory, allowing the CPU to convert samples to DAC-ready values while data is being read in the background. 
This approach would be very useful to applications which are more CPU-heavy, as we would spend much less CPU time on reading from SPI, 
and would enable more CPU-heavy audio processing which could happen in parallel with the DMA read of further sectors.

SPI has a single "transceive" operation which simultaneously sends and receives a single byte at a time. Because of this, we need 2 DMA channels. 

The first channel, TX, is configured to copy a single 0xff byte from a buffer in memory to the SPI register. This is the "initiator" DMA. Its trigger is configured as the "RX" SPI trigger, so once a byte has successfully been _read_ from the SPI register, we trigger the next _write_.

The second channel, RX, is configured to copy _from_ the SPI register to the receive buffer in memory. Its trigger is configured as the "TX" SPI trigger, so once a byte has been successfully _written_ over SPI, we trigger the next _read_. 

We manually trigger the first _send_, after which these two DMA channels trigger each other back and forth until the requested # of bytes has been read. This allows us to read from SPI as fast as possible without CPU interference.

To set up for the DMA, we initiate a multi-sector read on a specific sector using `CMD18: READ_MULTIPLE_SECTOR`
then wait for the `DATA_START_SECTOR` marker `0xFE`. Then we trigger the DMA, which consists of 3 distinct chained descriptors. The first will read 2 bytes into a temporary buffer to pull off the sector start markers. Then we read 1 full sector of data (512 bytes) into our real receive buffer. Finally we have a descriptor which reads the 2 CRC bytes again into a temporary buffer which is ignored.

## Audio Playback

For audio playback the Feather M0 has a built-in 12-bit DAC on pin A0, which is perfectly adequate for our purposes. This signal is fed into a PAM8302A 2.5W mono audio amp. This amp is not super well-suited to the ~45 Ohm speaker that came with the vintage rotary phone, but with the volume adequately tuned it worked fine. It ended up being _very_ finnicky to tune with the trim pot though, as the bottom 25% of the trim pot would give me no sound at all, and the top 50% of the trim pot's range would draw too much current and shut off the microcontroller, though fortunately that narrow band still made it plenty loud for near-ear use.

Timer-triggered DMA is very important for accurate audio playback, as any approach using the CPU is prone to jitter and drift. As such, we configure the `TC5` timer on the SAMD21 to overflow every 1/44.1k seconds, and we allocate and configure a DMA channel to use that timer overflow as a trigger for a single beat, which copies a sample from the sample buffer to the DAC register (pin A0). See [AudioPlayer.cpp](./src/AudioPlayer.cpp) for details.

For playback we use a 16-bit DMA that copies from the converted sample buffer. We use a buffer-swap strategy for streaming, so while the playback DMA is playing one buffer, we are reading and converting the next set of samples into the second buffer. We rely on the SD card reading being faster than playback for streaming to be successful--if the playback of a buffer finishes without us having enqueue another chunk converted, playback will end. 

Currently this code is limited to 44.1k Mono 16-bit PCM WAV files, but it would be fairly easy to support all WAV files:

1. We already read the sample rate from the WAV header for validation--we can reconfigure TC5 at the actual sample rate of the file instead of locking it to 44.1k
2. We could support stereo files also by reading the WAV header and either changing our DMA increment to 2 (since stereo samples are interleaved, we just skip 2 beats instead of 1 beat to play one channel at a time), or we can change our conversion code to mix stereo down to mono.
3. We have a 12-bit DAC, so we always want to use 16-bit DMA beat (HWORD), but if we can again select a sample conversion strategy based on the WAV header to support other sample formats.

## Dialer

I used the original dialer from the vintage rotary phone. The mechanism is an electrical contact which is interrupted for each digit. So dialing a 1 will create a single pulse of roughly 60ms, a 2 will be 2 pulses of roughly 60ms separated by some 30-60ms.

I passed 5V through the switch, and used a 10k pulldown resistor to ground on pin A1--normally the pin is high, but when the switch is interrupted, the pin is pulled low by the pulldown. I originally set this up using interrupts on that pin to detect changes, but I found that to be incredibly noisy and quite challenging to effectively debounce. Given the timings involved are pretty huge and not critical, this was also way overkill, so I just switched to a CPU approach to detect pulses and a timeout for detecing when the pulses for a dial have finished, and this worked great.  