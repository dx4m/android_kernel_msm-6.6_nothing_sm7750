#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-msm-geni.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/debugfs.h>
#include <linux/miscdevice.h>
#include <linux/completion.h>
#include <linux/mman.h>
#include <linux/poll.h>
#include <linux/pm_wakeup.h>
#include <asm/uaccess.h>
#include <linux/pinctrl/consumer.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
//#include <stdbool.h>
//#include <stddef.h>
#include <linux/rtc.h>
#include <linux/timekeeping.h>
#include <asm/cpufeature.h>

#include "matrix-leds.h"
#include "matrix_leds_video_data.h"

#define MAX_SPI_FREQ_HZ (1000000) //(20000000)
#define HI 200
#define HI_200 200
#define HI_120 120
#define HI_80 80
#define HI_50 50

#define LO 100
#define LINE 25

#define DRV_TAG "[matrix-leds] "

#define LTN_CHIP_ID		0x10
#define RFD_CHIP_ID		0x20
#define LEDS_TRANSMIT_FW_DATA						0
#define LEDS_REFRESH_FW_DATA						1
#define LEDS_FW_REQUEST_SUPPORT						 1
/* Example: matrix_leds_fw.bin */
#define FILE_NAME_LENGTH					128
#define LEDS_LTN_FW_NAME_PREX_WITH_REQUEST				 "matrix_leds_ltn_fw"
#define LEDS_RFD_FW_NAME_PREX_WITH_REQUEST				 "matrix_leds_rfd_fw"
#define LEDS_LTN_BL_NAME_PREX_WITH_REQUEST				 "matrix_leds_ltn_bl"
#define LEDS_RFD_BL_NAME_PREX_WITH_REQUEST				 "matrix_leds_rfd_bl"
#define LEDS_READ_BOOT_ID_TIMEOUT					 3
#define LEDS_FLASH_PACKET_LENGTH_SPI_LOW			 (4 * 1024 - 4)
#define LEDS_FLASH_PACKET_LENGTH_SPI				 (32 * 1024 - 16)
#define LEDS_LTN_FW_VERSION								(0x17)
#define LEDS_RFD_FW_VERSION								(0x0C)
#define LEDS_LTN_BOOTLOADER_VERSION						(0x01)
#define LEDS_RFD_BOOTLOADER_VERSION						(0x01)

#define HEX_BUF_SIZE (16 * 1024)

static int log_level = 0;
module_param(log_level, int, 0644);

#define LOG_INFO(fmt, ...) \
	do { \
		if (log_level == 0 || log_level == 0xFF) \
			pr_info(DRV_TAG fmt, ##__VA_ARGS__); \
	} while (0)

#define LOG_ERR(fmt, ...) \
	do { \
		if (log_level == 0 || log_level == 1 || log_level == 0xFF) \
			pr_err(DRV_TAG fmt, ##__VA_ARGS__); \
	} while (0)

#define LOG_DBG(fmt, ...) \
	do { \
		if (log_level == 0xFF) \
			pr_info(DRV_TAG fmt, ##__VA_ARGS__); \
	} while (0)

/*
* led pixels count
*/
#define VIDEO_FRAMES_OF_PAC		(6)
#define NUM_LEDS_VIDEOS (sizeof(leds_fac_video) / sizeof(struct leds_video_data))

#if IS_ENABLED(CONFIG_ARCH_MEDIATEK)
extern void mt_spi_enable_master_clk(struct spi_device *spidev);
extern void mt_spi_disable_master_clk(struct spi_device *spidev);
#endif

#define BUILD_PACKET_INFO(total, current, length) \
	((uint8_t[4]) { \
		(uint8_t)((length) & 0xFF),				 /* Low byte of data length */ \
		(uint8_t)(((length) >> 8) & 0xFF),		 /* High byte of data length */ \
		(uint8_t)((current) & 0xFF),			 /* Low byte of current packet number */ \
		(uint8_t)((total) & 0xFF)				 /* Low byte of total packet count */ \
	})

uint8_t zero[] = {1, 2, LINE, LINE + 3, LINE * 2, LINE * 2 + 3, LINE * 3, LINE * 3 + 3,
	LINE * 4 + 1, LINE * 4 + 2};
uint8_t four[] = {2, LINE + 1, LINE + 2, LINE * 2, LINE * 2 + 2,
	LINE * 3, LINE * 3 + 1, LINE * 3 + 2, LINE * 3 + 3, LINE * 4 + 2};
uint8_t five[] = {1, 2, 3, LINE + 1, LINE * 2 + 1, LINE * 2 + 2, LINE * 3 + 3,
	LINE * 4 + 1,  LINE * 4 + 2};
uint8_t six[] = {1, 2, LINE, LINE * 2, LINE * 2 + 1, LINE * 2 + 2, LINE * 2 + 3,
	LINE * 3, LINE * 3 + 3, LINE * 4 + 1, LINE * 4 + 2};
uint8_t seven[] = {0, 1, 2, LINE + 3, LINE * 2 + 2,
	LINE * 3 + 1, LINE * 4 + 1};
uint8_t eight[] = {1, 2, LINE, LINE + 3, LINE * 2 + 1, LINE * 2 + 2,
	LINE * 3, LINE * 3 + 3, LINE * 4 + 1, LINE * 4 + 2};
uint8_t nine[] = {1, 2, LINE, LINE + 3, LINE * 2 + 1, LINE * 2 + 2, LINE * 2 + 3,
	LINE * 3 + 3, LINE * 4 + 1, LINE * 4 + 2};

static DECLARE_WAIT_QUEUE_HEAD(matrix_leds_waitq);
int ev_happen = 0;
char ev_code = '0';

struct matrix_leds_device *g_matrix_leds;
static int matrix_leds_get_fw_ver_info(struct matrix_leds_device *matrix_leds, u8 *fw_id, u32 len);
static int leds_update_video(struct matrix_leds_device *matrix_leds, const u8 *buf, u32 len);
static int matrix_leds_set_register_value(struct matrix_leds_device *matrix_leds, u16 reg_addr,u16 value);
static int matrix_leds_get_register_value(struct matrix_leds_device *matrix_leds,u16 reg_addr, u8 *reg_value, u32 len);
static int matrix_leds_play_picture(struct matrix_leds_device *matrix_leds, uint16_t* pic);


/*----------------------------------------------------------------------------------------------
* IO/Power Control
* ----------------------------------------------------------------------------------------------
*/

static inline const struct leds_video_data* get_leds_video_data_by_name(const char *name)
{
	const struct leds_video_data *video;
	int i;

	if (name == NULL) {
		return NULL;
	}

	video = leds_fac_video;
	for (i = 0; i < NUM_LEDS_VIDEOS; i++, video++) {
		if (strcmp(video->name, name) == 0) {
			return video;
		}
	}

	return NULL;
}

static int matrix_leds_spi_read_bytes(
	struct matrix_leds_device *matrix_leds,
	uint8_t *tx_buf, uint8_t *rx_buf, uint32_t data_len)
{
	int ret = 0;
	struct spi_device *spi_dev = matrix_leds->spi_dev;
	struct spi_message msg;
	struct spi_transfer xfer;
	struct spi_geni_qcom_ctrl_data delay_params = {
		.spi_cs_clk_delay = 500,
		.spi_inter_words_delay = 0,
	};

	spi_dev->controller_data = &delay_params;
	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = tx_buf;
	xfer.rx_buf = rx_buf;
	xfer.len	= data_len;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	ret = spi_sync(spi_dev, &msg);

	LOG_INFO(" read bytes led %d ret %d\n", data_len, ret);

	return ret;
}

static int matrix_leds_spi_write_bytes(
	struct matrix_leds_device *matrix_leds,
	uint8_t *tx_buf, uint32_t data_len)
{
	int ret = 0;
	struct spi_device *spi_dev = matrix_leds->spi_dev;
	struct spi_message msg;
	struct spi_transfer xfer;
	struct spi_geni_qcom_ctrl_data delay_params = {
		.spi_cs_clk_delay = 500,
		.spi_inter_words_delay = 0,
	};

	spi_dev->controller_data = &delay_params;
	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = tx_buf;
	xfer.len	= data_len;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	//pinctrl_select_state(matrix_leds->dev_pinctrl, matrix_leds->pinctrl_miso_spi);
	//msleep(5);
	ret = spi_sync(spi_dev, &msg);

	LOG_INFO("write bytes len %d, ret %d\n", data_len, ret);

	return ret;
}

static int matrix_leds_power_ctrl(
	struct matrix_leds_device *matrix_leds, bool on)
{

	int ret = 0;
	if (matrix_leds->power_on == on) {
		LOG_DBG("power_ctrl: already %d\n", on);
		return 0;
	}

	if (matrix_leds->fw_loading == 1) {
		LOG_INFO("fw loading not power off!\n");
		return 0;
	}

	matrix_leds->power_on = on;

#ifdef CONFIG_OF
	LOG_INFO("power_ctrl: power_on %d\n", on);

	if (matrix_leds->power_on) {
		pinctrl_select_state(matrix_leds->dev_pinctrl, matrix_leds->pinctrl_ldo_enable);
		matrix_leds->work_mode = LEDS_WORKING;
	#if IS_ENABLED(CONFIG_ARCH_MEDIATEK)
		mt_spi_enable_master_clk(matrix_leds->spi_dev);
	#endif
	} else {
		ret = matrix_leds_set_register_value(matrix_leds, 0x000D, 0x0055);
		if (ret < 0) {
			LOG_ERR("Failed to write register 0x000D before power down\n");
		}

		msleep(30);
		pinctrl_select_state(matrix_leds->dev_pinctrl, matrix_leds->pinctrl_ldo_disable);
		matrix_leds->work_mode = LEDS_NONE;
		matrix_leds->video_playing = false;
	#if IS_ENABLED(CONFIG_ARCH_MEDIATEK)
		mt_spi_disable_master_clk(matrix_leds->spi_dev);
	#endif
	}
	//if (matrix_leds->chip_id_byte == RFD_CHIP_ID) {
	//	msleep(500);
	//} else {
		msleep(40);
	//}
	return 0;
#else
	return 0;
#endif
}

static int matrix_leds_set_dump_reg(
	struct matrix_leds_device *matrix_leds, u32 reg_addr)
{
	int ret = 0;
	const size_t cmd_len = 16;

	u8 *tx_buf = kmalloc(cmd_len, GFP_KERNEL);
	if (!tx_buf) {
		LOG_ERR("Failed to allocate tx buffer\n");
		return -ENOMEM;
	}

	memset(tx_buf, 0, cmd_len);

	set_messge_header(tx_buf);
	tx_buf[2] = SPI_WRITE_FLAG;
	tx_buf[3] = SPI_WRITE_COMMAND;
	tx_buf[4] = COMMAND_DESC_SUB_0(SPI_WRITE_DEBUG_DESC);
	tx_buf[5] = COMMAND_DESC_SUB_1(SPI_WRITE_DEBUG_DESC);
	tx_buf[6] = COMMAND_DESC_SUB_2(SPI_WRITE_DEBUG_DESC);
	tx_buf[7] = COMMAND_DESC_SUB_3(SPI_WRITE_DEBUG_DESC);
	tx_buf[8] = COMMAND_DESC_SUB_0(DEBUG_MODE);
	tx_buf[9] = COMMAND_DESC_SUB_1(DEBUG_MODE);
	tx_buf[10] = COMMAND_DESC_SUB_0(reg_addr);
	tx_buf[11] = COMMAND_DESC_SUB_1(reg_addr);
	tx_buf[12] = COMMAND_DESC_SUB_2(reg_addr);
	tx_buf[13] = COMMAND_DESC_SUB_3(reg_addr);

	ret = matrix_leds_spi_write_bytes(matrix_leds, tx_buf, cmd_len);
	if (ret < 0) {
		LOG_ERR("Failed to read register 0x%08x\n", reg_addr);
		goto out_free;
	}

out_free:
	kfree(tx_buf);
	return ret;
}

static int matrix_leds_get_dump_reg(
	struct matrix_leds_device *matrix_leds, u32 reg_addr, u8 *reg_value, u32 len)
{
	int ret = 0;
	size_t data_offset = 10;
	const size_t cmd_len = 12;

	if (len > 1024) {
		LOG_ERR("Length must not exceed 1024 bytes\n");
		return -EINVAL;
	}

	u8 *tx_buf = kmalloc(1024 + cmd_len, GFP_KERNEL);
	if (!tx_buf) {
		LOG_ERR("Failed to allocate tx buffer\n");
		return -ENOMEM;
	}

	u8 *rx_buf = kmalloc(1024 + cmd_len, GFP_KERNEL);
	if (!rx_buf) {
		kfree(tx_buf);
		LOG_ERR("Failed to allocate rx buffer\n");
		return -ENOMEM;
	}

	memset(tx_buf, 0, 1024 + cmd_len);
	memset(rx_buf, 0, 1024 + cmd_len);

	set_messge_header(tx_buf);
	tx_buf[2] = SPI_READ_FLAG;
	tx_buf[3] = SPI_WRITE_COMMAND;
	tx_buf[4] = COMMAND_DESC_SUB_0(SPI_READ_DEBUG_DESC);
	tx_buf[5] = COMMAND_DESC_SUB_1(SPI_READ_DEBUG_DESC);
	tx_buf[6] = COMMAND_DESC_SUB_2(SPI_READ_DEBUG_DESC);
	tx_buf[7] = COMMAND_DESC_SUB_3(SPI_READ_DEBUG_DESC);
	tx_buf[8] = COMMAND_DESC_SUB_0(DEBUG_MODE);
	tx_buf[9] = COMMAND_DESC_SUB_1(DEBUG_MODE);
	tx_buf[10] = COMMAND_DESC_SUB_0(reg_addr);
	tx_buf[11] = COMMAND_DESC_SUB_1(reg_addr);
	tx_buf[12] = COMMAND_DESC_SUB_2(reg_addr);
	tx_buf[13] = COMMAND_DESC_SUB_3(reg_addr);

	ret = matrix_leds_spi_read_bytes(matrix_leds, tx_buf, rx_buf, 1024 + cmd_len);
	if (ret < 0) {
		LOG_ERR("Failed to read register 0x%08x\n", reg_addr);
		goto out_free;
	}

	memcpy(reg_value, rx_buf + data_offset - 1, len);

	{
		char log_buf[128];
		int pos = 0;

		pos += snprintf(log_buf + pos, sizeof(log_buf) - pos,
						"First %d bytes of reg_value @ 0x%08x: ", min(len, 20U), reg_addr);

		for (int i = 0; i < 20 && i < len; i++) {
			pos += snprintf(log_buf + pos, sizeof(log_buf) - pos, "%02X ", reg_value[i]);
			if ((i + 1) % 16 == 0 || i == 19 || i == len - 1) {
				LOG_INFO("%s", log_buf);
				pos = 0;
			}
		}
	}

out_free:
	kfree(tx_buf);
	kfree(rx_buf);
	return ret;
}

static int matrix_leds_get_register_value(
	struct matrix_leds_device *matrix_leds,u16 reg_addr, u8 *reg_value, u32 len)
{
	int ret = 0;
	u8 *tx_buf = NULL;
	//int tx_buf_len = 10;

	tx_buf = kzalloc(len * sizeof(u8), GFP_KERNEL);
	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	memset(tx_buf, 0x0, len);
	memset(reg_value, 0x0, len);
	set_messge_header(tx_buf);
	*(tx_buf + 2)= SPI_READ_FLAG;
	*(tx_buf + 3) = SPI_WRITE_COMMAND;
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(SPI_READ_COMMAND_DESC);
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(SPI_READ_COMMAND_DESC);
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(SPI_READ_COMMAND_DESC);
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(SPI_READ_COMMAND_DESC);
	*(tx_buf + 8) = COMMAND_DESC_SUB_0(reg_addr);
	*(tx_buf + 9) = COMMAND_DESC_SUB_1(reg_addr);
	ret = matrix_leds_spi_read_bytes(matrix_leds, tx_buf, reg_value, len);
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		LOG_ERR("Failed to read chip id info: %d\n", ret);
		goto cleanup;
	}

	LOG_DBG("reg: %02x value: [%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		matrix_leds->now_register,tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3],
		tx_buf[4], tx_buf[5], tx_buf[6], tx_buf[7],
		tx_buf[8], tx_buf[9], tx_buf[10]);

cleanup:
	kfree(tx_buf);
	return ret;
}

static int matrix_leds_set_register_value(
	struct matrix_leds_device *matrix_leds, u16 reg_addr,u16 value)
{
	int ret = 0;
	u8 *tx_buf = NULL;
	u32 len = 14;

	tx_buf = kzalloc(len, GFP_KERNEL);
	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	memset(tx_buf, 0x0, len);
	set_messge_header(tx_buf);
	*(tx_buf + 2)= SPI_WRITE_FLAG;
	*(tx_buf + 3) = SPI_READ_REG;
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 8) = COMMAND_DESC_SUB_0(reg_addr);
	*(tx_buf + 9) = COMMAND_DESC_SUB_1(reg_addr);
	*(tx_buf + 10) = COMMAND_DESC_SUB_0(value);
	*(tx_buf + 11) = COMMAND_DESC_SUB_1(value);
	//*(tx_buf + 9) = value;
	set_message_crc((tx_buf + 2), 10);

	LOG_DBG("set register value : [0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x] len %d\n",
		*(tx_buf), *(tx_buf + 1), *(tx_buf + 2), *(tx_buf + 3),
		*(tx_buf + 4), *(tx_buf + 5),
		*(tx_buf + 6), *(tx_buf + 7), *(tx_buf + 8), *(tx_buf + 9),*(tx_buf + 10),len);
	ret = matrix_leds_spi_write_bytes(matrix_leds, tx_buf, len);
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		LOG_ERR("Failed to read chip id info: %d\n", ret);
		goto cleanup;
	}

cleanup:
	kfree(tx_buf);
	return ret;
}

static int matrix_leds_get_chip_id_info(
	struct matrix_leds_device *matrix_leds, u8 *chip_id, u32 len)
{
	int ret = 0;
	u8 *tx_buf = NULL;

	tx_buf = kzalloc(len, GFP_KERNEL);
	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	memset(tx_buf, 0x0, len);
	memset(chip_id, 0x0, len);
	set_messge_header(tx_buf);
	*(tx_buf + 2)= SPI_READ_FLAG;
	*(tx_buf + 3) = SPI_READ_REG;
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(SPI_READ_CHIP_ID_DESC);
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(SPI_READ_CHIP_ID_DESC);
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(SPI_READ_CHIP_ID_DESC);
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(SPI_READ_CHIP_ID_DESC);
	*(tx_buf + 8) = COMMAND_DESC_SUB_0(LED_CHIP_ID_REG);
	*(tx_buf + 9) = COMMAND_DESC_SUB_1(LED_CHIP_ID_REG);
	//set_message_crc((tx_buf + 24), 22);
	ret = matrix_leds_spi_read_bytes(matrix_leds, tx_buf, chip_id, len);
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		LOG_ERR("Failed to read chip id info: %d\n", ret);
		goto cleanup;
	}

	LOG_INFO("chip id : [%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		chip_id[0], chip_id[1], chip_id[2], chip_id[3],
		chip_id[4], chip_id[5], chip_id[6], chip_id[7],
		chip_id[8], chip_id[9], chip_id[10], chip_id[11],
		chip_id[12], chip_id[13], chip_id[14], chip_id[15]);

cleanup:
	kfree(tx_buf);
	return ret;
}

struct matrix_leds_upgrade *fwupgrade;

static int leds_pram_write_data(const u8 *buf, u32 len)
{
	struct matrix_leds_upgrade *upg = fwupgrade;
	const size_t max_data_length = LED_MAX_DATA_LENGTH;
	size_t total_packets = (len + max_data_length - 1) / max_data_length;
	size_t current_packet = 0;
	int ret = 0;
	size_t offset = 0;
	uint8_t packet_info[4];

	for (; offset < len; ) {
		size_t data_length = (len - offset > max_data_length) ? max_data_length : len - offset;
		size_t packet_size = 8 + data_length + 2;

		uint8_t *tx_buf = kzalloc(packet_size, GFP_KERNEL);
		if (tx_buf == NULL) {
			LOG_ERR("leds_pram_write_ecc: no memory for SPI transfer\n");
			return -ENOMEM;
		}

		memcpy(packet_info, BUILD_PACKET_INFO(total_packets, current_packet + 1,
													data_length), sizeof(packet_info));
		set_messge_header(tx_buf);
		*(tx_buf + 2) = SPI_WRITE_FLAG;

		if (upg->ts_data->bootloader_upgrade) {
			*(tx_buf + 3) = SPI_WRITE_BOOT_TRANSFER;
		} else {
			*(tx_buf + 3) = SPI_WRITE_FW_TRANSFER;
		}

		*(tx_buf + 4) = packet_info[0];
		*(tx_buf + 5) = packet_info[1];
		*(tx_buf + 6) = packet_info[2];
		*(tx_buf + 7) = packet_info[3];
		memcpy((tx_buf + 8), buf + offset, data_length);
		set_message_crc((tx_buf + 2), (data_length + 6));

		LOG_DBG("total_packets is %zu, current_packet is %zu, data_length is %zu\n",
				total_packets, current_packet + 1, data_length);

		ret = matrix_leds_spi_write_bytes(upg->ts_data, tx_buf, packet_size);
		if (ret < 0) {
			upg->ts_data->io_fail_count ++;
			kfree(tx_buf);
			return ret;
		}
		kfree(tx_buf);
		offset += data_length;
		current_packet++;
		msleep(500);
	}
	return 0;
}

static int start_firmware_upgrade_and_crc(const u8 *buf, u32 len)
{
	int ret = 0;
	uint8_t *tx_buf = NULL;
	struct matrix_leds_upgrade *upg = fwupgrade;
	int buf_len = 12;
	uint16_t buf_crc = 0;

	tx_buf = kzalloc(buf_len, GFP_KERNEL);

	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	buf_crc = crc_16(buf, len);
	set_messge_header(tx_buf);
	*(tx_buf + 2) = SPI_WRITE_FLAG;
	if (upg->ts_data->bootloader_upgrade) {
		*(tx_buf + 3) = SPI_WRITE_BOOT_UPDATE;
	} else {
		*(tx_buf + 3) = SPI_WRITE_FW_UPDATE;
	}
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(SPI_WRITE_FW_UPDATE_MODE);
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(SPI_WRITE_FW_UPDATE_MODE);
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(SPI_WRITE_FW_UPDATE_MODE);
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(SPI_WRITE_FW_UPDATE_MODE);
	*(tx_buf + 8) = COMMAND_DESC_SUB_0(buf_crc);
	*(tx_buf + 9) = COMMAND_DESC_SUB_1(buf_crc);
	set_message_crc((tx_buf + 2), 8);

	LOG_INFO("firmware upgrade crc : [0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x-0x%x] len %d\n",
		*(tx_buf), *(tx_buf + 1), *(tx_buf + 2), *(tx_buf + 3),
		*(tx_buf + 4), *(tx_buf + 5),
		*(tx_buf + 6), *(tx_buf + 7), *(tx_buf + 8), *(tx_buf + 9),*(tx_buf + 10),*(tx_buf + 11),buf_len);
	ret = matrix_leds_spi_write_bytes(upg->ts_data, tx_buf, buf_len);
	if (ret < 0) {
		upg->ts_data->io_fail_count ++;
		kfree(tx_buf);
		return ret;
	}


	if (tx_buf != NULL) {
		kfree(tx_buf);
	}

	return ret;
}

static int leds_fw_write_start(const u8 *buf, u32 len)
{
	int ret = 0;
	struct matrix_leds_upgrade *upg = fwupgrade;

	LOG_INFO("begin to write and start fw(bin len:%d)", len);
	if (!upg || !upg->ts_data) {
		LOG_ERR("upgrade/ts_data is null");
		return -EINVAL;
	}

/*
	if (mode) {
		// enter into boot environment
		ret = leds_enter_into_boot(mode);
		if (ret < 0) {
		   LOG_ERR("enter into boot environment fail");
			return ret;
		}
	}
*/

	/* write pram */
	ret = leds_pram_write_data(buf, len);
	if (ret < 0) {
		LOG_ERR("write pram fail");
		return ret;
	}
	return ret;
}

static int leds_fw_download(const u8 *buf, u32 len, bool status)
{
	int ret = 0;
	int i = 0;
	struct matrix_leds_upgrade *upg = fwupgrade;

	LOG_INFO("fw upgrade download function");
	if (!upg || !upg->ts_data) {
		LOG_ERR("upgrade/ts_data is null");
		return -EINVAL;
	}

	if (!buf) {
		LOG_ERR("fw/len(%d) is invalid", len);
		return -EINVAL;
	}

	upg->ts_data->fw_loading = 1;

	for (i = 0; i < 3; i++) {
		LOG_DBG("fw download times:%d", i + 1);
		ret = leds_fw_write_start(buf, len);
		if (0 == ret)
			break;
	}

	if (i >= 3) {
		LOG_ERR("fw download fail");
		ret = -EIO;
		goto err_fw_download;
	}

	if (status == true) {
		ret = start_firmware_upgrade_and_crc(buf, len);
		if (ret < 0) {
			LOG_ERR("firmware upgrade fail");
			return ret;
		}
	} else {
		LOG_INFO("Only firmware parameters are passed, and firmware is not upgraded\n");
	}

err_fw_download:
	//upg->ts_data->fw_loading = 0;
	LOG_INFO("leds_fw_download end!");
	return ret;
}

static int matrix_leds_fwupg_get_fw_file(struct matrix_leds_upgrade *upg)
{
	int ret = 0;
	const struct firmware *fw = NULL;
	u8 *tmpbuf = NULL;
	char fwname[FILE_NAME_LENGTH] = { 0 };
	LOG_DBG("get upgrade fw file");
	if (upg->ts_data->bootloader_upgrade) {
		if (upg->ts_data->chip_id_byte == LTN_CHIP_ID)
			snprintf(fwname, FILE_NAME_LENGTH, "%s.bin",LEDS_LTN_BL_NAME_PREX_WITH_REQUEST);
		else if (upg->ts_data->chip_id_byte == RFD_CHIP_ID)
			snprintf(fwname, FILE_NAME_LENGTH, "%s.bin",LEDS_RFD_BL_NAME_PREX_WITH_REQUEST);
	} else {
		if (upg->ts_data->chip_id_byte == LTN_CHIP_ID)
			snprintf(fwname, FILE_NAME_LENGTH, "%s.bin",LEDS_LTN_FW_NAME_PREX_WITH_REQUEST);
		else if (upg->ts_data->chip_id_byte == RFD_CHIP_ID)
			snprintf(fwname, FILE_NAME_LENGTH, "%s.bin",LEDS_RFD_FW_NAME_PREX_WITH_REQUEST);
	}

	ret = request_firmware(&fw, fwname, upg->ts_data->dev);
	if (0 == ret) {
		LOG_INFO("firmware(%s) request successfully", fwname);
		tmpbuf = vmalloc(fw->size);
		if (NULL == tmpbuf) {
			LOG_ERR("fw buffer vmalloc fail");
			ret = -ENOMEM;
		} else {
			memcpy(tmpbuf, fw->data, fw->size);
			upg->fw = tmpbuf;
			upg->fw_length = fw->size;
			//upg->fw_from_request = 1;
		}
	} else {
		 LOG_ERR("firmware(%s) request fail,ret=%d", fwname, ret);
	}

	if (fw != NULL) {
		release_firmware(fw);
		fw = NULL;
	}
	LOG_DBG("upgrade fw file len:%d", upg->fw_length);
	return ret;
}

static int matrix_leds_compare_fw_version(struct matrix_leds_upgrade *upg)
{
	u32 fw_len = 16;
	u8 *fw_id = NULL;
	int ret = 0;
	unsigned char expected_version = 0;

	fw_id = kzalloc(fw_len, GFP_KERNEL);
	if (!fw_id) {
		LOG_ERR("Failed to allocate memory for firmware ID\n");
		return -ENOMEM;
	}

	ret = matrix_leds_get_fw_ver_info(upg->ts_data, fw_id, fw_len);
	if (ret < 0) {
		LOG_ERR("Failed to get firmware version info: %d\n", ret);
		goto free_memory;
	}

	switch (upg->ts_data->chip_id_byte) {
	case LTN_CHIP_ID:
		expected_version = LEDS_LTN_FW_VERSION;
		break;
	case RFD_CHIP_ID:
		expected_version = LEDS_RFD_FW_VERSION;
		break;
	default:
		LOG_ERR("Unsupported chip ID detected: %u\n", upg->ts_data->chip_id_byte);
		ret = -EALREADY;
		goto free_memory;
	}

	if ((fw_id[13] == 0x00) || fw_id[13] == 0xff) {
		LOG_ERR("SPI read fail or MCU FW loading...\n");
		ret = -EALREADY;
		goto free_memory;
	}

	if (fw_id[13] == expected_version) {
		LOG_INFO("Firmware version is the same, no need to update.\n");
		ret = 1; // Firmware versions match, no update needed
	} else {
		LOG_INFO("Firmware version differs, an update is required.\n");
		ret = 0; // Firmware versions differ, update needed
	}

free_memory:
	kfree(fw_id);
	return ret;
}

static int try_compare_fw_version(struct matrix_leds_upgrade *upg, int retries, int delay_ms)
{
	int ret;
	for (int i = 0; i < retries; i++) {
		ret = matrix_leds_compare_fw_version(upg);
		if (ret >= 0) {
			return ret;
		}
		msleep(delay_ms);
	}

	return -1; // Indicate failure after all retries
}

static void matrix_leds_fwupg_work(struct work_struct *work)
{
	int ret = 0;
	struct matrix_leds_upgrade *upg = fwupgrade;
	uint16_t black_pic[LEDS_PIXEL_TOTAL_COUNT] = {0};

	LOG_DBG("fw upgrade work function");
	if (!upg || !upg->ts_data) {
		LOG_ERR("upg/ts_data is null\n");
		return ;
	}

	/*send black picture*/
	LOG_DBG("set leds backlight black!");
	ret = matrix_leds_play_picture(upg->ts_data, black_pic);
	if (ret < 0) {
		LOG_ERR("Failed to send black picture\n");
    } else {
		LOG_DBG("Sent black picture successfully\n");
    }

	if (upg->ts_data->work_mode != LEDS_DEBUG_BREATHING_LIGHT) {
		/*Compare firmware version numbers*/
		ret = matrix_leds_compare_fw_version(upg);
		if (ret == 1) {// Firmware versions match, no update needed
			goto exit;
		}
	}

	/* get fw */
	ret = matrix_leds_fwupg_get_fw_file(upg);
	if (ret < 0) {
		LOG_ERR("get file fail, can't upgrade");
		return ;
	}

	if (upg->ts_data->fw_loading) {
		LOG_ERR("fw is loading, not download again");
		return ;
	}

	/*download fw*/
	ret = leds_fw_download(upg->fw, upg->fw_length,upg->ts_data->need_upgrade);
	if (ret < 0) {
		LOG_ERR("fw auto download failed");
	//} else {
		// msleep(50);
		//ret = leds_read_reg(FTS_REG_CHIP_ID, &chip_id);
		//LOG_INFO(DRV_	TAG "read chip id:0x%02x", chip_id);
	}

	msleep(1000);
	ret = try_compare_fw_version(upg, 10, 1000);
	if (ret < 0) {
		matrix_leds_power_ctrl(upg->ts_data, true);
		msleep(500);
		ret = try_compare_fw_version(upg, 10, 1000);
		if (ret < 0) {
			LOG_ERR("firmware upgrade failed after multiple attempts.");
		}
	}

exit:
	if (upg->ts_data->chip_id_byte == LTN_CHIP_ID) {
		LOG_INFO("video_data_loaded is %d.\n",upg->ts_data->video_data_loaded);
		if (!upg->ts_data->video_data_loaded) {
			const struct leds_video_data *video_data = get_leds_video_data_by_name("LED_FACTORY_VIDEO");
			if (video_data != NULL) {
				ret = leds_update_video(upg->ts_data, video_data->data, video_data->size);
				if (ret < 0) {
					LOG_ERR("Failed to update video data: %d\n", ret);
				}
				upg->ts_data->video_data_loaded = true;
				LOG_INFO("Video data successfully loaded.\n");
			} else {
				LOG_ERR("Failed to find video data by name\n");
			}
		} else {
			LOG_INFO("video data not need update!\n");
		}
	}
	upg->ts_data->bootloader_upgrade = false;
	upg->ts_data->need_upgrade = LEDS_TRANSMIT_FW_DATA;
	upg->ts_data->fw_loading = 0;
	matrix_leds_power_ctrl(upg->ts_data, false);

}

int matrix_leds_fwupg_init(struct matrix_leds_device *matrix_leds)
{

	LOG_INFO("fw upgrade init function\n");
	if(!matrix_leds || !matrix_leds->leds_workqueue) {
		LOG_ERR("matrix_leds/workqueue is NULL, can't run upgrade function\n");
	}

	fwupgrade = (struct matrix_leds_upgrade *)kzalloc(sizeof(*fwupgrade), GFP_KERNEL);
	if (NULL == fwupgrade) {
		LOG_ERR("malloc memory for upgrade fail");
		return -ENOMEM;
	}

	fwupgrade->ts_data = matrix_leds;
	INIT_WORK(&matrix_leds->fwupg_work, matrix_leds_fwupg_work);
	queue_work(matrix_leds->leds_workqueue, &matrix_leds->fwupg_work);
	return 0;
}

/*----------------------------------------------------------------------------------------------
* Function, releated the protocol
* ----------------------------------------------------------------------------------------------
*/
static int matrix_leds_get_fw_ver_info(
	struct matrix_leds_device *matrix_leds, u8 *fw_id, u32 len)
{
	int ret = 0;
	u8 *tx_buf = NULL;

	tx_buf = kzalloc(len, GFP_KERNEL);
	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	memset(tx_buf, 0x0, len);
	memset(fw_id, 0x0, len);
	set_messge_header(tx_buf);
	*(tx_buf + 2)= SPI_READ_FLAG;
	*(tx_buf + 3) = SPI_READ_FW_VER;
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(SPI_READ_FW_VER_DESC);
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(SPI_READ_FW_VER_DESC);
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(SPI_READ_FW_VER_DESC);
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(SPI_READ_FW_VER_DESC);
	//set_message_crc((tx_buf + 24), 22);
	ret = matrix_leds_spi_read_bytes(matrix_leds, tx_buf, fw_id, len);
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		LOG_ERR("Failed to read firmware version info: %d\n", ret);
		goto cleanup;
	}

	LOG_INFO("fw id [%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		fw_id[0], fw_id[1], fw_id[2], fw_id[3],
		fw_id[4], fw_id[5], fw_id[6], fw_id[7],
		fw_id[8], fw_id[9], fw_id[10], fw_id[11],
		fw_id[12], fw_id[13], fw_id[14], fw_id[15]);

cleanup:
	kfree(tx_buf);
	return ret;
}

static int matrix_leds_set_brightness_gain(
	struct matrix_leds_device *matrix_leds, uint16_t gain)
{
	uint8_t *tx_buf = NULL;
	int ret = 0;
	/*
	* header 2 + data-header 6 + regaddr + value + crc 2;
	*/
	int len = 14;

	tx_buf = kzalloc(len, GFP_KERNEL);

	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	set_messge_header(tx_buf);
	*(tx_buf + 2) = SPI_WRITE_FLAG;
	*(tx_buf + 3) = SPI_WRITE_BRIGHTNESS_GAIN;
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(SPI_WRITE_BRIGHTNESS_GAIN_DESC);
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(SPI_WRITE_BRIGHTNESS_GAIN_DESC);
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(SPI_WRITE_BRIGHTNESS_GAIN_DESC);
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(SPI_WRITE_BRIGHTNESS_GAIN_DESC);
	*(tx_buf + 8) = COMMAND_DESC_SUB_0(LED_BRIGHTNESS_GAIN_REG);
	*(tx_buf + 9) = COMMAND_DESC_SUB_1(LED_BRIGHTNESS_GAIN_REG);
	*(tx_buf + 10) = COMMAND_DESC_SUB_0(gain);
	*(tx_buf + 11) = COMMAND_DESC_SUB_1(gain);
	set_message_crc((tx_buf + 2), 10);

	LOG_DBG("brightness gain [%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3],
		tx_buf[4], tx_buf[5], tx_buf[6], tx_buf[7],
		tx_buf[8], tx_buf[9], tx_buf[10], tx_buf[11]);

	ret = matrix_leds_spi_write_bytes(matrix_leds, tx_buf, len);
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		kfree(tx_buf);
		return ret;
	}

	if (tx_buf != NULL) {
		kfree(tx_buf);
	}

	return ret;
}
/*
* Send single picture to matrix leds. will show the picture if host donot notify stop
*/
static int matrix_leds_play_picture(
	struct matrix_leds_device *matrix_leds, uint16_t* pic)
{
	uint8_t *tx_buf = NULL;
	int ret = 0;
	int i = 0;
	int offset = 0;
	size_t log_buf_size = 0;
	char *log_buf = NULL;

	/*
	* header 2 + data-header 6 + pixels 517 + crc 2;
	*/
	int len = 2 + LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT) + 6 + 2;

	tx_buf = kzalloc(len, GFP_KERNEL);

	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	set_messge_header(tx_buf);
	*(tx_buf + 2) = SPI_WRITE_FLAG;
	*(tx_buf + 3) = SPI_WRITE_PICTURE_PLAY;
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT));
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT));
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT));
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT));
	for(i =0; i< LEDS_PIXEL_TOTAL_COUNT; ++i) {
		*(tx_buf + 8 +i*2 +0) = COMMAND_DESC_SUB_0(pic[i]);
		*(tx_buf + 8 + i*2 +1) = COMMAND_DESC_SUB_1(pic[i]);
	}
	set_message_crc((tx_buf + 2), (LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT) + 6));

	log_buf_size =len * 5 + 100;
	log_buf = kzalloc(log_buf_size, GFP_KERNEL);
	if (!log_buf) {
		LOG_ERR("Failed to allocate memory for log buffer\n");
		return -ENOMEM;
	}

	offset += snprintf(log_buf + offset, log_buf_size - offset, "play_picture: tx_buf content (%d bytes): ", len);
	for (i = 0; i < len; i++) {
		offset += snprintf(log_buf + offset, log_buf_size - offset, "0x%02X", tx_buf[i]);
		if (i < len - 1) {
			offset += snprintf(log_buf + offset, log_buf_size - offset, " ");
		}
	}
	offset += snprintf(log_buf + offset, log_buf_size - offset, "\n");
	LOG_DBG("%s", log_buf);
	kfree(log_buf);

	ret = matrix_leds_spi_write_bytes(matrix_leds, tx_buf, len);
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		kfree(tx_buf);
		return ret;
	}

	if (tx_buf != NULL) {
		kfree(tx_buf);
	}

	return ret;
}


static int leds_update_video(struct matrix_leds_device *matrix_leds, const u8 *buf, u32 len)
{
	const size_t max_data_length = LED_MAX_DATA_LENGTH;
	size_t total_packets;
	size_t current_packet = 0;
	int ret = 0;
	size_t offset = 0;
	size_t data_length_bytes;
	size_t packet_size;
	uint8_t packet_info[4];
	uint8_t *tx_buf;

	total_packets = (len + max_data_length - 1) / max_data_length;

	for (; offset < len; ) {
		data_length_bytes = (len - offset > max_data_length) ? max_data_length : len - offset;
		packet_size = 8 + data_length_bytes + 2;

		tx_buf = kzalloc(packet_size, GFP_KERNEL);
		if (tx_buf == NULL) {
			LOG_ERR("leds_pram_write_ecc: no memory for SPI transfer\n");
			return -ENOMEM;
		}

		memcpy(packet_info, BUILD_PACKET_INFO(total_packets, current_packet + 1, data_length_bytes), sizeof(packet_info));
		set_messge_header(tx_buf);
		*(tx_buf + 2) = SPI_WRITE_FLAG;
		*(tx_buf + 3) = SPI_WRITE_VIDEO_CONTENT;
		*(tx_buf + 4) = packet_info[0];
		*(tx_buf + 5) = packet_info[1];
		*(tx_buf + 6) = packet_info[2];
		*(tx_buf + 7) = packet_info[3];
		memcpy(tx_buf + 8, buf + offset, data_length_bytes);
		set_message_crc((tx_buf + 2), (data_length_bytes + 6));

		LOG_DBG("total_packets is %zu, current_packet is %zu, data_length is %zu\n",
				total_packets, current_packet + 1, data_length_bytes);

		ret = matrix_leds_spi_write_bytes(matrix_leds, tx_buf, packet_size);
		if (ret < 0) {
			matrix_leds->io_fail_count ++;
			kfree(tx_buf);
			return ret;
		}

		kfree(tx_buf);
		offset += data_length_bytes;
		current_packet++;
		if (matrix_leds->chip_id_byte == LTN_CHIP_ID) {
			msleep(100);
		} else {
			msleep(10);
		}
	}

	return 0;
}

static int leds_video_play(struct matrix_leds_device *matrix_leds,u16 value)
{
	int ret = 0;
	uint8_t *tx_buf = NULL;
	int buf_len = 14;
	size_t log_buf_size = 0;
	char *log_buf = NULL;
	int offset = 0;

	tx_buf = kzalloc(buf_len, GFP_KERNEL);
	if (tx_buf == NULL) {
		LOG_ERR("play_picture: no memory for SPI transfer\n");
		return -ENOMEM;
	}

	LOG_DBG("Parsed hex value: 0x%04x\n", value);

	set_messge_header(tx_buf);
	*(tx_buf + 2) = SPI_WRITE_FLAG;
	*(tx_buf + 3) = SPI_WRITE_COMMAND;
	*(tx_buf + 4) = COMMAND_DESC_SUB_0(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 5) = COMMAND_DESC_SUB_1(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 6) = COMMAND_DESC_SUB_2(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 7) = COMMAND_DESC_SUB_3(SPI_WRITE_COMMAND_DESC);
	*(tx_buf + 8) = COMMAND_DESC_SUB_0(LED_VIDEO_PLAY_REG);
	*(tx_buf + 9) = COMMAND_DESC_SUB_1(LED_VIDEO_PLAY_REG);
	*(tx_buf + 10) = COMMAND_DESC_SUB_0(value);
	*(tx_buf + 11) = COMMAND_DESC_SUB_1(value);
	set_message_crc((tx_buf + 2), 10);

	log_buf_size = buf_len * 5 + 100;
	log_buf = kzalloc(log_buf_size, GFP_KERNEL);
	if (!log_buf) {
		LOG_ERR("Failed to allocate memory for log buffer\n");
		kfree(tx_buf);
		return -ENOMEM;
	}

	offset += snprintf(log_buf + offset, log_buf_size - offset, "video play start: ");
	for (int i = 0; i < buf_len; i++) {
		offset += snprintf(log_buf + offset, log_buf_size - offset, "0x%02X", tx_buf[i]);
		if (i < buf_len - 1) {
			offset += snprintf(log_buf + offset, log_buf_size - offset, " ");
		}
	}
	offset += snprintf(log_buf + offset, log_buf_size - offset, "\n");
	LOG_DBG("%s", log_buf);
	kfree(log_buf);

	ret = matrix_leds_spi_write_bytes(matrix_leds, tx_buf, buf_len);
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		kfree(tx_buf);
		return ret;
	}

	if (tx_buf != NULL) {
		kfree(tx_buf);
	}

	return ret;
}

static int matrix_leds_stop_play(
	struct matrix_leds_device *matrix_leds)
{
	int ret = 0;
	uint8_t tx_buf[4];

	set_messge_header(tx_buf);
	tx_buf[2] = SPI_WRITE_FLAG;
	tx_buf[3] = SPI_WRITE_STOP_PLAY;
	/*
	ret = matrix_leds_spi_write_bytes(matrix_leds, tx_buf, sizeof(tx_buf));
	if (ret < 0) {
		matrix_leds->io_fail_count ++;
		return ret;
	}*/

	return ret;
}

//static int matrix_leds_start_video(
//	struct matrix_leds_device *driver_dev, uint8_t mode)
//{
//	int ret = 0;
//	uint8_t tx_buf[9];
//
//	set_messge_header(tx_buf);
//	tx_buf[2] = SPI_WRITE_FLAG;
//	tx_buf[3] = SPI_WRITE_VIDEO_PLAY;
//	tx_buf[4] = COMMAND_DESC_SUB_0(SPI_WRITE_VIDEO_PLAY_MODE);
//	tx_buf[5] = COMMAND_DESC_SUB_1(SPI_WRITE_VIDEO_PLAY_MODE);
//	tx_buf[6] = COMMAND_DESC_SUB_2(SPI_WRITE_VIDEO_PLAY_MODE);
//	tx_buf[7] = COMMAND_DESC_SUB_3(SPI_WRITE_VIDEO_PLAY_MODE);
//
//	switch (mode) {
//		case 1:
//			tx_buf[8] = PLAY_ONCE;
//			break;
//		case 2:
//			tx_buf[8] = PLAY_TWICE;
//			break;
//		case 3:
//			tx_buf[8] = PLAY_FOREVER;
//			break;
//		default :
//			tx_buf[8] = PLAY_ONCE;
//			break;
//	}
//
//	ret = matrix_leds_spi_write_bytes(driver_dev, tx_buf, sizeof(tx_buf));
//
//	return ret;
//}

/*----------------------------------------------------------------------------------------------
* Device Attrs
* ----------------------------------------------------------------------------------------------
*/
static ssize_t matrix_leds_read_chip_id_show( struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	ssize_t len = 0;
	u32 chip_len = 26;
	u8 *chip_id = NULL;
	int ret = 0;
	int j = 0;
	u8 read_chip_id_byte = 0;

	chip_id = kzalloc(chip_len, GFP_KERNEL);
	if (!chip_id) {
		LOG_ERR("Failed to allocate memory for chip_id\n");
		return -ENOMEM;
	}

	matrix_leds_power_ctrl(matrix_leds, true);
	ret = matrix_leds_get_chip_id_info(matrix_leds, chip_id, chip_len);
	if (ret < 0) {
		LOG_ERR("Failed to get Chip id info: %d\n", ret);
		kfree(chip_id);
		return ret;
	}

	for (j = 0; j < chip_len; j++) {
		if (chip_id[j] == LTN_CHIP_ID || chip_id[j] == RFD_CHIP_ID) {
			read_chip_id_byte = chip_id[j];
			break;
		}
	}

	len += snprintf(buf + len, PAGE_SIZE - len, "Chip ID: %02X\n", read_chip_id_byte);
	kfree(chip_id);
	return len;
}

static ssize_t matrix_leds_fw_ver_info_show(
	struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	ssize_t len = 0;
	u32 fw_len = 16;
	u8 *fw_id = NULL;
	int ret = 0;
	int retries = 3;
	//u32 i = 0;

	fw_id = kzalloc(fw_len, GFP_KERNEL);
	if (!fw_id) {
		LOG_ERR("Failed to allocate memory for firmware ID\n");
		return -ENOMEM;
	}

	matrix_leds_power_ctrl(matrix_leds, true);

	while (retries > 0) {
		ret = matrix_leds_get_fw_ver_info(matrix_leds, fw_id, fw_len);
		if (ret < 0) {
			LOG_ERR("Failed to get firmware version info: %d\n", ret);
			kfree(fw_id);
			return ret;
		}
		if (matrix_leds->chip_id_byte == LTN_CHIP_ID && fw_id[13] == LEDS_LTN_FW_VERSION) {
			break;
		} else if (matrix_leds->chip_id_byte == RFD_CHIP_ID && fw_id[13] == LEDS_RFD_FW_VERSION) {
			break;
		}
		LOG_ERR("Failed to get firmware version: %02x\n", fw_id[13]);
		retries--;
		msleep(20);
	}

	len += snprintf(buf + len, PAGE_SIZE - len, "MCU Firmware Version: ");
	/*
	for (i = 0; i < fw_len && i < fw_len; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", fw_id[i]);
	}*/
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X",fw_id[13]);
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	if (matrix_leds->chip_id_byte == LTN_CHIP_ID) {
		len += snprintf(buf + len, PAGE_SIZE - len, "Software Firmware Version: %02X",LEDS_LTN_FW_VERSION);
	} else if (matrix_leds->chip_id_byte == RFD_CHIP_ID) {
		len += snprintf(buf + len, PAGE_SIZE - len, "Software Firmware Version: %02X",LEDS_RFD_FW_VERSION);
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	kfree(fw_id);
	return len;
}

static ssize_t matrix_leds_request_firmware_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	int val = 0;
	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	sscanf(buf, "%d", &val);
	LOG_DBG("request status is %d\n", val);

	matrix_leds_power_ctrl(matrix_leds, true);
	if (val == 0) {
		matrix_leds->work_mode = LEDS_NONE;
		LOG_DBG("Exit debug mode,Enter working mode");
	} else if(val == 1) {
		matrix_leds->need_upgrade = LEDS_TRANSMIT_FW_DATA;
		matrix_leds->work_mode = LEDS_DEBUG_BREATHING_LIGHT;
		queue_work(matrix_leds->leds_workqueue, &matrix_leds->fwupg_work);
	} else if (val == 2){
		matrix_leds->need_upgrade = LEDS_REFRESH_FW_DATA;
		matrix_leds->work_mode = LEDS_DEBUG_BREATHING_LIGHT;
		queue_work(matrix_leds->leds_workqueue, &matrix_leds->fwupg_work);
	} else {
		LOG_DBG("NOT request firmware\n");
	}

	__pm_relax(matrix_leds->leds_wakeup_source);
	return len;
}

static ssize_t matrix_leds_bootloader_ver_info_show(
	struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	ssize_t len = 0;
	u32 fw_len = 16;
	u8 *fw_id = NULL;
	int ret = 0;
	int retries = 3;
	//u32 i = 0;

	fw_id = kzalloc(fw_len, GFP_KERNEL);
	if (!fw_id) {
		LOG_ERR("Failed to allocate memory for firmware ID\n");
		return -ENOMEM;
	}

	matrix_leds_power_ctrl(matrix_leds, true);

	while (retries > 0) {
		ret = matrix_leds_get_fw_ver_info(matrix_leds, fw_id, fw_len);
		if (ret < 0) {
			LOG_ERR("Failed to get firmware version info: %d\n", ret);
			kfree(fw_id);
			return ret;
		}
		if (matrix_leds->chip_id_byte == LTN_CHIP_ID && fw_id[12] == LEDS_LTN_BOOTLOADER_VERSION) {
			break;
		} else if (matrix_leds->chip_id_byte == RFD_CHIP_ID && fw_id[12] == LEDS_RFD_BOOTLOADER_VERSION) {
			break;
		}
		LOG_ERR("Failed to get firmware version: %02x\n", fw_id[12]);
		retries--;
		msleep(20);
	}

	len += snprintf(buf + len, PAGE_SIZE - len, "MCU Bootloader Version: ");
	/*
	for (i = 0; i < fw_len && i < fw_len; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", fw_id[i]);
	}*/
	len += snprintf(buf + len, PAGE_SIZE - len, "%02X",fw_id[12]);
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	if (matrix_leds->chip_id_byte == LTN_CHIP_ID) {
		len += snprintf(buf + len, PAGE_SIZE - len, "Software Bootloader Version: %02X",LEDS_LTN_BOOTLOADER_VERSION);
	} else if (matrix_leds->chip_id_byte == RFD_CHIP_ID) {
		len += snprintf(buf + len, PAGE_SIZE - len, "Software Bootloader Version: %02X",LEDS_RFD_BOOTLOADER_VERSION);
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	kfree(fw_id);
	return len;
}

static ssize_t matrix_leds_request_bootloader_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	int val = 0;
	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	sscanf(buf, "%d", &val);
	LOG_DBG("request status is %d\n", val);

	matrix_leds_power_ctrl(matrix_leds, true);
	if (val == 0) {
		matrix_leds->bootloader_upgrade = false;
		matrix_leds->work_mode = LEDS_NONE;
		LOG_DBG("Exit debug mode,Enter working mode");
	} else if(val == 1) {
		matrix_leds->bootloader_upgrade = true;
		matrix_leds->need_upgrade = LEDS_TRANSMIT_FW_DATA;
		matrix_leds->work_mode = LEDS_DEBUG_BREATHING_LIGHT;

		queue_work(matrix_leds->leds_workqueue, &matrix_leds->fwupg_work);
	} else if (val == 2){
		matrix_leds->bootloader_upgrade = true;
		matrix_leds->need_upgrade = LEDS_REFRESH_FW_DATA;
		matrix_leds->work_mode = LEDS_DEBUG_BREATHING_LIGHT;
		queue_work(matrix_leds->leds_workqueue, &matrix_leds->fwupg_work);
	} else {
		LOG_DBG("NOT request firmware\n");
	}

	__pm_relax(matrix_leds->leds_wakeup_source);
	return len;
}
/*
* frame_brightness will recieve the one frame data (489 pixel member)
* Notice Upper layer use the MAX_NORMALIZATION_BRIGHTNESS here
*/
static ssize_t matrix_leds_frame_brightness_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	char ch = ' ';
	const char *p;
	uint16_t val = 0;
	int i = 0;
	uint16_t frame_brightness[LEDS_PIXEL_TOTAL_COUNT];
	int frame_num = 0;
	size_t log_buf_size = 0;
	char *log_buf = NULL;
	int offset = 0;

	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	frame_num = 0;
	if (sscanf(buf, "%hd", &val) == 1) {
		frame_brightness[frame_num] = LEDS_LIMIT_HW_MAX_BRIGHTNESS(val);
		p = strchr(buf, ch);
		while (p) {
			p = p + 1;
			if (sscanf(p, "%hd", &val) == 1) {
				frame_num++;
				frame_brightness[frame_num]= LEDS_LIMIT_HW_MAX_BRIGHTNESS(val);
				p = strchr(p, ch);
			} else {
				break;
			}
		}
		frame_num++;
	}

	log_buf_size =frame_num * 5 + 100;
	log_buf = kzalloc(log_buf_size, GFP_KERNEL);
	if (!log_buf) {
		LOG_ERR("Failed to allocate memory for log buffer\n");
		__pm_relax(matrix_leds->leds_wakeup_source);
		return -ENOMEM;
	}

	offset += snprintf(log_buf + offset, log_buf_size - offset, "frame_brightness: [");
	for (i = 0; i < frame_num; i++) {
		offset += snprintf(log_buf + offset, log_buf_size - offset, "%04X", frame_brightness[i]);
		if (i < frame_num - 1) {
			offset += snprintf(log_buf + offset, log_buf_size - offset, " ");
		}
	}

	offset += snprintf(log_buf + offset, log_buf_size - offset, "]\n");
	LOG_DBG("%s", log_buf);

	kfree(log_buf);

	LOG_INFO("frame_brightness %d var %d\n", frame_num, val);

	if (frame_num == 1) {
		for (i = 0; i < LEDS_PIXEL_TOTAL_COUNT; i++) {
			frame_brightness[i] = LEDS_LIMIT_HW_MAX_BRIGHTNESS(val);
		}
		matrix_leds_play_picture(matrix_leds, frame_brightness);
	} else if (frame_num == LEDS_PIXEL_TOTAL_COUNT) {
		matrix_leds_play_picture(matrix_leds, frame_brightness);
	}

	__pm_relax(matrix_leds->leds_wakeup_source);

	return len;
}

/*
* adds a brightness configuration
*/
static ssize_t matrix_leds_brightness_configuration_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	uint16_t gain = 0;
	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	if (sscanf(buf, "%hx", &gain) != 1) {
		LOG_ERR("Invalid input\n");
		goto exit;
	}

	LOG_INFO("test pic index %d\n", gain);
	if (gain > BACKLIGHT_GAIN_MAX) {
		gain = BACKLIGHT_GAIN_MAX;
	} else if (gain < BACKLIGHT_GAIN_MIN) {
		gain = BACKLIGHT_GAIN_MIN;
	}

	matrix_leds_set_brightness_gain(matrix_leds, gain);

exit:
	__pm_relax(matrix_leds->leds_wakeup_source);
	return len;
}

/*
* adds a default factory lamp bead control pattern
*/
static ssize_t matrix_leds_fac_test_pic_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	uint32_t val = 0;
	uint32_t array_index = 0;
	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	sscanf(buf, "%d", &val);
	LOG_DBG("test pic index %d\n", val);

	array_index = val - 1;
	if (array_index < 0 || array_index >= LEDS_FAC_TEST_PIC_COUNT) {
		array_index = 0;
	}

	LOG_INFO("Using test pic index %d (array index: %d)\n", val, array_index);
	matrix_leds_power_ctrl(matrix_leds, true);
	matrix_leds_play_picture(matrix_leds, fac_test_pic[array_index]);

	__pm_relax(matrix_leds->leds_wakeup_source);
	return len;
}

static ssize_t matrix_leds_video_play_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	uint16_t val = 0;
	//u32 reg_len = 26;
	//u8 *reg_value = NULL;
	int ret;

	__pm_stay_awake(matrix_leds->leds_wakeup_source);

/*
	reg_value = kzalloc(reg_len, GFP_KERNEL);
	if (!reg_value) {
		LOG_ERR("Failed to allocate memory for reg_value\n");
		__pm_relax(matrix_leds->leds_wakeup_source); // Ensure pm_relax is called before returning
		return -ENOMEM;
	}
*/
	/* Parse input value */
	if (sscanf(buf, "%hx", &val) != 1) {
		LOG_ERR("Failed to parse input value\n");
		goto free_and_exit;
	}

	LOG_DBG("Parsed hex value: 0x%04x\n", val);

	/* Power on control */
	matrix_leds_power_ctrl(matrix_leds, true);

	/*
	// Erase flash and wait for MCU ready
	matrix_leds_set_register_value(matrix_leds, LED_ERASE_FLASH_REG, 0);
	msleep(200);

	ret = matrix_leds_get_register_value(matrix_leds, LED_MCU_STATE_REG, reg_value, reg_len);
	if (ret < 0) {
		LOG_ERR("Failed to get reg info: %d\n", ret);
		goto free_and_exit;
	}

	// Check if MCU is ready
	while (*reg_value == 0) {
		msleep(200);
		LOG_INFO("MCU not ready, please wait...\n");

		ret = matrix_leds_get_register_value(matrix_leds, LED_MCU_STATE_REG, reg_value, reg_len);
		if (ret < 0) {
			LOG_ERR("Failed to get reg info during waiting: %d\n", ret);
			goto free_and_exit;
		}
	}
	*/

	/* Load and play video */
	if (matrix_leds->chip_id_byte == RFD_CHIP_ID) {
		const struct leds_video_data *video_data = get_leds_video_data_by_name("LED_FACTORY_VIDEO");
		if (video_data != NULL) {
			ret = leds_update_video(matrix_leds, video_data->data, video_data->size);
			if (ret < 0) {
				LOG_ERR("Failed to update video data: %d\n", ret);
				goto free_and_exit;
			}
			//matrix_leds->video_data_loaded = true;
			LOG_INFO("Video data successfully loaded.\n");
		} else {
			LOG_ERR("Failed to find video data by name\n");
			goto free_and_exit;
		}
	} else {
		if (!matrix_leds->video_data_loaded) {
			const struct leds_video_data *video_data = get_leds_video_data_by_name("LED_FACTORY_VIDEO");
			if (video_data != NULL) {
				ret = leds_update_video(matrix_leds, video_data->data, video_data->size);
				if (ret < 0) {
					LOG_ERR("Failed to update video data: %d\n", ret);
					goto free_and_exit;
				}
				matrix_leds->video_data_loaded = true;
				LOG_INFO("Video data successfully loaded.\n");
			} else {
				LOG_ERR("Failed to find video data by name\n");
				goto free_and_exit;
			}
		} else {
			LOG_INFO("video data not need update!\n");
		}
	}

	leds_video_play(matrix_leds, val);
	matrix_leds->video_playing = true;

free_and_exit:
	//kfree(reg_value);
	__pm_relax(matrix_leds->leds_wakeup_source);
	return len;
}


/*
*added for Read and write the current backlight leve
*/
static ssize_t matrix_led_register_operation_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	ssize_t len = 0;
	u32 reg_len = 0;
	u8 *reg_value = NULL;
	int ret = 0;
	u32 start_index = 0;

	switch (matrix_leds->now_register) {
		case LED_BRIGHTNESS_GAIN_REG:
		case LED_CHIP_ID_REG:
		case LED_DEBUG_LOG_REG:
		case LED_FW_UPDATE_STATES_REG:
		case LED_PICTURE_PLAY_REG:
		case LED_VIDEO_PLAY_REG:
		case LED_SPI_RATE_REG:
			reg_len = 12;
			break;
		case LED_OPEN_TEST_REG:
		case LED_SHORT_TEST_REG:
			reg_len = 89;
			break;
		default:
			LOG_ERR("Unsupported register: %d\n", matrix_leds->now_register);
			return -EINVAL;
	}

	if (reg_len > 0) {
		reg_value = kzalloc(reg_len * sizeof(u8), GFP_KERNEL);
		if (!reg_value) {
			LOG_ERR("Failed to allocate memory for reg_value\n");
			return -ENOMEM;
		}

		ret = matrix_leds_get_register_value(matrix_leds, matrix_leds->now_register, reg_value, reg_len);
		if (ret < 0) {
			LOG_ERR("Failed to get register value: %d\n", ret);
			kfree(reg_value);
			return ret;
		}
	}

	len += snprintf(buf + len, PAGE_SIZE - len, "REG VALUE: ");
	if (reg_value != NULL) {
		if (matrix_leds->now_register == LED_OPEN_TEST_REG ||
			matrix_leds->now_register == LED_SHORT_TEST_REG) {
			start_index = 10;
		}
		for (u32 i = start_index; i < reg_len; i++) {
			len += snprintf(buf + len, PAGE_SIZE - len, "%02X ", reg_value[i]);
		}
		kfree(reg_value);
	} else {
		len += snprintf(buf + len, PAGE_SIZE - len, "No data\n");
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "\n");

	return len;
}

static ssize_t matrix_led_register_operation_store(struct device *dev,
					struct device_attribute *attr,const char *buf, size_t len)
{
	int ret = 0;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	uint16_t reg_addr, value;
	//u8 reg_value[16] = {0};

	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	ret = sscanf(buf, "%hx %hx", &reg_addr, &value);
	if (ret == 2) {
		LOG_INFO("Writing to register %d with value %d\n", reg_addr, value);
		matrix_leds->now_register = reg_addr;
		matrix_leds_set_register_value(matrix_leds, reg_addr, value);
	} else {
		__pm_relax(matrix_leds->leds_wakeup_source);
		return -EINVAL;
	}

	if ((reg_addr == LED_VIDEO_PLAY_REG) && (value == 0))
		matrix_leds->video_playing = false;


	__pm_relax(matrix_leds->leds_wakeup_source);
	return len;
}

/*
*added for Read and write the current backlight leve
*/
static ssize_t matrix_leds_all_brightness_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);

	count = snprintf(buf, PAGE_SIZE,"%u\n", matrix_leds->now_brightness);

	return count;
}

static ssize_t matrix_leds_all_brightness_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	uint32_t val = 0;
	uint16_t pic[LEDS_PIXEL_TOTAL_COUNT] = {0};
	int i;

	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	sscanf(buf, "%d", &val);

	if (val > LED_MAX_HW_BRIGHTNESS)
		val = LED_MAX_HW_BRIGHTNESS;

	if (val)
		matrix_leds_power_ctrl(matrix_leds, true);

	matrix_leds->now_brightness = val;

	LOG_INFO("all_brightness %d\n", val);
	for (i = 0; i < LEDS_PIXEL_TOTAL_COUNT; i++)
		pic[i] = LEDS_VIDEO_BRIGHTNESS(val);

	matrix_leds_play_picture(matrix_leds, pic);

	if (!val)
		matrix_leds_power_ctrl(matrix_leds, false);

	__pm_relax(matrix_leds->leds_wakeup_source);

	return len;
}

static ssize_t matrix_leds_single_brightness_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);

	count = snprintf(buf, PAGE_SIZE,"%u\n", matrix_leds->now_single_brightness);

	return count;
}

static ssize_t matrix_leds_single_brightness_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	uint32_t val = 0;
	uint16_t pic[LEDS_PIXEL_TOTAL_COUNT] = {0};
	int index = 0;

	__pm_stay_awake(matrix_leds->leds_wakeup_source);

	if (sscanf(buf, "%d %d", &index, &val) != 2) {
		LOG_ERR("Invalid input format. Expected 'index brightness'\n");
		__pm_relax(matrix_leds->leds_wakeup_source);
		return -EINVAL;
	}

	if (index < 0 || index >= LEDS_PIXEL_TOTAL_COUNT) {
		LOG_ERR("Invalid LED index: %d (must be in range [0, %d])\n", index, LEDS_PIXEL_TOTAL_COUNT - 1);
		__pm_relax(matrix_leds->leds_wakeup_source);
		return -EINVAL;
	}

	if (val > LED_MAX_HW_BRIGHTNESS)
		val = LED_MAX_HW_BRIGHTNESS;

	if (val)
		matrix_leds_power_ctrl(matrix_leds, true);

	memset(pic, 0, sizeof(pic));

	pic[index] = LEDS_VIDEO_BRIGHTNESS(val);

	matrix_leds->now_single_brightness = val;

	LOG_INFO("Set single LED brightness: index=%d, brightness=%d\n", index, val);

	matrix_leds_play_picture(matrix_leds, pic);

	__pm_relax(matrix_leds->leds_wakeup_source);

	return len;
}

static ssize_t matrix_leds_power_on_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	uint32_t val = 0;
	struct timespec64 ts;
	struct rtc_time tm;

	__pm_stay_awake(matrix_leds->leds_wakeup_source);
	sscanf(buf, "%d", &val);

	ktime_get_real_ts64(&ts);
	rtc_time64_to_tm(ts.tv_sec, &tm);

	LOG_INFO("%s %d %d-%02d-%02d %02d:%02d:%02d.%09lu UTC\n", __func__, val,
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);

	if (val == 0) {
		matrix_leds_power_ctrl(matrix_leds, false);
	} else {
		matrix_leds_power_ctrl(matrix_leds, true);
	}

	__pm_relax(matrix_leds->leds_wakeup_source);

	return len;
}

static ssize_t matrix_leds_power_on_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);

	LOG_INFO("%s power_on is :%d\n", __func__, matrix_leds->power_on);

	return sprintf(buf, "%d\n", matrix_leds->power_on);
}

static ssize_t matrix_leds_always_on_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);

	LOG_INFO("%s always_on is :%d\n", __func__, matrix_leds->always_on);
	return sprintf(buf, "%d\n", matrix_leds->always_on);
}

static ssize_t matrix_leds_always_on_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	unsigned int val;

	sscanf(buf, "%d", &val);
	LOG_INFO("%s val:%d\n", __func__, val);

	matrix_leds->always_on = val;
	return len;
}

static ssize_t matrix_leds_trans_info_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);

	LOG_INFO("%s trans info is :%d\n", __func__, matrix_leds->io_fail_count);
	return sprintf(buf, "%d\n", matrix_leds->io_fail_count);
}

static ssize_t matrix_leds_trans_info_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct matrix_leds_device *matrix_leds = container_of(led_cdev, struct matrix_leds_device, cdev);
	unsigned int val;

	sscanf(buf, "%d", &val);
	LOG_INFO("%s val:%d\n", __func__, val);

	matrix_leds->io_fail_count = val;
	return len;
}

static ssize_t matrix_leds_debug_level_show(struct device *dev,struct device_attribute *attr,char *buf)
{
	LOG_INFO("%s log_level is :0x%X\n", __func__, log_level);
	return sprintf(buf, "0x%X\n", log_level);
}

static ssize_t matrix_leds_debug_level_store(struct device *dev,struct device_attribute *attr,const char *buf, size_t len)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret < 0) {
	LOG_ERR("%s: Invalid input, failed to parse log_level\n", __func__);
	return -EINVAL;	
	}

	LOG_INFO("%s val:0x%X (%d)\n", __func__, val, val);

	log_level = val;
	return len;
}

static ssize_t matrix_leds_light_id_info_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t len)
{
	int32_t var1 = 0, var2 = 0;
	struct timespec64 ts;
	struct rtc_time tm;

	if (sscanf(buf, "%d %d", &var1, &var2) == 2) {
		ktime_get_real_ts64(&ts);
		rtc_time64_to_tm(ts.tv_sec, &tm);
		LOG_INFO("LightID: %d on %d %d-%02d-%02d %02d:%02d:%02d.%09lu UTC\n",
			var1, var2, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
	}

	return len;
}

static DEVICE_ATTR(led_chip_id, 0444, matrix_leds_read_chip_id_show, NULL);
static DEVICE_ATTR(led_video_play, 0220, NULL, matrix_leds_video_play_store);
static DEVICE_ATTR(led_fac_test_pic, 0220, NULL, matrix_leds_fac_test_pic_store);
static DEVICE_ATTR(frame_brightness, 0220, NULL, matrix_leds_frame_brightness_store);
static DEVICE_ATTR(led_power_on, 0664, matrix_leds_power_on_show, matrix_leds_power_on_store);
static DEVICE_ATTR(trans_info, 0664, matrix_leds_trans_info_show, matrix_leds_trans_info_store);
static DEVICE_ATTR(led_always_on, 0664, matrix_leds_always_on_show, matrix_leds_always_on_store);
static DEVICE_ATTR(debug_level, 0664, matrix_leds_debug_level_show, matrix_leds_debug_level_store);
static DEVICE_ATTR(led_brightness_configuration, 0220, NULL, matrix_leds_brightness_configuration_store);
static DEVICE_ATTR(all_brightness, 0664, matrix_leds_all_brightness_show, matrix_leds_all_brightness_store);
static DEVICE_ATTR(firmware_version, 0664, matrix_leds_fw_ver_info_show, matrix_leds_request_firmware_store);
static DEVICE_ATTR(bootloader_version, 0664, matrix_leds_bootloader_ver_info_show, matrix_leds_request_bootloader_store);
static DEVICE_ATTR(single_brightness, 0664, matrix_leds_single_brightness_show, matrix_leds_single_brightness_store);
static DEVICE_ATTR(led_register_operation, 0664, matrix_led_register_operation_show, matrix_led_register_operation_store);
static DEVICE_ATTR(light_id_info, 0220, NULL, matrix_leds_light_id_info_store);

static struct attribute *matrix_leds_attributes[] = {
	&dev_attr_trans_info.attr,
	&dev_attr_debug_level.attr,
	&dev_attr_led_chip_id.attr,
	&dev_attr_led_power_on.attr,
	&dev_attr_led_always_on.attr,
	&dev_attr_led_video_play.attr,
	&dev_attr_all_brightness.attr,
	&dev_attr_single_brightness.attr,
	&dev_attr_led_fac_test_pic.attr,
	&dev_attr_frame_brightness.attr,
	&dev_attr_firmware_version.attr,
	&dev_attr_bootloader_version.attr,
	&dev_attr_led_register_operation.attr,
	&dev_attr_led_brightness_configuration.attr,
	&dev_attr_light_id_info.attr,
	NULL,
};

static struct attribute_group matrix_leds_attribute_group = {
	.attrs = matrix_leds_attributes
};

static void matrix_leds_clean_buf(
	struct matrix_leds_device * matrix_leds, int status)
{
	struct mmap_buf_format *opbuf = matrix_leds->start_buf;
	int i = 0;

	for (i = 0; i < LED_MMAP_BUF_SUM; i++) {
		memset(opbuf->data, 0, LED_MMAP_BUF_SIZE * 2);
		opbuf->status = status;
		opbuf = opbuf->kernel_next;
	}
	LOG_INFO("%s clean buffer@%d\n", __func__, matrix_leds->work_mode);

	matrix_leds_stop_play(matrix_leds);
}

static void matrix_leds_effect_work(
	struct work_struct *work)
{
	struct matrix_leds_device *matrix_leds = container_of(work, struct matrix_leds_device,
			leds_effect_work);
	uint32_t retry = 0;
	ktime_t start, runtime, delay;
	int i = 0, j = 0;
	uint16_t pic[LEDS_PIXEL_TOTAL_COUNT] = {0};

	LOG_INFO("%s work@%d\n", __func__, matrix_leds->work_mode);

	__pm_stay_awake(matrix_leds->leds_wakeup_source);
	matrix_leds->curr_buf = matrix_leds->start_buf;
	do {
		/*LOG_INFO("effect working IdxBuf@%d:%p status 0x%X length %d \n",
			matrix_leds->curr_buf->bit, matrix_leds->curr_buf, matrix_leds->curr_buf->status, matrix_leds->curr_buf->length);*/
		if (matrix_leds->curr_buf->status == MMAP_BUF_DATA_VALID) {
			if (matrix_leds->sec_num == LEDS_PIXEL_TOTAL_COUNT) {
				for (i = 0; i < matrix_leds->curr_buf->length; i += LEDS_PIXEL_TOTAL_COUNT) {
					start = ktime_get();
					LOG_INFO("effect working IdxBuf@%d consume %d/%d br %d\n",
						matrix_leds->curr_buf->bit, i, matrix_leds->curr_buf->length, matrix_leds->curr_buf->brightness);
					memcpy(pic, &matrix_leds->curr_buf->data[i], LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT));
					for (j = 0; j < LEDS_PIXEL_TOTAL_COUNT; j++) {
						pic[j] = pic[j] * matrix_leds->curr_buf->brightness / MAX_NORMALIZATION_BRIGHTNESS;
					}
					matrix_leds_play_picture(matrix_leds, pic);
					if (matrix_leds->work_mode != LEDS_WORKING) {
						LOG_INFO("break after play\n");
						break;
					}
					runtime = ktime_sub(ktime_get(), start);
					delay = 16666 * 1000 - runtime;//ns
					if (delay > 0) {
						usleep_range(delay/1000, delay/1000);
					}
					if (matrix_leds->work_mode != LEDS_WORKING) {
						LOG_INFO("break after next frame\n");
						break;
					}
				}
				if ((matrix_leds->curr_buf->length < LEDS_PIXEL_TOTAL_COUNT) || (matrix_leds->work_mode != LEDS_WORKING)) {
					LOG_INFO("break length@%d work_mode@%d\n", matrix_leds->curr_buf->length, matrix_leds->work_mode);
					break;
				}
			}
			matrix_leds->curr_buf->status = MMAP_BUF_DATA_INVALID;
			matrix_leds->curr_buf->length = 0;
			/*LOG_INFO("IdxBuf@%d:%p status 0x%X next@%d:%p 0x%X\n",
				matrix_leds->curr_buf->bit, matrix_leds->curr_buf, matrix_leds->curr_buf->status,
				matrix_leds->curr_buf->kernel_next->bit, matrix_leds->curr_buf->kernel_next, matrix_leds->curr_buf->kernel_next->status);*/
			matrix_leds->curr_buf = matrix_leds->curr_buf->kernel_next;
			retry = 0;
		} else if (matrix_leds->curr_buf->status == MMAP_BUF_DATA_FINISHED) {
			LOG_INFO("break buf status @finish\n");
			break;
		} else {
			if (matrix_leds->work_mode != LEDS_WORKING) {
				LOG_INFO("break work_mode@%d\n", matrix_leds->work_mode);
				break;
			}
			LOG_INFO("effect working IdxBuf@%d status 0x%X waiting\n", matrix_leds->curr_buf->bit, matrix_leds->curr_buf->status);
			msleep(1);
		}
		if (matrix_leds->work_mode != LEDS_WORKING) {
			LOG_INFO("break work_mode@%d\n", matrix_leds->work_mode);
			break;
		}
	} while (retry++ < 30);

	/*
	* Exit the effect work, make to default status
	*/
	LOG_INFO("%s update default brightness\n", __func__);
	for (j = 0; j < LEDS_PIXEL_TOTAL_COUNT; j++) {
		pic[j] = 0;
	}
	matrix_leds_play_picture(matrix_leds, pic);

	ev_happen = 1;
	ev_code = '1';
	LOG_INFO("%s ev_happen:%d ev_code:%c\n", __func__, ev_happen, ev_code);
	wake_up_interruptible(&matrix_leds_waitq);
	matrix_leds_clean_buf(matrix_leds, MMAP_BUF_DATA_FINISHED);
	__pm_relax(matrix_leds->leds_wakeup_source);
}

static unsigned int matrix_leds_poll(
	struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	LOG_INFO("%s ev_happen %d\n", __func__, ev_happen);

	poll_wait(file, &matrix_leds_waitq, wait);

	if (ev_happen == 1) {
		mask |= POLLIN | POLLRDNORM;
	}

	return mask;
}

static int matrix_leds_open(
	struct inode *inode, struct file *filp)
{
	return 0;
}

static int matrix_leds_release(
	struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t matrix_leds_read(
	struct file *file, char __user *user, size_t size, loff_t *ppos)
{
	int ret =0;

	LOG_INFO("%s ev_happen %d\n", __func__, ev_happen);

	if (size != 1)
		return -EINVAL;
	wait_event_interruptible(matrix_leds_waitq, ev_happen);
	ret = copy_to_user(user, &ev_code, 1);
	if (ret == 0) {
		ev_code = '0';
		ev_happen = 0;
		return 1;
	}
	ev_code = '0';
	ev_happen = 0;

	return ret;
}

static ssize_t matrix_leds_write(
	struct file *filp, const char __user *buf,
	size_t count, loff_t *f_pos)
{
	struct matrix_leds_device *matrix_leds = g_matrix_leds;
	int ret = 0;
	uint16_t *pic;

	LOG_INFO("matrix_leds_write buffer len:%zu\n", count);

	pic = kzalloc(LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT), GFP_KERNEL);

	if (pic == NULL) {
		LOG_ERR("matrix_leds_write picFromHal is null\n");
		return 1;
	}
	memset(pic, 0, LED_PIC_PIXEL_SIZE(LEDS_PIXEL_TOTAL_COUNT));
	if (count <= LEDS_PIXEL_TOTAL_COUNT) {
		ret = copy_from_user(pic, buf, count);
		if (ret) {
			LOG_ERR("matrix_leds_write can not copy data to kernel ret: %d\n", ret);
		} else {
			matrix_leds_play_picture(matrix_leds, pic);
		}
	}

	kfree(pic);
	pic = NULL;

	return count;
}

static inline unsigned long nt_arch_calc_vm_flag_bits(unsigned long flags)
{
	/*
	 * Only allow MTE on anonymous mappings as these are guaranteed to be
	 * backed by tags-capable memory. The vm_flags may be overridden by a
	 * filesystem supporting MTE (RAM-based).
	 */
	if (system_supports_mte() && (flags & MAP_ANONYMOUS))
		return VM_MTE_ALLOWED;

	return 0;
}

static inline unsigned long
__nt_calc_vm_flag_bits(unsigned long flags)
{
	return _calc_vm_trans(flags, MAP_GROWSDOWN,  VM_GROWSDOWN ) |
	       _calc_vm_trans(flags, MAP_LOCKED,     VM_LOCKED    ) |
	       _calc_vm_trans(flags, MAP_SYNC,	     VM_SYNC      ) |
	       nt_arch_calc_vm_flag_bits(flags);
}

static int matrix_leds_mmap(
	struct file *filp, struct vm_area_struct *vma)
{
	struct matrix_leds_device *matrix_leds = g_matrix_leds;
	unsigned long phys;
	int ret = 0;

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 7, 0)
	vm_flags_t vm_flags = calc_vm_prot_bits(PROT_READ|PROT_WRITE, 0) |
				__nt_calc_vm_flag_bits( MAP_SHARED);

	vm_flags |= current->mm->def_flags | VM_MAYREAD | VM_MAYWRITE |
			VM_MAYEXEC | VM_SHARED | VM_MAYSHARE;

	if (vma && (pgprot_val(vma->vm_page_prot) !=
			pgprot_val(vm_get_page_prot(vm_flags)))) {
		LOG_ERR("vm_page_prot error!");
		return -EPERM;
	}

	if (vma && ((vma->vm_end - vma->vm_start) !=
			(PAGE_SIZE << LED_MMAP_PAGE_ORDER))) {
		LOG_ERR("mmap size check err!");
		return -EPERM;
	}
#endif

	phys = virt_to_phys(matrix_leds->start_buf);

	ret = remap_pfn_range(vma, vma->vm_start, (phys >> PAGE_SHIFT), (vma->vm_end - vma->vm_start), vma->vm_page_prot);
	if (ret) {
		dev_err(matrix_leds->dev, "Error mmap failed\n");
		return ret;
	}

	LOG_INFO("%s start_buf 0x%p\n", __func__, matrix_leds->start_buf);
	/*
	{
		int i;
		struct mmap_buf_format *temp;
		temp = matrix_leds->start_buf;
		for (i = 0; i < LED_MMAP_BUF_SUM; i++) {
			LOG_INFO("IdxBuf@%d:%p status 0x%X next@%d:%p 0x%X\n",
				temp->bit, temp, temp->status, temp->kernel_next->bit, temp->kernel_next, temp->kernel_next->status);
			temp = temp->kernel_next;
		}
	}
	*/

	return ret;
}

long matrix_leds_ioctl(
	struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct matrix_leds_device *matrix_leds = g_matrix_leds;
	void __user *argp = (void __user *)arg;
	unsigned char always_on = 0;
	struct timespec64 ts;
	struct rtc_time tm;

	switch(cmd) {
		case LED_STRIPS_STREAM_MODE:
			if (copy_from_user(&matrix_leds->sec_num, argp, 2)) {
				return -EFAULT;
			}
			ktime_get_real_ts64(&ts);
			rtc_time64_to_tm(ts.tv_sec, &tm);
			LOG_INFO("STREAM_MODE %d %d-%02d-%02d %02d:%02d:%02d.%09lu UTC\n",
				matrix_leds->sec_num,
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
			matrix_leds->work_mode = LEDS_WORKING;
			//matrix_leds_power_ctrl(matrix_leds, true);
			/*
			{
				struct mmap_buf_format *temp;
				int i;
				for (i = 0; i < LED_MMAP_BUF_SUM; i++) {
					LOG_INFO("IdxBuf@%d:%p status 0x%X next@%d:%p 0x%X\n",
						temp->bit, temp, temp->status, temp->kernel_next->bit, temp->kernel_next, temp->kernel_next->status);
					temp = temp->kernel_next;
				}
			}
			*/
			queue_work(matrix_leds->leds_workqueue, &matrix_leds->leds_effect_work);
			break;
		case LED_STRIPS_STOP_MODE:
			LOG_INFO("ioctl LED_STRIPS_STOP_MODE\n");
			matrix_leds->work_mode = LEDS_NONE;
			//matrix_leds_power_ctrl(matrix_leds, false);
			matrix_leds_clean_buf(matrix_leds, MMAP_BUF_DATA_FINISHED);
			break;
		case LED_STRIPS_ALWAYS_ON:
			LOG_INFO("ioctl LED_STRIPS_ALWAYS_ON\n");
			if (copy_from_user(&always_on, argp, sizeof(always_on))) {
				return -EFAULT;
			}
			LOG_INFO("ioctl always_on: %d\n", always_on);
			if (always_on) {
				matrix_leds->always_on = true;
				LOG_INFO("always_on is %d\n",matrix_leds->always_on);
			} else {
				matrix_leds->always_on = false;
				LOG_INFO("always_on is %d\n",matrix_leds->always_on);
			}
			break;
		default:
			LOG_INFO("ioctl No match Mode.\n");
			break;
	}

	return 0;
}

static const struct file_operations matrix_leds_ioctl_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = matrix_leds_ioctl,
	.open = matrix_leds_open,
	.read = matrix_leds_read,
	.write = matrix_leds_write,
	.mmap = matrix_leds_mmap,
	.poll = matrix_leds_poll,
	.release = matrix_leds_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = matrix_leds_ioctl,
#endif
};

static struct miscdevice matrix_leds_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "matrix_leds",
	.fops = &matrix_leds_ioctl_fops,
};

static void matrix_leds_set_brightness(
	struct led_classdev *cdev,
	enum led_brightness brightness)
{

}

static int matrix_leds_parse_dts_info(
	struct matrix_leds_device *matrix_leds)
{
#ifdef CONFIG_OF
	struct device *dev = &(matrix_leds->spi_dev->dev);
	int ret = 0;

	matrix_leds->dev_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(matrix_leds->dev_pinctrl)) {
		ret = PTR_ERR(matrix_leds->dev_pinctrl);
		matrix_leds->dev_pinctrl = NULL;
		LOG_ERR("Can't get pin ctrl\n");
		return ret;
	}

	matrix_leds->pinctrl_ldo_enable = pinctrl_lookup_state(
		matrix_leds->dev_pinctrl, "matrix_leds_power_on");
	if (IS_ERR(matrix_leds->pinctrl_ldo_enable)) {
		ret = PTR_ERR(matrix_leds->pinctrl_ldo_enable);
		matrix_leds->pinctrl_ldo_enable = NULL;
		LOG_ERR("Can't get matrix_leds_power_on\n");
		return ret;
	}

	matrix_leds->pinctrl_ldo_disable = pinctrl_lookup_state(
		matrix_leds->dev_pinctrl, "matrix_leds_power_off");
	if (IS_ERR(matrix_leds->pinctrl_ldo_disable)) {
		ret = PTR_ERR(matrix_leds->pinctrl_ldo_disable);
		matrix_leds->pinctrl_ldo_disable = NULL;
		LOG_ERR("Can't get matrix_leds_power_off\n");
		return ret;
	}
/*
	matrix_leds->pinctrl_miso_spi = pinctrl_lookup_state(
		matrix_leds->dev_pinctrl, "miniled_miso_spi");
	if (IS_ERR(matrix_leds->pinctrl_miso_spi)) {
		ret = PTR_ERR(matrix_leds->pinctrl_miso_spi);
		matrix_leds->pinctrl_miso_spi = NULL;
		LOG_ERR("Can't get miniled_miso_spi\n");
		return ret;
	}
*/
	//pinctrl_select_state(matrix_leds->dev_pinctrl, matrix_leds->pinctrl_miso_spi);

	return ret;
#else
	return 0;
#endif
}

int led_get_ic_information(struct matrix_leds_device *matrix_leds)
{
	u32 chip_len = 26;
	u8 *chip_id = NULL;
	int ret, i,j;

	chip_id = kzalloc(chip_len, GFP_KERNEL);
	if (!chip_id) {
		LOG_ERR("Failed to allocate memory for chip_id\n");
		return -ENOMEM;
	}

	for (i = 0; i < 20; i++) {
		ret = matrix_leds_get_chip_id_info(matrix_leds, chip_id, chip_len);
		if (ret < 0) {
			LOG_ERR("Failed to get Chip id info: %d\n", ret);
			break;
		}
		for (j = 0; j < chip_len; j++) {
			if (chip_id[j] == LTN_CHIP_ID || chip_id[j] == RFD_CHIP_ID) {
				matrix_leds->chip_id_byte = chip_id[j];
				goto found;
			}
		}
	}

found:
	if (i == 20) {
		LOG_ERR("Failed to obtain correct chip ID after 3 attempts.\n");
		ret = -EIO;
	} else {
		LOG_INFO("Successfully obtained chip ID: 0x%02X.\n", matrix_leds->chip_id_byte);
	}

	kfree(chip_id);
	return ret;
}

static ssize_t matrix_led_proc_read(struct file *file, char __user *ubuf,
									size_t count, loff_t *ppos)
{
	int ret = 0;
	struct seq_file *m = file->private_data;
	struct matrix_proc_data *pdata = m->private;

	struct matrix_leds_device *matrix_leds = NULL;

	const u32 block_size = 1024;
	const u32 flash_addr = 0x08000000;
	const u32 sram_addr  = 0x20000000;
	const u32 flash_size = 128 * 1024;
	const u32 sram_size  = 32 * 1024;
	const u32 total_size = flash_size + sram_size;
	const u32 total_blocks = total_size / block_size;

	u32 current_block = 0;
	u32 offset = 0;
	u32 addr = 0;
	bool is_flash = false;

	char *hex_buffer = NULL;
	size_t output_len = 0;
	size_t copy_len = 0;

	if (!pdata || !pdata->matrix_leds) {
		LOG_ERR("Invalid private data or matrix_leds is NULL\n");
		return -EFAULT;
	}

	matrix_leds = pdata->matrix_leds;

	if (count < block_size * 2) {
		LOG_ERR("User buffer too small\n");
		return -EINVAL;
	}

	if (pdata->total_size < total_size) {
		LOG_ERR("Buffer too small to hold both FLASH and SRAM data\n");
		return -ENOMEM;
	}

	if (*ppos == 0 && pdata->cur_block == 0) {
		pdata->filled_size = 0;
	}

	if (pdata->cur_block >= total_blocks) {
		LOG_INFO("All blocks read, resetting to start.\n");
		pdata->cur_block = 0;
		return 0; // EOF
	}

	current_block = pdata->cur_block;
	offset = current_block * block_size;

	is_flash = (offset < flash_size);
	if (is_flash) {
		addr = flash_addr + offset;
		if ((addr + block_size) > (flash_addr + flash_size)) {
			LOG_ERR("Flash address overflow\n");
			return -EFAULT;
		}
	} else {
		addr = sram_addr + (offset - flash_size);
		if ((addr + block_size) > (sram_addr + sram_size)) {
			LOG_ERR("SRAM address overflow\n");
			return -EFAULT;
		}
	}

	matrix_leds_power_ctrl(matrix_leds, true);
	msleep(60);

	LOG_INFO("Reading block %u from 0x%08x (%u bytes)\n", current_block, addr, block_size);
	ret = matrix_leds_set_dump_reg(matrix_leds, addr);
	if (ret < 0) {
		LOG_ERR("Failed to write block %u data: %d\n", current_block, ret);
		return ret;
	}

	msleep(10);
	ret = matrix_leds_get_dump_reg(matrix_leds, addr,
								   pdata->data_buf + offset, block_size);
	if (ret < 0) {
		LOG_ERR("Failed to read block %u data: %d\n", current_block, ret);
		return ret;
	}

	pdata->cur_block++;
	pdata->filled_size = (current_block + 1) * block_size;

	hex_buffer = kmalloc(HEX_BUF_SIZE, GFP_KERNEL);
	if (!hex_buffer) {
		LOG_ERR("Failed to allocate hex buffer\n");
		return -ENOMEM;
	}

	output_len = 0;

	for (int i = 0; i < block_size; i += 32) {
		for (int j = 0; j < 32 && (i + j) < block_size; j++) {
			int remain = HEX_BUF_SIZE - output_len;
			if (remain < 3) {
				LOG_ERR("Buffer full, truncating at %zu bytes\n", output_len);
				goto copy_data;
			}

			u8 byte = pdata->data_buf[offset + i + j];
			snprintf(hex_buffer + output_len, remain, "%02x ", byte);
			output_len += 3;
		}

		if (HEX_BUF_SIZE - output_len > 1) {
			hex_buffer[output_len++] = '\n';
		}
	}

	hex_buffer[output_len] = '\0';

copy_data:
	copy_len = min(count, output_len);

	if (copy_to_user(ubuf, hex_buffer, copy_len)) {
		LOG_ERR("Failed to copy hex data to user space\n");
		kfree(hex_buffer);
		return -EFAULT;
	}

	*ppos += copy_len;

	kfree(hex_buffer);
	return copy_len;
}

static int matrix_led_proc_open(struct inode *inode, struct file *file)
{
	struct matrix_proc_data *pdata = pde_data(inode);
	return single_open(file, NULL, pdata);
}

static int matrix_led_proc_release(struct inode *inode, struct file *file)
{
	return single_release(inode, file);
}

static const struct proc_ops matrix_led_proc_ops = {
	.proc_open    = matrix_led_proc_open,
	.proc_read    = matrix_led_proc_read,
	.proc_lseek   = no_llseek,
	.proc_release = matrix_led_proc_release,
};

static int matrix_leds_spi_probe(struct spi_device *spi_dev)
{
	struct matrix_leds_device *matrix_leds;
	struct matrix_proc_data *pdata;
	int ret = 0;

	spi_dev->mode = SPI_MODE_0;
	spi_dev->bits_per_word = 8;
	spi_dev->max_speed_hz = MAX_SPI_FREQ_HZ;
	LOG_INFO("matrix_leds_spi_probe\n");
	matrix_leds = devm_kzalloc(&spi_dev->dev, sizeof(*matrix_leds), GFP_KERNEL);
	if (!matrix_leds) {
		LOG_ERR("Can't allocate SPI priv data\n");
		return -ENOMEM;
	}

	pdata = devm_kzalloc(&spi_dev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		LOG_ERR("Can't allocate pdata data\n");
		return -ENOMEM;
	}

	pdata->total_size = (128 + 32) * 1024;
	pdata->data_buf = devm_kmalloc(&spi_dev->dev, pdata->total_size, GFP_KERNEL);
	if (!pdata->data_buf) {
		LOG_ERR("Can't allocate data_buf data\n");
		return -ENOMEM;
	}

	pdata->filled_size = 0;
	strlcpy(pdata->name, "led_dump", sizeof(pdata->name));
	pdata->matrix_leds = matrix_leds;
	matrix_leds->spi_dev = spi_dev;
	matrix_leds->dev = &spi_dev->dev;
	spi_set_drvdata(spi_dev, matrix_leds);
	dev_set_drvdata(&spi_dev->dev, matrix_leds);

	/*
	* Parese the dts configuration
	*/
	ret = matrix_leds_parse_dts_info(matrix_leds);
	if (ret) {
		LOG_ERR("fail to parse dts info %d\n", ret);
		goto kfree_dev_mem;
	}

	/*
	* TODO:parse the vendor info,detect exist or not.
	*/
	if (spi_setup(spi_dev)) {
		LOG_ERR("fail to spi_setup %d\n", ret);
		goto kfree_dev_mem;
	}

	//DO IT
	//matrix_leds_power_ctrl(matrix_leds, false);

	/*
	* Init the matrix_leds_device members
	*/
	spin_lock_init(&matrix_leds->io_lock);
	matrix_leds->power_on = false;
	matrix_leds->work_mode = LEDS_NONE;
	matrix_leds->chip_id_byte = 0;

	matrix_leds_power_ctrl(matrix_leds, true);
	/*
	* TODO
	* Here the name can be releate vendor info
	*/
	matrix_leds->cdev.name = "matrix-leds";
	matrix_leds->cdev.brightness = 128;
	matrix_leds->cdev.max_brightness = 2047; /* Un-used-infomation */
	matrix_leds->cdev.brightness_set = matrix_leds_set_brightness;
	matrix_leds->need_upgrade = true;
	matrix_leds->bootloader_upgrade = false;
	matrix_leds->now_brightness = 0;
	matrix_leds->video_playing = false;
	matrix_leds->always_on = false;
	matrix_leds->fw_loading = 0;
	matrix_leds->video_data_loaded = false;
	matrix_leds->now_register = LED_CHIP_ID_REG;
	matrix_leds->leds_wakeup_source = wakeup_source_register(matrix_leds->dev, "matrix_leds");
	if (matrix_leds->leds_wakeup_source == NULL) {
		LOG_ERR("could not register wakeup source");
		goto kfree_dev_mem;
	}

	msleep(500);
	ret = led_get_ic_information(matrix_leds);
	if (ret) {
		LOG_ERR("not read CHIP IC, unregister driver");
		goto kfree_dev_mem;
	}

	pdata->entry = proc_create_data("matrix_led_dump", 0444, NULL, &matrix_led_proc_ops, pdata);
	if (!pdata->entry) {
		LOG_ERR("Failed to create proc entry\n");
		goto kfree_dev_mem;
	}

	ret = led_classdev_register(matrix_leds->dev, &(matrix_leds->cdev));
	if (ret) {
		LOG_ERR("fail to register led class %d\n", ret);
		goto kfree_dev_mem;
	}
	ret = sysfs_create_group(&(matrix_leds->cdev.dev->kobj), &matrix_leds_attribute_group);
	if (ret) {
		LOG_ERR("fail to create sysfs group %d\n", ret);
		goto kfree_dev_mem;
	}
	ret = misc_register(&matrix_leds_dev);
	if (ret) {
		dev_err(&spi_dev->dev, "%s: misc_register failed\n", __func__);
		goto kfree_dev_mem;
	}

	LOG_INFO("SPI Speed %dHz Mode %d(%d Bits)\n",
		spi_dev->max_speed_hz, spi_dev->mode, spi_dev->bits_per_word);

	/* get the virtural address */
	matrix_leds->start_buf = (struct mmap_buf_format *)__get_free_pages(GFP_KERNEL, LED_MMAP_PAGE_ORDER);
	if(matrix_leds->start_buf == NULL) {
		dev_err(&spi_dev->dev, "Error __get_free_pages failed\n");
	}

	LOG_INFO("start_buf 0x%p size %lu\n", matrix_leds->start_buf, sizeof(struct mmap_buf_format));

	SetPageReserved(virt_to_page(matrix_leds->start_buf));
	{
		struct mmap_buf_format *temp;
		uint32_t i = 0;
		temp = matrix_leds->start_buf;
		for (i = 1; i < LED_MMAP_BUF_SUM; i++) {
			temp->kernel_next = (matrix_leds->start_buf + i);
			temp = temp->kernel_next;
		}
		temp->kernel_next = matrix_leds->start_buf;

		temp = matrix_leds->start_buf;
		for (i = 0; i < LED_MMAP_BUF_SUM; i++) {
			temp->bit = i;
			temp->status = MMAP_BUF_DATA_INVALID;
			LOG_INFO("IdxBuf@%d:%p status 0x%X next@%d:%p 0x%X\n",
				temp->bit, temp, temp->status, temp->kernel_next->bit, temp->kernel_next, temp->kernel_next->status);
			temp = temp->kernel_next;
		}
	}

	matrix_leds->leds_workqueue = create_singlethread_workqueue("leds_wq");
	if (!matrix_leds->leds_workqueue) {
		LOG_ERR("create leds workqueue fail\n");
		goto kfree_dev_mem;
	} else {
		LOG_INFO("create work\n");
		INIT_WORK(&matrix_leds->leds_effect_work, matrix_leds_effect_work);
	}

	/*led fw upload*/
	ret = matrix_leds_fwupg_init(matrix_leds);
	if (ret) {
		LOG_ERR("matrix_leds_fwupg_init fail\n");
	}

	g_matrix_leds = matrix_leds;

	LOG_INFO("probe success\n");

	return 0;

kfree_dev_mem:
	if (matrix_leds->start_buf) {
		ClearPageReserved(virt_to_page(matrix_leds->start_buf));
		free_pages((unsigned long)matrix_leds->start_buf, LED_MMAP_PAGE_ORDER);
	}

	if (pdata && pdata->entry) {
		remove_proc_entry("matrix_led_dump", NULL);
	}

	matrix_leds_power_ctrl(matrix_leds, false);
	devm_kfree(&spi_dev->dev, matrix_leds);
	devm_kfree(&spi_dev->dev, pdata);
	LOG_ERR("probe fail\n");

	return ret;
}

static void matrix_leds_spi_remove(struct spi_device *spi_dev)
{
	struct matrix_leds_device *matrix_leds = spi_get_drvdata(spi_dev);
	struct matrix_proc_data *pdata = dev_get_drvdata(&spi_dev->dev);

	if (matrix_leds->leds_wakeup_source)
		wakeup_source_unregister(matrix_leds->leds_wakeup_source);

	sysfs_remove_group(&(matrix_leds->cdev.dev->kobj), &matrix_leds_attribute_group);
	led_classdev_unregister(&matrix_leds->cdev);

	flush_workqueue(matrix_leds->leds_workqueue);
	destroy_workqueue(matrix_leds->leds_workqueue);

	ClearPageReserved(virt_to_page(matrix_leds->start_buf));
	free_pages((unsigned long)matrix_leds->start_buf, LED_MMAP_PAGE_ORDER);
	misc_deregister(&matrix_leds_dev);

	if (pdata && pdata->entry) {
		remove_proc_entry("matrix_led_dump", NULL);
	}

	matrix_leds_power_ctrl(matrix_leds, false);
	devm_kfree(&spi_dev->dev, matrix_leds);
	devm_kfree(&spi_dev->dev, pdata);

	LOG_INFO("remove success\n");

}

static void matrix_led_suspend_marker(char *annotation)
{
	struct timespec64 ts;
	struct rtc_time tm;

	ktime_get_real_ts64(&ts);
	rtc_time64_to_tm(ts.tv_sec, &tm);
	LOG_INFO("PM: suspend %s %d-%02d-%02d %02d:%02d:%02d.%09lu UTC\n",
		annotation, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
}

static int matrix_leds_spi_suspend(struct device *dev)
{
	struct matrix_leds_device *matrix_leds = dev_get_drvdata(dev);

	LOG_INFO("suspend video_playing %d always_on %d\n", matrix_leds->video_playing, matrix_leds->always_on);
	matrix_led_suspend_marker("entry");

	if ((matrix_leds->video_playing) || (matrix_leds->always_on)) {
		/* * Ignore if playing video or always on */
	} else {
		matrix_leds_power_ctrl(matrix_leds, false);
	}

	return 0;
}

static int matrix_leds_spi_resume(struct device *dev)
{
	struct matrix_leds_device *matrix_leds = dev_get_drvdata(dev);

	LOG_INFO("resume video_playing %d always_on %d\n", matrix_leds->video_playing, matrix_leds->always_on);
	matrix_led_suspend_marker("exit");

	return 0;
}

static const struct dev_pm_ops matrix_leds_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(matrix_leds_spi_suspend, matrix_leds_spi_resume)
};

#ifdef CONFIG_OF
static const struct of_device_id matrix_leds_of_match[] = {
	{.compatible = "Nothing,matrix_leds_spi"},
	{},
};
MODULE_DEVICE_TABLE(of, matrix_leds_of_match);
#endif

static struct spi_driver matrix_leds_spi_driver = {
	.probe = matrix_leds_spi_probe,
	.remove = matrix_leds_spi_remove,
	.driver = {
		.name		= "matrix_leds_spi",
		.owner = THIS_MODULE,
	#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(matrix_leds_of_match),
	#endif
		.pm = &matrix_leds_pm_ops,
	},
};

module_spi_driver(matrix_leds_spi_driver);

MODULE_DESCRIPTION("Matrix-LEDs Driver");
MODULE_LICENSE("GPL v2");
