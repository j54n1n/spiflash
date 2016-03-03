/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2016 Julian Sanin

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*******************************************************************************/

#ifndef SPI_FLASH_H
#define SPI_FLASH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <time.h>
#endif

enum SpiFlashError {
	SpiFlashErrorSuccess,
	SpiFlashErrorTimeout,
	SpiFlashErrorAccessDenied,
	SpiFlashErrorInputValue
};

template<typename SpiDevice, uint32_t FLASH_SIZE = 0x7FFFFull /*512k*/>
class SpiFlash {

	enum {
		CMD_WRITE_STATUS_REGISTER = 0x01,
		CMD_PAGE_PROGRAM = 0x02,
		CMD_READ_DATA = 0x03,
		CMD_READ_STATUS_REGISTER = 0x05,
		CMD_WRITE_ENABLE = 0x06,
		CMD_SECTOR_ERASE_4K = 0x20,
		CMD_READ_UNIQUE_ID = 0x4B,
		CMD_BLOCK_ERASE_32K = 0x52,
		CMD_RELEASE_POWER_DOWN = 0xAB,
		CMD_POWER_DOWN = 0xB9,
		CMD_JEDEC_ID = 0x9F,
		REG_STATUS_REGISTER_BUSY = (1 << 0),
	};

	SpiDevice spi;
	bool isPoweredDown;

	void recoverFromPowerDown(void) {
		if (isPoweredDown) {
			spi.transfer(CMD_RELEASE_POWER_DOWN);
			isPoweredDown = false;
		}
	}

	//! Set the write enable latch.
	void writeEnable(void) {
		spi.transfer(CMD_WRITE_ENABLE);
	}

	//! Erase a block of SPI flash.
	void eraseBlock(uint32_t offset, uint8_t block) {
		// Invalid block size.
		if (block != 4 && block != 32)
			return SpiFlashErrorInputValue;
		// Not block aligned.
		if ((offset % (block * 1024)) != 0)
			return SpiFlashErrorInputValue;
		//Enable writing to SPI Flash.
		int result = writeEnable();
		if (result) {
			return result;
		}
		uint8_t bytes[] = {
			((block == 4) ? CMD_SECTOR_ERASE_4K : CMD_BLOCK_ERASE_32K),
			((offset >> 16) & 0xFF),
			((offset >> 8) & 0xFF),
			(offset & 0xFF)
		};
		spi.transferBulk(bytes, sizeof(bytes));
		// Wait for previous operation to complete.
		return wait();
	}

public:
	void init(void) {
		spi.master();
		recoverFromPowerDown();
	}
	//! Waits for the chip to finish the current operation. Must be called
	//! after erase/write operations to ensure successive commands are executed.
	//! \returns SpiFlashErrorSuccess or SpiFlashErrorTimeout otherwise.
	int wait(void) {
		recoverFromPowerDown();
#ifdef ARDUINO
		const uint32_t TIMEOUT = 800; // 800ms.
		uint32_t start = millis();
#else
		const clock_t TIMEOUT = 0.8 * CLOCKS_PER_SEC; // 800ms.
		clock_t start = clock();
#endif
		while (getStatus() & REG_STATUS_REGISTER_BUSY) {
#ifdef ARDUINO
			uint32_t current = millis();
#else
			clock_t current = clock();
#endif
			if ((current - start) > TIMEOUT) {
				return SpiFlashErrorTimeout;
			}
		}
		return SpiFlashErrorSuccess;
	}
	//! Returns the contents of SPI Flash status register.
	//! \returns register contents.
	uint8_t getStatus(void) {
		recoverFromPowerDown();
		return spi.transferRegister(CMD_READ_STATUS_REGISTER, 0);
	}
	//! Sets the SPI Flash status register (non-volatile bits only).
	//! \param registerValue Status register value.
	int setStatus(uint8_t registerValue) {
		recoverFromPowerDown();
		//if (checkWriteProtection() != SpiFlashWriteProtectionNone) {
		//	return SpiFlashErrorAccessDenied;
		//}
		writeEnable();
		spi.transferRegister(CMD_WRITE_STATUS_REGISTER, registerValue);
		// Update takes up to 10 ms, so wait for transaction to finish.
		return wait();
	}
	//! Returns the content of SPI Flash memory.
	//! \param data Buffer to write flash contents.
	//! \param offset
	//! \param bytes
	//! \returns SpiFlashErrorSuccess or non-zero if any error.
	int read(uint8_t* /*[out]*/ data, uint32_t offset, uint8_t bytes) {
		if ((offset + bytes) > FLASH_SIZE) {
			return SpiFlashErrorInputValue;
		}
		recoverFromPowerDown();
		const size_t length = 4 + bytes; // Command + address + bytes.
		uint8_t* buffer = new uint8_t[length];
		for (size_t i = 0; i < length; i++) {
			buffer[i] = 0x00;
		}
		buffer[0] = CMD_READ_DATA;
		buffer[1] = ((offset >> 16) & 0xFF);
		buffer[2] = ((offset >> 8) & 0xFF);
		buffer[3] = (offset & 0xFF);
		spi.transferBulk(buffer, length);
		memcpy(data, &buffer[4], bytes);
		delete[] buffer;
		return SpiFlashErrorSuccess;
	}
	//! Erase SPI flash.
	//! \param offset Flash offset to start erasing.
	//! \param bytes Number of bytes to erase.
	//! \returns SpiFlashErrorSuccess or non-zero if any error.
	int erase(size_t offset, size_t bytes) {
		if ((offset + bytes) > FLASH_SIZE) {
			return SpiFlashErrorInputValue;
		}
		// Not aligned to sector(4kb).
		if (offset % 4096 || bytes % 4096) {
			return SpiFlashErrorInputValue;
		}
		recoverFromPowerDown();
		// Largest unit is block (32kb).
		if (offset % (32 * 1024) == 0) {
			while (bytes != (bytes % (32 * 1024))) {
				int result = eraseBlock(offset, 32);
				if (result) {
					return result;
				}
				bytes -= 32 * 1024;
				offset += 32 * 1024;
			}
		}
		// Largest unit is sector(4kb).
		while (bytes != (bytes % (4 * 1024))) {
			int result = eraseBlock(offset, 4);
			if (result) {
				result;
			}
			bytes -= 4 * 1024;
			offset += 4 * 1024;
		}
		return SpiFlashErrorSuccess;
	}
	//! Write to SPI Flash. Assumes already erased.
	//! \param data Data to write to Flash.
	//! \param offset Flash offset to write.
	//! \param bytes Number of bytes to write.
	//! \returns SpiFlashErrorSuccess or non-zero if any error.
	int write(const uint8_t* const /*[in]*/ data, uint32_t offset,
			uint8_t bytes) {
		if (!data || ((offset + bytes) > FLASH_SIZE)) {
			return SpiFlashErrorInputValue;
		}
		recoverFromPowerDown();
		uint16_t writeSize;
		while (bytes > 0) {
			// Write length can not go beyond the end of the flash page.
			writeSize = 256 - (offset & 0xFF);
			writeSize = (bytes <= writeSize) ? bytes : writeSize;
			// Wait for previous operation to complete.
			int result = wait();
			if (result) {
				return result;
			}
			// Enable writing to SPI flash.
			writeEnable();
			const size_t length = 4 + writeSize; // Command + address + bytes.
			uint8_t* buffer = new uint8_t[length];
			for (size_t i = 0; i < length; i++) {
				buffer[i] = 0x00;
			}
			buffer[0] = CMD_PAGE_PROGRAM;
			buffer[1] = ((offset >> 16) & 0xFF);
			buffer[2] = ((offset >> 8) & 0xFF);
			buffer[3] = (offset & 0xFF);
			memcpy(&buffer[4], data, writeSize);
			spi.transferBulk(buffer, length);
			delete[] buffer;
			data += writeSize;
			offset += writeSize;
			bytes -= writeSize;
		}
		// Wait for previous operation to complete.
		return wait();
	}
	//! Returns the SPI flash JEDEC ID (manufacturer ID, memory type, and
	//! capacity).
	//! \returns Flash JEDEC ID or 0 on error.
	uint32_t getJedecId(void) {
		recoverFromPowerDown();
		uint32_t jedecId = 0;
		const size_t length = 4; // Command + manufacturer + type + capacity.
		uint8_t buffer[length] = { 0 };
		buffer[0] = CMD_JEDEC_ID;
		spi.transferBulk(buffer, length);
		jedecId =
			((uint32_t)buffer[1] << 16) |
			((uint32_t)buffer[2] <<  8) |
			((uint32_t)buffer[3] <<  0);
		return jedecId;
	}
	//! Returns the SPI flash unique ID (serial).
	//! \returns Flash unique ID or 0 on error.
	uint64_t getUniqueId(void) {
		recoverFromPowerDown();
		uint64_t uniqueId = 0;
		const size_t length = 5 + 8; // Command + dummy bytes + unique id.
		uint8_t buffer[length] = { 0 };
		buffer[0] = CMD_READ_UNIQUE_ID;
		spi.transferBulk(buffer, length);
		uniqueId =
			((uint64_t)buffer[5]  << 56) |
			((uint64_t)buffer[6]  << 48) |
			((uint64_t)buffer[7]  << 40) |
			((uint64_t)buffer[8]  << 32) |
			((uint64_t)buffer[9]  << 24) |
			((uint64_t)buffer[10] << 16) |
			((uint64_t)buffer[11] <<  8) |
			((uint64_t)buffer[12] <<  0);
		return uniqueId;
	}
	//! Set Flash memory in power down mode.
	void sleep(void) {
		if (!isPoweredDown) {
			spi.transfer(CMD_POWER_DOWN);
			isPoweredDown = true;
		}
	}
};

#endif // SPI_FLASH_H
