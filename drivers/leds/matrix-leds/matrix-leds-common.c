/*
* matrix-leds-common.c - common implements for matrix leds
*/

#include "matrix-leds-common.h"

static bool crc_tabccitt_init = false;
static uint16_t crc_tabccitt[256] = {0};

void init_crcccitt_tab(void) {
	uint16_t i;
	uint16_t j;
	uint16_t crc;
	uint16_t c;

	for (i = 0; i < 256; i++) {
		crc = 0;
		c = i << 8;
		for (j = 0; j < 8; j++) {
			if ((crc ^ c) & 0x8000)
				crc = (crc<<1)^0x1021;
			else
				crc = crc<<1;
			c = c<<1;
		}
		crc_tabccitt[i] = crc;
	}
	crc_tabccitt_init = true;
}

uint16_t crc_16(const unsigned char *input_str, uint32_t num_bytes) {
	uint16_t crc;
	const unsigned char *ptr;
	uint32_t a;

	if (!crc_tabccitt_init) {
		init_crcccitt_tab();
	}

	crc = 0xFFFF;
	ptr = input_str;

	if (ptr != NULL) {
		for(a = 0; a < num_bytes; a++) {
			crc = (crc << 8) ^ crc_tabccitt[((crc >> 8) ^ (uint16_t) *ptr++) & 0x00FF];
		}
	}

	return crc;
}

void set_messge_header(uint8_t *message) {
	*message =  COMMAND_DESC_SUB_1(SPI_RFAME_MAGIC);
	*(message + 1) = COMMAND_DESC_SUB_0(SPI_RFAME_MAGIC);
}

void set_message_crc(uint8_t *message, uint32_t num_bytes) {
	uint16_t crc = crc_16(message, num_bytes);
	*(message + num_bytes) = COMMAND_DESC_SUB_1(crc);
	*(message + num_bytes + 1) = COMMAND_DESC_SUB_0(crc);
}
