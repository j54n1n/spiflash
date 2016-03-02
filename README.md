# spiflash
SPI Flash memory driver for Winbond W25X and W25Q and compatible series.

## Interface
Connect at least the pins *VCC*, *DI*, *DO* *CS* and *GND*.

## Usage
Include the [SpiDevice](https://github.com/j54n1n/spidevice) library and its
dependencies and declare an instance. The optional second template parameter can
be used to specify a different memory size than the default 4Mbit/512kB.
```
#include <SPI.h>
#include <SpiDevice.h>
#include <SpiFlash.h>

SpiFlash<SpiDevice<8> > flash;

void setup() {
  flash.init();
  // other code ...
}
```