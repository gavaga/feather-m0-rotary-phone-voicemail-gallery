#ifndef SAMDDMASPICARD_H_
#define SAMDDMASPICARD_H_

#include <SdCard/SdSpiCard.h>

//==============================================================================
/**
 * \class DedicatedSpiCard
 * \brief Raw access to SD and SDHC flash memory cards via dedicate SPI port.
 */
class DedicatedSamdDmaSpiCard : public SharedSpiCard {
 public:
  /** Construct an instance of DedicatedSpiCard. */
  DedicatedSamdDmaSpiCard() {}
  /** Initialize the SD card.
   * \param[in] spiConfig SPI card configuration.
   * \return true for success or false for failure.
   */
  bool begin(SdSpiConfig spiConfig);
  /** \return true, can be in dedicaded state. */
  bool hasDedicatedSpi() {return true;}
  /** \return true if in dedicated SPI state. */
  bool isDedicatedSpi() {return m_dedicatedSpi;}
  /**
   * Read a 512 byte sector from an SD card.
   *
   * \param[in] sector Logical sector to be read.
   * \param[out] dst Pointer to the location that will receive the data.
   * \return true for success or false for failure.
   */
  bool readSector(uint32_t sector, uint8_t* dst);
  /**
   * Read multiple 512 byte sectors from an SD card.
   *
   * \param[in] sector Logical sector to be read.
   * \param[in] ns Number of sectors to be read.
   * \param[out] dst Pointer to the location that will receive the data.
   * \return true for success or false for failure.
   */
  bool readSectors(uint32_t sector, uint8_t* dst, size_t ns);
  /** Set SPI sharing state
   * \param[in] value desired state.
   * \return true for success else false;
   */
  bool setDedicatedSpi(bool value);
  /**
   * Write a 512 byte sector to an SD card.
   *
   * \param[in] sector Logical sector to be written.
   * \param[in] src Pointer to the location of the data to be written.
   * \return true for success or false for failure.
   */
  bool writeSector(uint32_t sector, const uint8_t* src);
  /**
   * Write multiple 512 byte sectors to an SD card.
   *
   * \param[in] sector Logical sector to be written.
   * \param[in] ns Number of sectors to be written.
   * \param[in] src Pointer to the location of the data to be written.
   * \return true for success or false for failure.
   */
  bool writeSectors(uint32_t sector, const uint8_t* src, size_t ns);

 private:
  uint32_t m_curSector;
  bool m_dedicatedSpi = false;
};

#endif // SAMDDMASPICARD_H_