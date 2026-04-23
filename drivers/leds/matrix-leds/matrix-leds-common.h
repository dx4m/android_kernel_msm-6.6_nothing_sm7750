#ifndef __MATRIX_LEDS_COMMON_H__
#define __MATRIX_LEDS_COMMON_H__

#include <linux/types.h>

/*
* Total message format
* OP flags(8 Bits) + Operation Codes(8 Bits) + Data Description(32 Bits) + Data Field(Indefinite length) + CRC
*/

/*
* play pic, 527 bytes: header + data + crc
* header:(0x55 + 0xAA, 2 bytes)
* data:(0xA1 + 0x05 + 0x05 + 0x02 + 0x01 + 0x01 + pixels[517], total 523 bytes)
* crc:(crc only use for data(523 bytes),  high byte + low byte, 2 bytes)
*/

/*
* Data Description(32 Bits)
*/
#define SPI_RFAME_MAGIC						(0x55AA)

/*
* Operation Flags(8 Bits)
*/
#define SPI_WRITE_FLAG						(0xA1)
#define SPI_READ_FLAG						(0xA2)

/*
* Read Operation Codes(8 Bits)
*/
#define SPI_READ_REG						(0x01)
#define SPI_READ_FW_VER						(0x02)

/*
* Data Description(32 Bits)
*/
#define SPI_READ_CHIP_ID_DESC				(0x00000002)
#define SPI_READ_REG_DESC				(0x00000002)
#define SPI_READ_COMMAND_DESC				(0x00000002)
#define SPI_READ_FW_VER_DESC				(0x00000010)
#define SPI_WRITE_PICTURE_CONTENT			(0x01010205)
#define SPI_WRITE_COMMAND_DESC				(0x00000004)
#define SPI_WRITE_VIDEO_PLAY_MODE			(0x00000001)
#define SPI_WRITE_FW_UPDATE_MODE			(0x00000002)
#define SPI_WRITE_BRIGHTNESS_GAIN_DESC			(0x00000004)

#define DEBUG_MODE						(0x0003)
#define SPI_WRITE_DEBUG_DESC                (0x00000006)
#define SPI_READ_DEBUG_DESC                 (0x00000400)
/*
* Write Operation Codes(8 Bits)
*/
#define SPI_WRITE_COMMAND				(0x01)
#define SPI_WRITE_VIDEO_CONTENT				(0x02)
#define SPI_WRITE_VIDEO_PLAY				(0x03)
#define SPI_WRITE_STOP_PLAY				(0x03)
#define SPI_WRITE_PICTURE_PLAY				(0x04)
#define SPI_WRITE_SPI_CLK_CTRL				(0x05)
#define SPI_WRITE_FW_TRANSFER				(0x06)
#define SPI_WRITE_FW_UPDATE				(0x07)
#define SPI_WRITE_BOOT_TRANSFER				(0x08)
#define SPI_WRITE_BOOT_UPDATE				(0x09)
#define SPI_WRITE_BRIGHTNESS_GAIN			(0x0A)

/*
* Video Play Mode
*/
#define PLAY_ONCE							(0x01)
#define PLAY_TWICE							(0x02)
#define PLAY_FOREVER							(0xAA)
/*
*  Command Description Sub macro
*/
#define COMMAND_DESC_SUB_0(var)				(uint8_t)((var) & 0xFF)
#define COMMAND_DESC_SUB_1(var)				(uint8_t)((var >> 8) & 0xFF)
#define COMMAND_DESC_SUB_2(var)				(uint8_t)((var >> 16) & 0xFF)
#define COMMAND_DESC_SUB_3(var)				(uint8_t)((var >> 24) & 0xFF)

void init_crcccitt_tab(void);
uint16_t crc_16(const unsigned char *input_str, uint32_t num_bytes);
void set_messge_header(uint8_t *message);
void set_message_crc(uint8_t *message, uint32_t num_bytes);

#endif /*__MATRIX_LEDS_COMMON_H__*/
