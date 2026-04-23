// SPDX-License-Identifier: GPL-2.0
#include "aw9380x.h"
#include "aw9380x_bin.h"
#include "aw9380x_reg.h"

#include <linux/rtc.h>
#include <linux/timekeeping.h>

#define AW9380X_DRIVER_VERSION "v0.5.1"
#define AW9380X_I2C_NAME "aw9380x_cap"
#define AW9380X_RW_RETRY_TIME_MIN (2000)
#define AW9380X_RW_RETRY_TIME_MAX (3000)
#define AW9380X_IRQ_STATE_TIME (1000)
#define AW9380X_TIME_DEBOUNCE_TIME (1000)
#define AW9380X_REG_STEP (0x400 - 0x250)

static struct mutex aw9380x_lock;
static uint32_t g_diff_cal_val;
struct aw9380x_cap *g_p_cap;
static struct wakeup_source *aw9380x_wakeup_source;
static void aw9380x_fct_cali_coef_def_read(struct aw9380x_cap *p_cap);
static void aw9380x_fct_data_write_back_to_fw(struct aw9380x_cap *p_cap);
static irqreturn_t aw9380x_cap_default_irq_handle(int32_t irq, void *data);
static uint32_t awlog_time = 25;  //ms
static int32_t gpio_low_count = 0;

static const struct aw9380x_pm_info g_aw9380x_pm_info = {
	.resume_set_mode = AW9380X_ACTIVE_MODE,
	.suspend_set_mode = AW9380X_SLEEP_MODE,
	.shutdown_set_mode = AW9380X_SLEEP_MODE,
};

static const struct aw9380x_reg_list g_aw9380x_reg_list = {
	.reg_none_access = REG_NONE_ACCESS,
	.reg_rd_access = REG_RD_ACCESS,
	.reg_wd_access = REG_WR_ACCESS,
	.reg_perm = (struct aw9380x_reg_data *)&g_aw9380x_reg_access[0],
	.reg_num = ARRAY_SIZE(g_aw9380x_reg_access),
};

static const struct aw9380x_chip_mode_info g_aw9380x_chip_mode_info = {
	.active = AW9380X_ACTIVE_MODE,
	.pre_init_mode = AW9380X_SLEEP_MODE,
	.init_mode = AW9380X_ACTIVE_MODE,
};

static struct aw9380x_bin_info g_aw9380x_code_ram_info = {
	.bin_name = "aw9380x_code_ram.bin",
	.bin_data_ver = 0,
};

static struct aw9380x_bin_info g_aw9380x_reg_bin_info = {
	.bin_name = "aw9380x_reg.bin",
	.bin_data_ver = 0,
};

static const struct aw9380x_diff g_aw9380x_diff_info = {
	.diff0_reg = REG_DIFF_CH0,
	.diff_step = (REG_DIFF_CH1 - REG_DIFF_CH0),
	.rm_float = AW9380X_DATA_PROCESS_FACTOR};

static const struct aw9380x_para_info g_aw9380x_para_info = {
	.reg_arr = aw9380x_default_reg,
	.reg_arr_len = ARRAY_SIZE(aw9380x_default_reg),
};

static struct aw9380x_irq_cfg g_aw9380x_irq_cfg = {
	.flags = GPIOF_DIR_IN | GPIOF_INIT_HIGH,
	.irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
	.handler = NULL,
	.thread_fn = NULL,
};

static int32_t awinic_i2c_write(struct i2c_client *i2c, uint8_t *tr_data, uint16_t len)
{
	struct i2c_msg msg;

	msg.addr = i2c->addr;
	msg.flags = 0;
	msg.len = len;
	msg.buf = tr_data;

	return i2c_transfer(i2c->adapter, &msg, 1);
}

static int32_t awinic_i2c_read(struct i2c_client *i2c, uint8_t *addr,
		uint8_t addr_len, uint8_t *data, uint16_t data_len)
{
	struct i2c_msg msg[2];

	msg[0].addr = i2c->addr;
	msg[0].flags = 0;
	msg[0].len = addr_len;
	msg[0].buf = addr;

	msg[1].addr = i2c->addr;
	msg[1].flags = 1;
	msg[1].len = data_len;
	msg[1].buf = data;

	return i2c_transfer(i2c->adapter, msg, 2);
}

uint32_t aw9380x_sar_pow2(uint32_t cnt)
{
	uint32_t i = 0;
	uint32_t sum = 1;

	if (cnt == 0) {
		sum = 1;
	} else {
		for (i = 0; i < cnt; i++)
			sum *= 2;
	}

	return sum;
}

static void aw9380x_disable_irq(struct aw9380x_cap *p_cap)
{
	if (p_cap->irq_init.host_irq_stat == IRQ_ENABLE) {
		disable_irq(p_cap->irq_init.to_irq);
		p_cap->irq_init.host_irq_stat = IRQ_DISABLE;
	}
}

static void aw9380x_enable_irq(struct aw9380x_cap *p_cap)
{
	if (p_cap->irq_init.host_irq_stat == IRQ_DISABLE) {
		enable_irq(p_cap->irq_init.to_irq);
		p_cap->irq_init.host_irq_stat = IRQ_ENABLE;
	}
}

static int32_t aw9380x_i2c_read(struct i2c_client *i2c, uint16_t reg_addr16, uint32_t *reg_data32)
{
	int8_t cnt = AW9380X_I2C_RETRIES;
	int32_t ret = -1;
	uint8_t r_buf[6] = {0};

	if (i2c == NULL)
		return -AW_ERR;

	r_buf[0] = (unsigned char)(reg_addr16 >> OFFSET_BIT_8);
	r_buf[1] = (unsigned char)(reg_addr16);

	do {
		ret = awinic_i2c_read(i2c, r_buf, 2, &r_buf[2], 4);
		if (ret < 0)
			AWLOGE(&i2c->dev, "i2c read error reg: 0x%04x, ret= %d cnt= %d", reg_addr16, ret, cnt);
		else
			break;
		usleep_range(2000, 3000);
	} while (cnt--);

	if (cnt < 0) {
		AWLOGE(&i2c->dev, "i2c read error!");
		return -AW_ERR;
	}

	*reg_data32 = ((uint32_t)r_buf[5] << OFFSET_BIT_0) | ((uint32_t)r_buf[4] << OFFSET_BIT_8) |
				  ((uint32_t)r_buf[3] << OFFSET_BIT_16) | ((uint32_t)r_buf[2] << OFFSET_BIT_24);

	return AW_OK;
}

static int32_t aw9380x_i2c_write(struct i2c_client *i2c, uint16_t reg_addr16, uint32_t reg_data32)
{
	int8_t cnt = AW9380X_I2C_RETRIES;
	int32_t ret = -1;
	uint8_t w_buf[6] = {0};

	if (i2c == NULL)
		return -AW_ERR;

	/*reg_addr*/
	w_buf[0] = (uint8_t)(reg_addr16 >> OFFSET_BIT_8);
	w_buf[1] = (uint8_t)(reg_addr16);
	/*data*/
	w_buf[2] = (uint8_t)(reg_data32 >> OFFSET_BIT_24);
	w_buf[3] = (uint8_t)(reg_data32 >> OFFSET_BIT_16);
	w_buf[4] = (uint8_t)(reg_data32 >> OFFSET_BIT_8);
	w_buf[5] = (uint8_t)(reg_data32);

	do {
		ret = awinic_i2c_write(i2c, w_buf, ARRAY_SIZE(w_buf));
		if (ret < 0) {
			AWLOGE(&i2c->dev, "i2c write error reg: 0x%04x data: 0x%08x, ret= %d cnt= %d",
				   reg_addr16, reg_data32, ret, cnt);
		} else {
			break;
		}
	} while (cnt--);

	if (cnt < 0) {
		AWLOGE(&i2c->dev, "i2c write error!");
		return -AW_ERR;
	}

	return AW_OK;
}

static int32_t aw9380x_i2c_write_bits(struct i2c_client *i2c, uint16_t reg_addr16, uint32_t mask, uint32_t val)
{
	uint32_t reg_val;

	aw9380x_i2c_read(i2c, reg_addr16, &reg_val);
	reg_val &= mask;
	reg_val |= (val & (~mask));
	aw9380x_i2c_write(i2c, reg_addr16, reg_val);

	return AW_OK;
}

static int32_t aw9380x_i2c_read_seq(struct i2c_client *i2c, uint8_t *addr,
		uint8_t addr_len, uint8_t *data, uint16_t data_len)
{
	int8_t cnt = AW9380X_I2C_RETRIES;
	int32_t ret = -AW_ERR;

	do {
		ret = awinic_i2c_read(i2c, addr, addr_len, data, data_len);
		if (ret < 0)
			AWLOGE(&i2c->dev, "aw9380x cap i2c write error %d", ret);
		else
			break;
		usleep_range(AW9380X_RW_RETRY_TIME_MIN, AW9380X_RW_RETRY_TIME_MAX);
	} while (cnt--);

	if (cnt < 0) {
		AWLOGE(&i2c->dev, "aw9380x cap i2c read error!");
		return -AW_ERR;
	}

	return AW_OK;
}

static int32_t aw9380x_i2c_write_seq(struct i2c_client *i2c, uint8_t *tr_data, uint16_t len)
{
	int8_t cnt = AW9380X_I2C_RETRIES;
	int32_t ret = -AW_ERR;

	do {
		ret = awinic_i2c_write(i2c, tr_data, len);
		if (ret < 0)
			AWLOGE(&i2c->dev, "aw9380x cap write seq error %d", ret);
		else
			break;
		usleep_range(AW9380X_RW_RETRY_TIME_MIN, AW9380X_RW_RETRY_TIME_MAX);
	} while (cnt--);

	if (cnt < 0) {
		AWLOGE(&i2c->dev, "aw9380x cap write error!");
		return -AW_ERR;
	}

	return AW_OK;
}

static int32_t aw9380x_regulator_is_get_voltage(struct aw9380x_cap *p_cap)
{
	uint32_t cnt = 10;
	int32_t voltage_val = 0;

	while (cnt--) {
		voltage_val = regulator_get_voltage(p_cap->vcc);
		AWLOGE(p_cap->dev, "aw9380x voltage is : %d uv", voltage_val);
		if (voltage_val >= AW9380X_VCC_MIN_UV)
			return AW_OK;
		mdelay(1);
	}

	msleep(20);

	return -AW_VERS_ERR;
}

static void aw9380x_set_cs_as_irq(struct aw9380x_cap *p_cap, int flag)
{
	if (flag == AW9380X_CS2_IRQ) {
		aw9380x_i2c_write(p_cap->i2c, 0xfff4, 0x3c00d013);
		aw9380x_i2c_write(p_cap->i2c, 0xc100, 0x00000020);
		aw9380x_i2c_write(p_cap->i2c, 0xe018, 0x00000004);
	} else if (flag == AW9380X_CS5_IRQ) {
		aw9380x_i2c_write(p_cap->i2c, 0xfff4, 0x3c00d013);
		aw9380x_i2c_write(p_cap->i2c, 0xc100, 0x00000800);
		aw9380x_i2c_write(p_cap->i2c, 0xe018, 0x00000020);
	} else {
		aw9380x_i2c_write(p_cap->i2c, 0xfff4, 0x3c00d013);
		aw9380x_i2c_write(p_cap->i2c, 0xc100, 0x00000000);
		aw9380x_i2c_write(p_cap->i2c, 0xe018, 0x00000000);
	}
}

static void aw9380x_mode_set(struct aw9380x_cap *p_cap, enum aw9380x_op_mode mode)
{
	uint32_t reg_data = 0;

	if (mode == AW9380X_ACTIVE_MODE) {
		if (p_cap->last_mode == AW9380X_DEEPSLEEP_MODE) {
			aw9380x_i2c_write_bits(p_cap->i2c, REG_CHIPSTAT,
					~AW9380X_CPU_OSC_CTRL_MASK, AW9380X_CPU_OSC_CTRL_MASK);
		}

		aw9380x_i2c_write(p_cap->i2c, REG_CMD, AW9380X_ACTIVE_MODE);
	} else if (mode == AW9380X_SLEEP_MODE) {
		if (p_cap->last_mode == AW9380X_DEEPSLEEP_MODE) {
			aw9380x_i2c_write_bits(p_cap->i2c, REG_CHIPSTAT,
					~AW9380X_CPU_OSC_CTRL_MASK, AW9380X_CPU_OSC_CTRL_MASK);
		}
		aw9380x_i2c_write(p_cap->i2c, REG_CMD, AW9380X_SLEEP_MODE);

	} else if (mode == AW9380X_DEEPSLEEP_MODE) {
		aw9380x_i2c_write(p_cap->i2c, REG_CMD, AW9380X_DEEPSLEEP_MODE);
	}
	p_cap->last_mode = mode;
	aw9380x_i2c_read(p_cap->i2c, 0x001c, &reg_data);
}

static void aw9380x_update_ch_en(struct aw9380x_cap *p_cap, bool en)
{
	int i;
	uint32_t cfg_val, ch_en_val;

	aw9380x_i2c_read(p_cap->i2c, REG_SCANCTRL0, &ch_en_val);
	AWLOGI(p_cap->dev, "ch_en_val old:0x%08X", ch_en_val);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		aw9380x_i2c_read(p_cap->i2c, REG_AFESOFTCFG0_CH0 + i * AW_REG_STEP, &cfg_val);
		if ((cfg_val & 0xFF) == 0x06) {
			if (en)
				ch_en_val |= 1 << i;
			else
				ch_en_val &= ~(1 << i);
		}
	}
	aw9380x_i2c_write(p_cap->i2c, REG_SCANCTRL0, ch_en_val);
	AWLOGI(p_cap->dev, "ch_en_val new:0x%08X", ch_en_val);
}

// update code ram and reg start
static int32_t aw9380x_update_code_ram_param(struct aw9380x_cap *p_cap)
{
	if (p_cap->code_ram_info->bin_name == NULL)
		return -AW_ERR;

	return AW_OK;
}

static int32_t aw9380x_update_reg_param(struct aw9380x_cap *p_cap)
{
	if (p_cap->reg_bin_info == NULL || p_cap->reg_bin_info->bin_name == NULL)
		return -AW_ERR;

	return AW_OK;
}

static void aw9380x_convert_little_endian_2_big_endian(struct aw_bin *aw_bin)
{
	int i = 0;
	uint32_t start_index = aw_bin->header_info[0].valid_data_addr;
	uint32_t fw_len = aw_bin->header_info[0].reg_num;
	uint32_t uints = fw_len / AW9380X_SRAM_UPDATE_ONE_UINT_SIZE;
	uint8_t tmp1 = 0;
	uint8_t tmp2 = 0;
	uint8_t tmp3 = 0;
	uint8_t tmp4 = 0;

	for (i = 0; i < uints; i++) {
		tmp1 = aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE + 3];
		tmp2 = aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE + 2];
		tmp3 = aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE + 1];
		tmp4 = aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE];
		aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE] = tmp1;
		aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE + 1] = tmp2;
		aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE + 2] = tmp3;
		aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_UINT_SIZE + 3] = tmp4;
	}
}

static int32_t aw9380x_coderam_write_check(struct aw9380x_cap *p_cap,
		uint16_t addr, uint8_t *dat, uint32_t len)
{
	uint8_t ret = 0;
	uint8_t *r_buf = NULL;
	uint8_t addr_buf[2] = {0};

	if (len > AW9380X_SRAM_UPDATE_ONE_PACK_SIZE + 2) {
		AWLOGE(p_cap->dev, "coderam write len error, max = %d, read = %d",
				AW9380X_SRAM_UPDATE_ONE_PACK_SIZE, len);
		return -AW_ERR;
	}

	ret = aw9380x_i2c_write_seq(p_cap->i2c, dat, len);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, " 0x%x, write_seq error(%d)!", addr, ret);
		return ret;
	}

	r_buf = kzalloc(AW9380X_SRAM_UPDATE_ONE_PACK_SIZE, GFP_KERNEL);
	if (r_buf == NULL) {
		AWLOGE(p_cap->dev, "%s malloc error", __func__);
		return -AW_ERR;
	}

	addr_buf[0] = (uint8_t)(addr >> OFFSET_BIT_8);
	addr_buf[1] = (uint8_t)addr;
	ret = aw9380x_i2c_read_seq(p_cap->i2c, addr_buf, 2, r_buf, len - 2);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "0x%x, read_seq error!", addr);
		goto err_out;
	}

	if (memcmp(dat + 2, r_buf, len - 2) != 0) {
		AWLOGE(p_cap->dev, "read is not equal to write 0x%x\n", addr);
		ret = -AW_ERR;
	}

err_out:
	kfree(r_buf);

	return ret;
}
/**
 * @brief  |----------------code ram-----------------|
 *         |--- app filled here---|--fill with 0xff--|
 *       0x2000                                    0x4fff
 *
 *         if the size of app is less than the size of coderam, the rest of
 *         ram is filled with 0xff.
 * @param offset the rear addr of app
 * @return int32_t
 */
static int32_t aw9380x_sram_fill_not_wrote_area(struct aw9380x_cap *p_cap,
						uint32_t offset)
{
	uint8_t *buf = NULL;
	int32_t ret = 0;
	uint32_t i = 0;
	uint32_t write_len = 0;
	uint32_t download_addr_with_ofst = 0;
	uint32_t last_pack_len = (AW9380X_SRAM_END_ADDR - offset) %
		AW9380X_SRAM_UPDATE_ONE_PACK_SIZE;
	uint32_t pack_cnt = last_pack_len == 0 ?
		((AW9380X_SRAM_END_ADDR - offset) / AW9380X_SRAM_UPDATE_ONE_PACK_SIZE) :
		((AW9380X_SRAM_END_ADDR - offset) / AW9380X_SRAM_UPDATE_ONE_PACK_SIZE) + 1;

	AWLOGI(p_cap->dev, "last_pack_len = %d, pack_cnt = %d, offset = 0x%x", last_pack_len, pack_cnt, offset);

	buf = kzalloc(AW9380X_SRAM_UPDATE_ONE_PACK_SIZE + 2, GFP_KERNEL);
	if (buf == NULL) {
		AWLOGE(p_cap->dev, "%s malloc error", __func__);
		return -AW_ERR;
	}
	memset(buf, 0xff, AW9380X_SRAM_UPDATE_ONE_PACK_SIZE + 2);

	for (i = 0; i < pack_cnt; i++) {
		download_addr_with_ofst = offset + i * AW9380X_SRAM_UPDATE_ONE_PACK_SIZE;
		*(buf + 0) = (uint8_t)(download_addr_with_ofst >> OFFSET_BIT_8);
		*(buf + 1) = (uint8_t)(download_addr_with_ofst);
		write_len = (i == (pack_cnt - 1) && last_pack_len) ?
			last_pack_len : AW9380X_SRAM_UPDATE_ONE_PACK_SIZE;
		ret = aw9380x_coderam_write_check(p_cap, download_addr_with_ofst, buf,
				write_len + 2);
		if (ret != AW_OK)
			break;
	}

	kfree(buf);

	return ret;
}

static int32_t aw9380x_sram_data_write(struct aw_bin *aw_bin, struct aw9380x_cap *p_cap)
{
	uint8_t *buf = NULL;
	int32_t ret = 0;
	uint32_t i = 0;
	uint32_t write_len = 0;
	uint32_t pack_cnt = 0;
	uint32_t start_index = aw_bin->header_info[0].valid_data_addr;
	uint32_t fw_bin_version = aw_bin->header_info[0].app_version;
	uint32_t download_addr = AW9380X_SRAM_START_ADDR;
	uint32_t fw_len = aw_bin->header_info[0].reg_num;
	uint32_t last_pack_len = fw_len % AW9380X_SRAM_UPDATE_ONE_PACK_SIZE;
	uint32_t download_addr_with_ofst = 0;

	pack_cnt = last_pack_len ? (fw_len / AW9380X_SRAM_UPDATE_ONE_PACK_SIZE) + 1 :
			(fw_len / AW9380X_SRAM_UPDATE_ONE_PACK_SIZE);

	p_cap->fw_bin_version = fw_bin_version;
	AWLOGI(p_cap->dev, "fw_bin_version = 0x%x, download_addr = 0x%x, start_index = %d, fw_len = %d, pack_cnt = %d",
		 fw_bin_version, download_addr, start_index, fw_len, pack_cnt);

	/* write data buf   |addr|data|  */
	buf = kzalloc(AW9380X_SRAM_UPDATE_ONE_PACK_SIZE + 2, GFP_KERNEL);
	if (buf == NULL) {
		AWLOGE(p_cap->dev, "aw9380x_coderam_data_write malloc error");
		return -AW_ERR;
	}

	for (i = 0; i < pack_cnt; i++) {
		memset(buf, 0, AW9380X_SRAM_UPDATE_ONE_PACK_SIZE + 2);
		download_addr_with_ofst = download_addr + i * AW9380X_SRAM_UPDATE_ONE_PACK_SIZE;
		write_len = (i == (pack_cnt - 1) && last_pack_len) ?
			last_pack_len : AW9380X_SRAM_UPDATE_ONE_PACK_SIZE;
		*(buf + 0) = (uint8_t)(download_addr_with_ofst >> OFFSET_BIT_8);
		*(buf + 1) = (uint8_t)(download_addr_with_ofst);
		memcpy(buf + 2, &aw_bin->info.data[start_index + i * AW9380X_SRAM_UPDATE_ONE_PACK_SIZE],
				write_len);
		ret = aw9380x_coderam_write_check(p_cap, download_addr_with_ofst, buf, write_len + 2);
		if (ret != AW_OK)
			goto err_out;
	}
	if (download_addr_with_ofst + write_len < AW9380X_SRAM_END_ADDR) {
		/* fill 0xff in the area that not worte. */
		ret = aw9380x_sram_fill_not_wrote_area(p_cap,
				download_addr_with_ofst + write_len);
		if (ret != AW_OK) {
			AWLOGE(p_cap->dev, "cnt%d, sram_fill_not_wrote_area error!\n", i);
			goto err_out;
		}
	}

err_out:
	kfree(buf);

	return ret;
}

static int32_t aw9380x_read_init_over_irq(struct aw9380x_cap *cap)
{
	uint32_t cnt = 1000;
	uint32_t reg = 0;
	int32_t ret = 0;

	while (cnt--) {
		ret = aw9380x_i2c_read(cap->i2c, REG_IRQSRC, &reg);
		if (ret != 0) {
			AWLOGE(cap->dev, "i2c error %d", ret);
			return ret;
		}
		if ((reg & 0x01) == 0x01) {
			AWLOGI(cap->dev, "read init irq success!");
			aw9380x_i2c_read(cap->i2c, REG_FWVER, &reg);
			AWLOGI(cap->dev, "firmware version = 0x%08x", reg);
			return AW_OK;
		}
		usleep_range(10000, 10050);
	}
	AWLOGE(cap->dev, "read init over irq error");

	return -AW_ERR;
}

static int32_t aw9380x_update_code_ram(struct aw_bin *aw_bin, struct aw9380x_cap *p_cap)
{
	struct i2c_client *i2c = p_cap->i2c;
	int32_t ret = 0;

	// step1: close coderam shutdown mode
	aw9380x_i2c_write(i2c, 0xfff4, 0x3c00d11f);
	aw9380x_i2c_write(i2c, 0xc400, 0x21660000);

	// step 2: reset mcu only and set boot mode to 1. (0xf800 0x00010100)
	aw9380x_i2c_write(i2c, 0xF800, 0x0010100);

	// step 3: enable data ram. (0xFFE4 0x3C000000)
	aw9380x_i2c_write(i2c, 0xFFE4, 0x3C000000);

	// setp 4: convert LD to BD
	aw9380x_convert_little_endian_2_big_endian(aw_bin);

	// step 5: write ram data.
	ret = aw9380x_sram_data_write(aw_bin, p_cap);
	if (ret == AW_OK) {
		AWLOGI(p_cap->dev, "sram_data_write OK");
	} else {
		AWLOGE(p_cap->dev, "sram_data_write error");
		return -AW_ERR;
	}
	mdelay(100);

	// step 6: exit reset mcu and boot cpu in ram. (0xf800 0x00010100)
	aw9380x_i2c_write(i2c, 0xf800, 0x00000100);

	// step 7: reset cpu (0xFF0C 0x0)
	aw9380x_i2c_write(i2c, 0xFF0C, 0x0);

	// step 8: Wait for chip initialization to complete
	msleep(500);

	return aw9380x_read_init_over_irq(p_cap);
}

static int32_t aw9380x_load_reg(struct aw_bin *aw_bin, struct i2c_client *i2c)
{
	uint32_t i = 0;
	int32_t ret = 0;
	uint16_t reg_addr = 0;
	uint32_t reg_data = 0;
	uint32_t start_addr = aw_bin->header_info[0].valid_data_addr;

	for (i = 0; i < aw_bin->header_info[0].valid_data_len; i += 6, start_addr += 6) {
		reg_addr = (aw_bin->info.data[start_addr]) |
				   aw_bin->info.data[start_addr + 1] << OFFSET_BIT_8;
		reg_data = aw_bin->info.data[start_addr + 2] |
				   (aw_bin->info.data[start_addr + 3] << OFFSET_BIT_8) |
				   (aw_bin->info.data[start_addr + 4] << OFFSET_BIT_16) |
				   (aw_bin->info.data[start_addr + 5] << OFFSET_BIT_24);

		ret = aw9380x_i2c_write(i2c, reg_addr, reg_data);
		if (ret < 0) {
			AWLOGE(&i2c->dev, "i2c write err");
			return -AW_ERR;
		}

		AWLOGI(&i2c->dev, "reg_addr = 0x%04x, reg_data = 0x%08x", reg_addr, reg_data);
	}

	return AW_OK;
}

static int32_t aw9380x_update_reg_by_bin(struct aw_bin *aw_bin, struct aw9380x_cap *p_cap)
{
	int32_t ret = 0;

	AWLOGI(p_cap->dev, "reg chip name: %s, soc chip name: %s, len = %d",
		   p_cap->chip_name, aw_bin->header_info[0].chip_type, aw_bin->info.len);

	ret = strncmp(p_cap->chip_name, aw_bin->header_info[0].chip_type,
			sizeof(aw_bin->header_info[0].chip_type));
	if (ret != 0) {
		AWLOGI(p_cap->dev, "load_binname(%s) incompatible with chip type(%s)",
			   p_cap->chip_name, aw_bin->header_info[0].chip_type);
	}

	p_cap->reg_bin_info->bin_data_ver = aw_bin->header_info[0].bin_data_ver;
	AWLOGI(p_cap->dev, "Bin_data_ver = 0x%x", p_cap->reg_bin_info->bin_data_ver);

	ret = aw9380x_load_reg(aw_bin, p_cap->i2c);

	return ret;
}

static int32_t aw9380x_parse_load_code_ram_bin(const struct firmware *cont,
		struct aw9380x_cap *p_cap)
{
	struct aw_bin *aw_bin = NULL;
	int32_t ret = -AW_ERR;

	if (!cont) {
		AWLOGE(p_cap->dev, "def_reg_bin request error!");
		return -AW_ERR;
	}

	AWLOGI(p_cap->dev, "Bin file size: %d", (uint32_t)cont->size);

	aw_bin = kzalloc(cont->size + sizeof(struct aw_bin), GFP_KERNEL);
	if (!aw_bin) {
		kfree(aw_bin);
		release_firmware(cont);
		AWLOGE(p_cap->dev, "failed to allcating memory!");
		return -AW_ERR;
	}

	aw_bin->info.len = cont->size;
	memcpy(aw_bin->info.data, cont->data, cont->size);

	ret = aw_parsing_bin_file(aw_bin);
	if (ret < 0) {
		AWLOGE(p_cap->dev, "parse bin fail! ret = %d", ret);
		goto err;
	}

	ret = aw9380x_update_code_ram(aw_bin, p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "load code ram bin to chip failed!");
		goto err;
	}
	// when coderam load end, the chip will go sleep mode
	p_cap->last_mode = AW9380X_SLEEP_MODE;
	AWLOGI(p_cap->dev, "load code ram bin ok!!!");
	p_cap->code_ram_info->bin_data_ver = aw_bin->header_info[0].bin_data_ver;
	return AW_OK;
err:
	if (aw_bin != NULL)
		kfree(aw_bin);

	return -AW_ERR;
}

static int32_t aw9380x_parse_load_reg_bin(const struct firmware *cont, struct aw9380x_cap *p_cap)
{
	struct aw_bin *aw_bin = NULL;
	int32_t ret = -AW_ERR;

	if (!cont) {
		AWLOGE(p_cap->dev, "def_reg_bin request error!");
		return -AW_ERR;
	}

	AWLOGI(p_cap->dev, "Bin file size: %d", (uint32_t)cont->size);

	aw_bin = kzalloc(cont->size + sizeof(struct aw_bin), GFP_KERNEL);
	if (!aw_bin) {
		kfree(aw_bin);
		release_firmware(cont);
		AWLOGE(p_cap->dev, "failed to allcating memory!");
		return -AW_ERR;
	}

	aw_bin->info.len = cont->size;
	memcpy(aw_bin->info.data, cont->data, cont->size);

	ret = aw_parsing_bin_file(aw_bin);
	if (ret < 0) {
		AWLOGE(p_cap->dev, "parse bin fail! ret = %d", ret);
		goto err;
	}

	ret = aw9380x_update_reg_by_bin(aw_bin, p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "load reg bin to chip failed!");
		goto err;
	}

	AWLOGI(p_cap->dev, "load reg bin ok!!!");

	return AW_OK;
err:
	if (aw_bin != NULL)
		kfree(aw_bin);

	return -AW_ERR;
}

static int32_t aw9380x_load_code_ram_bin(struct aw9380x_cap *p_cap)
{
	int32_t ret = -AW_ERR;
	const struct firmware *fw = NULL;

	AWLOGI(p_cap->dev, "name :%s", p_cap->code_ram_info->bin_name);

	ret = request_firmware(&fw, p_cap->code_ram_info->bin_name, p_cap->dev);
	AWLOGI(p_cap->dev, "request_firmware ret = %d", ret);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "parse %s error!", p_cap->code_ram_info->bin_name);
		return -AW_ERR;
	}

	ret = aw9380x_parse_load_code_ram_bin(fw, p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "reg_bin %s load error!", p_cap->code_ram_info->bin_name);
		p_cap->code_ram_bin_load_flag = false;
		return -AW_ERR;
	}
	p_cap->code_ram_bin_load_flag = true;
	return AW_OK;
}

static int32_t aw9380x_load_reg_bin(struct aw9380x_cap *p_cap)
{
	int32_t ret = -AW_ERR;
	const struct firmware *fw = NULL;

	AWLOGI(p_cap->dev, "name :%s.bin", p_cap->reg_bin_info->bin_name);

	ret = request_firmware(&fw, p_cap->reg_bin_info->bin_name, p_cap->dev);
	AWLOGI(p_cap->dev, "request_firmware ret = %d", ret);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "parse %s error!", p_cap->reg_bin_info->bin_name);
		return -AW_ERR;
	}

	ret = aw9380x_parse_load_reg_bin(fw, p_cap);
	if (ret != AW_OK) {
		p_cap->reg_bin_load_flag = -AW_ERR;
		AWLOGE(p_cap->dev, "reg_bin %s load error!", p_cap->reg_bin_info->bin_name);
		return -AW_ERR;
	}
	p_cap->reg_bin_load_flag = AW_OK;
	return AW_OK;
}

static int32_t aw9380x_update_coderam_fw(struct aw9380x_cap *p_cap)
{
	int32_t ret = -AW_ERR;

	//AWLOGI(p_cap->dev, "enter");
	if (aw9380x_update_code_ram_param(p_cap) != AW_OK) {
		AWLOGE(p_cap->dev, "update_code_ram_param failed");
		return ret;
	}

	return aw9380x_load_code_ram_bin(p_cap);
}

static int32_t aw9380x_update_reg_fw(struct aw9380x_cap *p_cap)
{
	uint32_t ret = 0;

	ret = aw9380x_update_reg_param(p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "update_reg_param failed");
		return ret;
	}

	return aw9380x_load_reg_bin(p_cap);
}

static int32_t aw9380x_para_loaded(struct i2c_client *i2c,
		const struct aw9380x_para_info *para_load)
{
	int32_t i = 0;
	int32_t ret = 0;

	for (i = 0; i < para_load->reg_arr_len; i = i + 2) {
		ret = aw9380x_i2c_write(i2c, (uint16_t)para_load->reg_arr[i],
				para_load->reg_arr[i + 1]);
		if (ret != AW_OK)
			return -AW_REG_LOAD_ERR;
		AWLOGI(&i2c->dev, "reg_addr = 0x%04x, reg_data = 0x%08x",
				para_load->reg_arr[i],
				para_load->reg_arr[i + 1]);
	}

	AWLOGI(&i2c->dev, "para writen completely");

	return AW_OK;
}

static void aw9380x_update_no_wait(struct aw9380x_cap *p_cap)
{
	uint32_t ret = 0;

	aw9380x_disable_irq(p_cap);
	mutex_lock(&aw9380x_lock);

	// 1. update code ram
	ret = aw9380x_update_coderam_fw(p_cap);
	if (ret != AW_OK)
		AWLOGE(p_cap->dev, "upgrade code ram firmware error!");
	else
		AWLOGI(p_cap->dev, "upgrade code ram firmware success!");

	// 2. update reg bin
	ret = aw9380x_update_reg_fw(p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "upgrade reg firmware failed!, will use default set");
		aw9380x_para_loaded(p_cap->i2c, p_cap->para_info);
	}

	if (p_cap->chip_id == AW93803_CHIP_ID)
		aw9380x_set_cs_as_irq(p_cap, AW9380X_CSX_TO_IRQ);

	if (AW_CS_ERR_DET == 0)
		aw9380x_update_ch_en(p_cap, false);

	// 3. active chip
	aw9380x_mode_set(p_cap, p_cap->chip_mode_info->init_mode);

	if (p_cap->irq_init.host_irq_stat == IRQ_DISABLE) {
		enable_irq(p_cap->irq_init.to_irq);
		p_cap->irq_init.host_irq_stat = IRQ_ENABLE;
	}

	p_cap->driver_code_init_over_flag = true;
	aw9380x_fct_cali_coef_def_read(p_cap);
	aw9380x_fct_data_write_back_to_fw(p_cap);

	mutex_unlock(&aw9380x_lock);
}

static void aw9380x_update_work(struct work_struct *work)
{
	struct aw9380x_cap *p_cap = container_of(work, struct aw9380x_cap, update_work.work);

	aw9380x_update_no_wait(p_cap);
}

static void aw9380x_update_delay(struct aw9380x_cap *p_cap)
{
	//AWLOGI(p_cap->dev, "enter");
	INIT_DELAYED_WORK(&p_cap->update_work, aw9380x_update_work);
	schedule_delayed_work(&p_cap->update_work,
			msecs_to_jiffies(AW9380X_POWER_ON_SYSFS_DELAY_MS));
}

// update code ram end

static int32_t aw9380x_regulator_power_init(struct aw9380x_cap *p_cap)
{
	int32_t rc = 0;

	AWLOGI(p_cap->dev, "aw9380x power init enter");

	p_cap->vcc = regulator_get(p_cap->dev, "avdd");
	if (IS_ERR(p_cap->vcc)) {
		rc = PTR_ERR(p_cap->vcc);
		AWLOGE(p_cap->dev, "regulator get failed vcc rc = %d", rc);
		return -AW_ERR;
	}

	if (regulator_count_voltages(p_cap->vcc) > 0) {
		rc = regulator_set_voltage(p_cap->vcc, AW9380X_VCC_MIN_UV, AW9380X_VCC_MAX_UV);
		if (rc) {
			AWLOGE(p_cap->dev, "regulator set vol failed rc = %d", rc);
			goto reg_vcc_put;
		}
	}

	return AW_OK;

reg_vcc_put:
	regulator_put(p_cap->vcc);

	return -AW_ERR;
}

static void aw9380x_power_enable(struct aw9380x_cap *p_cap, bool on)
{
	int32_t rc = 0;

	AWLOGI(p_cap->dev, "aw9380x power enable enter");

	if (on) {
		rc = regulator_enable(p_cap->vcc);
		if (rc) {
			AWLOGE(p_cap->dev, "regulator_enable vol failed rc = %d", rc);
		} else {
			p_cap->power_enable = true;
			msleep(20);
		}
	} else {
		rc = regulator_disable(p_cap->vcc);
		if (rc)
			AWLOGE(p_cap->dev, "regulator_disable vol failed rc = %d", rc);
		else
			p_cap->power_enable = false;
	}
}

static void aw9380x_power_deinit(struct aw9380x_cap *p_cap)
{
	if (p_cap->power_enable) {
		// Turn off the power output. However,
		// it may not be turned off immediately
		// There are scenes where power sharing can exist
		regulator_disable(p_cap->vcc);
		regulator_put(p_cap->vcc);
	}
}

static int32_t aw9380x_regulator_power(struct aw9380x_cap *p_cap)
{
	struct aw9380x_dts_info *p_dts_info = &p_cap->dts_info;
	int32_t ret = 0;

	p_dts_info->use_regulator_flag =
		of_property_read_bool(p_cap->i2c->dev.of_node, "aw_cap,regulator-power-supply");
	AWLOGI(p_cap->dev, "regulator-power-supply = <%d>", p_dts_info->use_regulator_flag);

	// Configure the use of regulator power supply in DTS
	if (p_cap->dts_info.use_regulator_flag == true) {
		ret = aw9380x_regulator_power_init(p_cap);
		if (ret != AW_OK) {
			AWLOGE(p_cap->dev, "power init failed");
			return ret;
		}
		aw9380x_power_enable(p_cap, AW_TRUE);
		ret = aw9380x_regulator_is_get_voltage(p_cap);
		if (ret != AW_OK) {
			AWLOGE(p_cap->dev, "get_voltage failed");
			aw9380x_power_deinit(p_cap);
		}
	}

	return ret;
}
#if 0
static int32_t aw9380x_pinctrl_init(struct aw9380x_cap *p_cap)
{
	struct aw9380x_pinctrl *pinctrl = &p_cap->pinctrl;
	uint8_t pin_default_name[50] = {0};
	uint8_t pin_output_low_name[50] = {0};
	uint8_t pin_output_high_name[50] = {0};

	//AWLOGI(p_cap->dev, "enter");

	pinctrl->pinctrl = devm_pinctrl_get(p_cap->dev);
	if (IS_ERR_OR_NULL(pinctrl->pinctrl)) {
		AWLOGI(p_cap->dev, "%s:No pinctrl found\n", __func__);
		pinctrl->pinctrl = NULL;
		return -EINVAL;
	}

	snprintf(pin_default_name, sizeof(pin_default_name),
			"aw9380x_%u", p_cap->dts_info.cap_num);
	AWLOGI(p_cap->dev, "pin_default_name = %s", pin_default_name);
	pinctrl->default_sta = pinctrl_lookup_state(pinctrl->pinctrl, pin_default_name);
	if (IS_ERR_OR_NULL(pinctrl->default_sta)) {
		AWLOGI(p_cap->dev, "Failed get pinctrl state:default state");
		goto exit_pinctrl_init;
	}

	snprintf(pin_output_high_name, sizeof(pin_output_high_name),
			"aw_int_output_high_sar%u", p_cap->dts_info.cap_num);
	AWLOGI(p_cap->dev, "pin_output_high_name = %s", pin_output_high_name);
	pinctrl->int_out_high = pinctrl_lookup_state(pinctrl->pinctrl, pin_output_high_name);
	if (IS_ERR_OR_NULL(pinctrl->int_out_high)) {
		AWLOGI(p_cap->dev, "Failed get pinctrl state:output_high");
		goto exit_pinctrl_init;
	}

	snprintf(pin_output_low_name, sizeof(pin_output_low_name),
			"aw_int_output_low_sar%u", p_cap->dts_info.cap_num);
	AWLOGI(p_cap->dev, "pin_output_low_name = %s", pin_output_low_name);
	pinctrl->int_out_low = pinctrl_lookup_state(pinctrl->pinctrl, pin_output_low_name);
	if (IS_ERR_OR_NULL(pinctrl->int_out_low)) {
		AWLOGI(p_cap->dev, "Failed get pinctrl state:output_low");
		goto exit_pinctrl_init;
	}

	AWLOGI(p_cap->dev, "Success init pinctrl");

	return 0;

exit_pinctrl_init:
	devm_pinctrl_put(pinctrl->pinctrl);
	pinctrl->pinctrl = NULL;

	return -EINVAL;
}

static void aw9380x_pinctrl_deinit(struct aw9380x_cap *p_cap)
{
	if (p_cap->pinctrl.pinctrl)
		devm_pinctrl_put(p_cap->pinctrl.pinctrl);
}
#endif
static int32_t aw9380x_parse_dts(struct device *dev, struct device_node *np,
		struct aw9380x_dts_info *p_dts_info)
{
	int32_t val = 0;

	// get cap num
	val = of_property_read_u32(np, "cap-num", &p_dts_info->cap_num);
	AWLOGI(dev, "cap num = %d", p_dts_info->cap_num);
	if (val != 0) {
		AWLOGE(dev, "multiple cap failed!");
		return -AW_ERR;
	}

	// get irq gpio
	p_dts_info->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	if (p_dts_info->irq_gpio < 0) {
		p_dts_info->irq_gpio = -1;
		AWLOGE(dev, "no irq gpio provided.");
		return -AW_ERR;
	}
	AWLOGI(dev, "irq gpio provided ok.");

	// get channel use flag
	val = of_property_read_u32(np, "channel_use_flag", &p_dts_info->channel_use_flag);
	AWLOGI(dev, "channel_use_flag = 0x%x", p_dts_info->channel_use_flag);
	if (val != 0) {
		AWLOGE(dev, "channel_use_flag failed!");
		return -AW_ERR;
	}

	// get pin_set_inter_pull-up
	p_dts_info->use_inter_pull_up = of_property_read_bool(np, "aw_cap,pin_set_inter_pull-up");
	AWLOGI(dev, "aw_cap,use_inter_pull_up = <%d>", p_dts_info->use_inter_pull_up);

	// get  using pm ops
	p_dts_info->use_pm = of_property_read_bool(np, "aw_cap,using_pm_ops");
	AWLOGI(dev, "aw_cap, using_pm_ops = <%d>", p_dts_info->use_pm);

	return AW_OK;
}

static int32_t aw9380x_check_chipid(struct aw9380x_cap *p_cap)
{
	uint32_t reg_val = 0;
	int32_t ret = -AW_ERR;

	ret = aw9380x_i2c_read(p_cap->i2c, REG_CHIP_ID0, &reg_val);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "read chip id failed, ret = %d", ret);
		return ret;
	}
	p_cap->chip_id = reg_val;
	switch (reg_val) {
	case AW93803_CHIP_ID:
		memcpy(p_cap->chip_name, AW93803_NAME, 8);
		AWLOGI(p_cap->dev, "AW93803 detected 0x%08x", reg_val);
		ret = AW_OK;
		break;
	case AW93804_CHIP_ID:
		memcpy(p_cap->chip_name, AW93804_NAME, 8);
		AWLOGI(p_cap->dev, "AW93804 detected 0x%08x", reg_val);
		ret = AW_OK;
		break;
	case AW93805_CHIP_ID:
		memcpy(p_cap->chip_name, AW93805_NAME, 8);
		AWLOGI(p_cap->dev, "AW93805 detected 0x%08x", reg_val);
		ret = AW_OK;
		break;
	default:
		ret = AW_ERR;
		AWLOGE(p_cap->dev, "chip error 0x%08x", reg_val);
		break;
	}

	return ret;
}

static void aw9380x_init_other_val(struct aw9380x_cap *p_cap)
{
	mutex_init(&aw9380x_lock);
	p_cap->pm_info = &g_aw9380x_pm_info;
	p_cap->p_reg_list = &g_aw9380x_reg_list;
	p_cap->chip_mode_info = &g_aw9380x_chip_mode_info;
	p_cap->code_ram_info = &g_aw9380x_code_ram_info;
	p_cap->p_diff = &g_aw9380x_diff_info;
	p_cap->para_info = &g_aw9380x_para_info;
	p_cap->reg_bin_info = &g_aw9380x_reg_bin_info;
	p_cap->irq_cfg = &g_aw9380x_irq_cfg;
	p_cap->driver_code_init_over_flag = false;
	p_cap->code_ram_bin_load_flag = false;
	p_cap->fault_flag = AW9380X_HEALTHY;

	p_cap->noth_btn_state = KEY_EVENT_UNKNOWN;
};

/********************************* debug node start ***********************************/
// awrw start
static int32_t aw9380x_awrw_data_analysis(struct aw9380x_cap *p_cap, const char *buf, uint8_t len)
{
	uint32_t i = 0;
	uint8_t data_temp[2] = {0};
	uint8_t index = 0;
	uint32_t tranfar_data_temp = 0;
	uint32_t theory_len = len * AW9380X_AWRW_DATA_WIDTH + AW9380X_AWRW_OffSET;
	uint32_t actual_len = strlen(buf);

	//AWLOGI(p_cap->dev, "enter");

	if (theory_len != actual_len) {
		AWLOGE(p_cap->dev, "error theory_len = %d actual_len = %d", theory_len, actual_len);
		return -AW_ERR;
	}

	for (i = 0; i < len * AW9380X_AWRW_DATA_WIDTH; i += AW9380X_AWRW_DATA_WIDTH) {
		data_temp[0] = buf[AW9380X_AWRW_OffSET + i + AW9380X_DATA_OffSET_2];
		data_temp[1] = buf[AW9380X_AWRW_OffSET + i + AW9380X_DATA_OffSET_3];

		if (sscanf(data_temp, "%02x", &tranfar_data_temp) == 1) {
			p_cap->awrw_info.p_i2c_tranfar_data[index] = (uint8_t)tranfar_data_temp;
			AWLOGI(p_cap->dev, "tranfar_data = 0x%2x",
				   p_cap->awrw_info.p_i2c_tranfar_data[index]);
		}
		index++;
	}

	return 0;
}

static int32_t aw9380x_awrw_write(struct aw9380x_cap *p_cap, const char *buf)
{
	int32_t ret = 0;
	uint8_t *w_buf;
	uint8_t write_nums, last_nums;
	uint32_t addr_temp;
	uint32_t data_len;
	int32_t i;

	ret = aw9380x_awrw_data_analysis(p_cap, buf, p_cap->awrw_info.i2c_tranfar_data_len);
	if (ret == 0) {
		w_buf = devm_kzalloc(p_cap->dev, p_cap->awrw_info.addr_len + AW9380X_I2C_TRANS_ONE_PACK_SIZE, GFP_KERNEL);
		if (w_buf == NULL) {
			AWLOGE(p_cap->dev, "devm_kzalloc error");
			return -AW_ERR;
		}
		data_len = p_cap->awrw_info.i2c_tranfar_data_len - p_cap->awrw_info.addr_len;
		write_nums =  data_len / AW9380X_I2C_TRANS_ONE_PACK_SIZE;
		last_nums = data_len % AW9380X_I2C_TRANS_ONE_PACK_SIZE;
		AWLOGI(p_cap->dev, "write_nums is %d, last_nums:%d", write_nums, last_nums);
		addr_temp = p_cap->awrw_info.p_i2c_tranfar_data[0] << 8 |
			p_cap->awrw_info.p_i2c_tranfar_data[1];
		for (i = 0; i < write_nums; i++) {
			w_buf[0] = addr_temp >> 8;
			w_buf[1] = addr_temp & 0xFF;
			memcpy(w_buf + 2, p_cap->awrw_info.p_i2c_tranfar_data +
					2 + i * AW9380X_I2C_TRANS_ONE_PACK_SIZE,
					AW9380X_I2C_TRANS_ONE_PACK_SIZE);
			aw9380x_i2c_write_seq(p_cap->i2c, w_buf,
					p_cap->awrw_info.addr_len + AW9380X_I2C_TRANS_ONE_PACK_SIZE);
			memset(w_buf, 0, p_cap->awrw_info.addr_len + AW9380X_I2C_TRANS_ONE_PACK_SIZE);
			addr_temp += AW9380X_I2C_TRANS_ONE_PACK_SIZE;
		}
		if (last_nums) {
			w_buf[0] = addr_temp >> 8;
			w_buf[1] = addr_temp & 0xFF;
			memcpy(w_buf + 2, p_cap->awrw_info.p_i2c_tranfar_data +
					2 + write_nums * AW9380X_I2C_TRANS_ONE_PACK_SIZE,
					last_nums);
			aw9380x_i2c_write_seq(p_cap->i2c, w_buf,
					p_cap->awrw_info.addr_len + last_nums);
		}
		devm_kfree(p_cap->dev, w_buf);
	}

	return ret;
}

static int32_t aw9380x_awrw_read(struct aw9380x_cap *p_cap, const char *buf)
{
	int32_t ret = 0;
	uint8_t *p_buf = p_cap->awrw_info.p_i2c_tranfar_data + p_cap->awrw_info.addr_len;
	uint32_t len = (uint16_t)(p_cap->awrw_info.data_len * p_cap->awrw_info.reg_num);
	uint8_t *r_buf;
	uint8_t read_nums, last_nums;
	uint32_t addr_temp;
	uint8_t addr_tx[2];
	int32_t i;

	ret = aw9380x_awrw_data_analysis(p_cap, buf, p_cap->awrw_info.addr_len);
	if (ret == 0) {
		AWLOGI(p_cap->dev, "len is %d", (p_cap->awrw_info.data_len * p_cap->awrw_info.reg_num));
		r_buf = devm_kzalloc(p_cap->dev, len, GFP_KERNEL);
		if (r_buf == NULL) {
			AWLOGE(p_cap->dev, "devm_kzalloc error");
			return -AW_ERR;
		}
		read_nums = len / AW9380X_I2C_TRANS_ONE_PACK_SIZE;
		last_nums = len % AW9380X_I2C_TRANS_ONE_PACK_SIZE;
		AWLOGI(p_cap->dev, "read_nums is %d, last_nums:%d", read_nums, last_nums);
		addr_temp = p_cap->awrw_info.p_i2c_tranfar_data[0] << 8 |
			p_cap->awrw_info.p_i2c_tranfar_data[1];
		AWLOGI(p_cap->dev, "addr_temp is 0x%04X", addr_temp);
		for (i = 0; i < read_nums; i++) {
			addr_tx[0] = addr_temp >> 8;
			addr_tx[1] = addr_temp & 0xFF;
			ret = aw9380x_i2c_read_seq(p_cap->i2c,
					addr_tx,
					p_cap->awrw_info.addr_len,
					r_buf + AW9380X_I2C_TRANS_ONE_PACK_SIZE * i,
					AW9380X_I2C_TRANS_ONE_PACK_SIZE);
			addr_temp += AW9380X_I2C_TRANS_ONE_PACK_SIZE;
		}
		if (last_nums) {
			addr_tx[0] = addr_temp >> 8;
			addr_tx[1] = addr_temp & 0xFF;
			ret = aw9380x_i2c_read_seq(p_cap->i2c,
					addr_tx,
					p_cap->awrw_info.addr_len,
					r_buf + AW9380X_I2C_TRANS_ONE_PACK_SIZE * read_nums,
					last_nums);
		}


		memcpy(p_cap->awrw_info.p_i2c_tranfar_data + p_cap->awrw_info.addr_len, r_buf, len);
		devm_kfree(p_cap->dev, r_buf);

		if (ret != AW_OK)
			memset(p_buf, 0xff, len);
	}

	return ret;
}

static int32_t aw9380x_awrw_get_func(struct aw9380x_cap *p_cap, char *buf)
{
	uint32_t len = 0;
	uint i = 0;

	if (p_cap->awrw_info.p_i2c_tranfar_data == NULL) {
		AWLOGE(p_cap->dev, "p_i2c_tranfar_data is NULL");
		return len;
	}

	if (p_cap->awrw_info.rw_flag == AW9380X_PACKAGE_RD) {
		for (i = 0; i < p_cap->awrw_info.i2c_tranfar_data_len; i++) {
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%02x,",
					p_cap->awrw_info.p_i2c_tranfar_data[i]);
		}
	} else {
		for (i = 0; i < (p_cap->awrw_info.data_len) * (p_cap->awrw_info.reg_num); i++) {
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%02x,",
					p_cap->awrw_info.p_i2c_tranfar_data[p_cap->awrw_info.addr_len + i]);
		}
	}
	snprintf(buf + len - 1, PAGE_SIZE - len, "\n");

	devm_kfree(p_cap->dev, p_cap->awrw_info.p_i2c_tranfar_data);
	p_cap->awrw_info.p_i2c_tranfar_data = NULL;

	return len;
}

static ssize_t awrw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&aw9380x_lock);

	ret = (ssize_t)aw9380x_awrw_get_func(p_cap, buf);

	mutex_unlock(&aw9380x_lock);

	return ret;
}

static int32_t aw9380x_awrw_handle(struct aw9380x_cap *p_cap, const char *buf)
{
	int32_t ret = 0;

	p_cap->awrw_info.i2c_tranfar_data_len = p_cap->awrw_info.addr_len +
		p_cap->awrw_info.data_len *
		p_cap->awrw_info.reg_num;

	if (p_cap->awrw_info.p_i2c_tranfar_data != NULL) {
		devm_kfree(p_cap->dev, p_cap->awrw_info.p_i2c_tranfar_data);
		p_cap->awrw_info.p_i2c_tranfar_data = NULL;
	}

	p_cap->awrw_info.p_i2c_tranfar_data = devm_kzalloc(p_cap->dev,
			p_cap->awrw_info.i2c_tranfar_data_len, GFP_KERNEL);
	if (p_cap->awrw_info.p_i2c_tranfar_data == NULL) {
		AWLOGE(p_cap->dev, "devm_kzalloc error");
		return -AW_ERR;
	}

	if (p_cap->awrw_info.rw_flag == AW9380X_I2C_WR) {
		ret = aw9380x_awrw_write(p_cap, buf);
		if (ret != 0)
			AWLOGE(p_cap->dev, "awrw_write error");
		if (p_cap->awrw_info.p_i2c_tranfar_data != NULL) {
			devm_kfree(p_cap->dev, p_cap->awrw_info.p_i2c_tranfar_data);
			p_cap->awrw_info.p_i2c_tranfar_data = NULL;
		}
	} else if (p_cap->awrw_info.rw_flag == AW9380X_I2C_RD) {
		ret = aw9380x_awrw_read(p_cap, buf);
		if (ret != 0)
			AWLOGE(p_cap->dev, "awrw_read error");
	} else {
		return -AW_ERR;
	}

	return AW_OK;
}

static int32_t aw9380x_awrw_set_func(struct aw9380x_cap *p_cap, const char *buf)
{
	uint32_t rw_flag = 0;
	uint32_t addr_bytes = 0;
	uint32_t data_bytes = 0;
	uint32_t package_nums = 0;
	uint32_t reg_num = 0;
	uint32_t i = 0;
	uint32_t j = 0;
	uint32_t addr_tmp = 0;
	uint32_t buf_index0 = 0;
	uint32_t buf_index1 = 0;
	uint32_t r_buf_len = 0;
	uint32_t tr_offset = 0;
	uint8_t addr[4] = {0};
	uint32_t theory_len = 0;
	uint32_t actual_len = 0;

	// step1: Parse frame header
	if (sscanf(buf, "0x%02x 0x%02x 0x%02x ", &rw_flag, &addr_bytes, &data_bytes) != 3) {
		AWLOGE(p_cap->dev, "sscanf0 parse error!");
		return -AW_ERR;
	}
	p_cap->awrw_info.rw_flag = (uint8_t)rw_flag;
	p_cap->awrw_info.addr_len = (uint8_t)addr_bytes;
	p_cap->awrw_info.data_len = (uint8_t)data_bytes;

	if (addr_bytes > 4) {
		return -AW_ERR;
		AWLOGE(p_cap->dev, "para error!");
	}

	if ((rw_flag == AW9380X_I2C_WR) || (rw_flag == AW9380X_I2C_RD)) {
		if (sscanf(buf + AW9380X_OFFSET_LEN, "0x%02x ", &reg_num) != 1) {
			AWLOGE(p_cap->dev, "sscanf1 parse error!");
			return -AW_ERR;
		}
		p_cap->awrw_info.reg_num = (uint8_t)reg_num;
		aw9380x_awrw_handle(p_cap, buf);
	} else if (rw_flag == AW9380X_PACKAGE_RD) {
		// step2: Get number of packages
		if (sscanf(buf + AW9380X_OFFSET_LEN, "0x%02x ", &package_nums) != 1) {
			AWLOGE(p_cap->dev, "sscanf2 parse error!");
			return -AW_ERR;
		}
		theory_len = AW9380X_OFFSET_LEN + AW9380X_AWRW_DATA_WIDTH +
			package_nums * (AW9380X_AWRW_DATA_WIDTH +
					AW9380X_AWRW_DATA_WIDTH * addr_bytes);
		actual_len = strlen(buf);
		//	AWLOGI(p_cap->dev, "theory_len:%d, actual_len:%d", theory_len, actual_len);
		if (theory_len != actual_len) {

			AWLOGE(p_cap->dev, "theory_len:%d, actual_len:%d error!", theory_len, actual_len);
			return -AW_ERR;
		}

		// step3: Get the size of read data and apply for space
		//	AWLOGI(p_cap->dev, "package_nums:%d", package_nums);
		for (i = 0; i < package_nums; i++) {
			buf_index0 = AW9380X_OFFSET_LEN + AW9380X_AWRW_DATA_WIDTH +
				(AW9380X_AWRW_DATA_WIDTH * addr_bytes + AW9380X_AWRW_DATA_WIDTH) * i;
			if (sscanf(buf + buf_index0, "0x%02x", &reg_num) != 1) {
				AWLOGE(p_cap->dev, "sscanf3 parse error!");
				return -AW_ERR;
			}
			// AWLOGI(p_cap->dev, "reg_num:%d", reg_num);
			r_buf_len += reg_num * data_bytes;
		}

		//	AWLOGI(p_cap->dev, "r_buf_len:%d", r_buf_len);
		p_cap->awrw_info.i2c_tranfar_data_len = r_buf_len;

		if (p_cap->awrw_info.p_i2c_tranfar_data != NULL) {
			devm_kfree(p_cap->dev, p_cap->awrw_info.p_i2c_tranfar_data);
			p_cap->awrw_info.p_i2c_tranfar_data = NULL;
		}
		p_cap->awrw_info.p_i2c_tranfar_data = devm_kzalloc(p_cap->dev,
				r_buf_len, GFP_KERNEL);
		if (p_cap->awrw_info.p_i2c_tranfar_data == NULL) {
			AWLOGE(p_cap->dev, "devm_kzalloc error");
			return -AW_ERR;
		}

		// step3: Resolve register address and read data in packets
		for (i = 0; i < package_nums; i++) {
			buf_index0 = AW9380X_OFFSET_LEN + AW9380X_AWRW_DATA_WIDTH +
				(AW9380X_AWRW_DATA_WIDTH * addr_bytes + AW9380X_AWRW_DATA_WIDTH) * i;
			if (sscanf(buf + buf_index0, "0x%02x", &reg_num) != 1) {
				AWLOGE(p_cap->dev, "sscanf4 parse error!");
				return -AW_ERR;
			}

			for (j = 0; j < addr_bytes; j += 1) {
				buf_index1 = buf_index0 + AW9380X_AWRW_DATA_WIDTH +
					(j * AW9380X_AWRW_DATA_WIDTH);
				// AWLOGI(p_cap->dev, "buf_index1 = %d", buf_index1);
				if (sscanf(buf + buf_index1, "0x%02x", &addr_tmp) == 1) {
					addr[j] = (uint8_t)addr_tmp;
					// AWLOGI(p_cap->dev, "tranfar_data = 0x%2x", addr[j]);
				} else {
					AWLOGE(p_cap->dev, "sscanf5 parse error!");
					return -AW_ERR;
				}
			}
			//	AWLOGI(p_cap->dev, "tr_offset = %d", tr_offset);
			aw9380x_i2c_read_seq(p_cap->i2c,
					addr,
					addr_bytes,
					p_cap->awrw_info.p_i2c_tranfar_data + tr_offset,
					(uint16_t)(data_bytes * reg_num));
			tr_offset += data_bytes * reg_num;
		}
	}

	return AW_OK;
}

static ssize_t
awrw_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	mutex_lock(&aw9380x_lock);

	aw9380x_awrw_set_func(p_cap, buf);

	mutex_unlock(&aw9380x_lock);

	return count;
}
// awrw end

// offset node start
static int32_t aw9380x_get_signed_cap(struct aw9380x_cap *p_cap, uint16_t reg_addr)
{
	uint32_t reg_data = 0;
	int32_t off_f = 0;
	uint32_t off_c = 0;
	uint32_t off_m = 0;
	uint32_t off_m_bit = 0;
	uint32_t off_c_bit = 0;
	int32_t s_ofst_c = 0;
	uint8_t i = 0;

	aw9380x_i2c_read(p_cap->i2c, reg_addr, &reg_data);

	off_f = ((reg_data >> AW_BIT16) & ONE_WORD) * AW9380X_STEP_LEN_UNSIGNED_CAP_FINE_ADJ;
	off_c = (reg_data >> AW_BIT8) & ONE_WORD;
	off_m = reg_data & ONE_WORD;

	for (i = 0; i < 8; i++) {
		off_m_bit = (off_m >> i) & 0x01;
		off_c_bit = (off_c >> i) & 0x01;
		s_ofst_c += ((1 - 2 * off_m_bit) * off_c_bit * aw9380x_sar_pow2(i)) *
			AW9380X_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ;
	}

	return (s_ofst_c + off_f);
}

static uint32_t aw9380x_get_unsigned_cap_minus(struct aw9380x_cap *p_cap, uint16_t reg_addr)
{
	uint32_t reg_data = 0;
	uint32_t rough = 0;
	uint32_t fine = 0;

	aw9380x_i2c_read(p_cap->i2c, reg_addr, &reg_data);

	rough = ((reg_data >> AW_BIT8) & ONE_WORD) * AW9380X_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ;
	fine = ((reg_data >> AW_BIT16) & ONE_WORD) * AW9380X_STEP_LEN_UNSIGNED_CAP_FINE_ADJ;
	AWLOGI(p_cap->dev, "rough=%d, fine = %d, total=%d",
			rough, fine, rough - fine);

	return (rough - fine);
}

static uint32_t aw9380x_get_unsigned_cap(struct aw9380x_cap *p_cap, uint16_t reg_addr)
{
	uint32_t reg_data = 0;
	uint32_t rough = 0;
	uint32_t fine = 0;

	aw9380x_i2c_read(p_cap->i2c, reg_addr, &reg_data);

	rough = ((reg_data >> AW_BIT8) & ONE_WORD) * AW9380X_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ;
	fine = ((reg_data >> AW_BIT16) & ONE_WORD) * AW9380X_STEP_LEN_UNSIGNED_CAP_FINE_ADJ;
	AWLOGI(p_cap->dev, "rough=%d, fine = %d, total=%d",
				rough, fine, rough + fine);

	return (rough + fine);
}

static ssize_t aw9380x_get_cap_offset(struct aw9380x_cap *p_cap, char *buf)
{
	ssize_t len = 0;
	uint32_t reg_data = 0;
	uint32_t mode = 0xff;
	uint32_t i = 0;
	uint32_t cap_ofst = 0;
	int32_t signed_cap_ofst = 0;
	uint32_t tmp = 0;

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		aw9380x_i2c_read(p_cap->i2c,
				REG_AFESOFTCFG0_CH0 +
				i * (REG_AFESOFTCFG0_CH1 - REG_AFESOFTCFG0_CH0),
				&reg_data);
		mode = reg_data & 0x0ff;
		switch (mode) {
		case AW9380X_UNSIGNED_CAP: // self-capacitance mode unsigned cail
			cap_ofst = aw9380x_get_unsigned_cap(p_cap,
					REG_AFECFG1_CH0 + i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
			// Because it has been expanded by 1000 times before,
			// the accuracy of removing mul's expansion loss can be ignored
			AWLOGI(p_cap->dev, "cap_ofst = %d", cap_ofst);
			len += snprintf(buf + len, PAGE_SIZE - len,
					"unsigned cap ofst ch%u: %u.%u pf\r\n",
					i,
					cap_ofst / AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE,
					cap_ofst % AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			break;
		case AW9380X_SIGNED_CAP: // self-capacitance mode signed cail
			signed_cap_ofst = aw9380x_get_signed_cap(p_cap,
					REG_AFECFG1_CH0 + i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
			AWLOGI(p_cap->dev, "cap_ofst = 0x%x", signed_cap_ofst);
			if (signed_cap_ofst < 0) {
				tmp = -signed_cap_ofst;
				AWLOGI(p_cap->dev, "cap_ofst2 = 0x%x", signed_cap_ofst);
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: -%u.%upf\r\n",
						i,
						tmp / AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						tmp % AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			} else {
				AWLOGI(p_cap->dev, "cap_ofst2 = 0x%x", signed_cap_ofst);
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: %d.%dpf\r\n",
						i,
						signed_cap_ofst / AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						signed_cap_ofst % AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			}
			break;
		case AW9380X_MUTUAL_CAP: // mutual-capacitance mode
			signed_cap_ofst = aw9380x_get_signed_cap(p_cap,
					REG_AFECFG1_CH0 + i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
			AWLOGI(p_cap->dev, "cap_ofst = 0x%x", signed_cap_ofst);
			if (signed_cap_ofst < 0) {
				tmp = -signed_cap_ofst;
				AWLOGI(p_cap->dev, "cap_ofst2 = 0x%x", signed_cap_ofst);
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: -%u.%upf\r\n",
						i,
						tmp / AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						tmp % AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			} else {
				AWLOGI(p_cap->dev, "cap_ofst2 = 0x%x", signed_cap_ofst);
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: %d.%dpf\r\n",
						i,
						signed_cap_ofst / AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						signed_cap_ofst % AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			}
			break;
		default:
			AWLOGI(p_cap->dev, "aw9380x ofst error 0x%x", reg_data & 0x0f);
			break;
		}
	}
	return len;
}

static ssize_t
offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	return aw9380x_get_cap_offset(p_cap, buf);
}
// offset node end

// aot node start
static void aw9380x_aot(struct aw9380x_cap *p_cap)
{
	aw9380x_i2c_write_bits(p_cap->i2c, REG_SCANCTRL1, ~0xfff, 0xfff);
}

static ssize_t
aot_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	uint32_t cali_flag = 0;

	if (kstrtouint(buf, 0, &cali_flag) != 0)
		return count;

	if (cali_flag == AW_TRUE) {
		aw9380x_aot(p_cap);
		p_cap->aot_done_flag = 0;
		AWLOGI(p_cap->dev, "aot success");
	} else {
		AWLOGE(p_cap->dev, "fail to set aot cali");
	}

	return count;
}
// aot node end

static ssize_t aw9380x_operation_mode_get(struct aw9380x_cap *p_cap, char *buf)
{
	ssize_t len = 0;

	if (p_cap->last_mode == AW9380X_ACTIVE_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Active\n");
	else if (p_cap->last_mode == AW9380X_SLEEP_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Sleep\n");
	else if (p_cap->last_mode == AW9380X_DEEPSLEEP_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: DeepSleep\n");
	else
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Unconfirmed\n");

	return len;
}

static ssize_t mode_operation_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	len = aw9380x_operation_mode_get(p_cap, buf);

	return len;
}

static ssize_t mode_operation_store(struct device *dev, struct device_attribute *attr,
									const char *buf, size_t count)
{
	uint32_t mode = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	if (kstrtouint(buf, 0, &mode) != 0) {
		AWLOGE(p_cap->dev, "kstrtouint parse err");
		return count;
	}
	AWLOGI(p_cap->dev, "set mode to %u", mode);
	aw9380x_mode_set(p_cap, mode);

	return count;
}

// set chip Soft reset start
void aw9380x_soft_reset(struct aw9380x_cap *p_cap)
{
	aw9380x_i2c_write(p_cap->i2c, REG_SA_RSTNALL, AW9380X_SOFT_RST_EN);
	msleep(AW9380X_RST_DELAY_MS);
}

static ssize_t
soft_rst_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	uint32_t flag = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	if (kstrtouint(buf, 0, &flag) != 0) {
		AWLOGE(p_cap->dev, "kstrtouint parse err");
		return count;
	}

	if (flag == AW_TRUE)
		aw9380x_soft_reset(p_cap);
	AWLOGI(p_cap->dev, "soft reset ok");

	return count;
}

// chip reset end

// reg node start
static ssize_t reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;
	uint16_t i = 0;
	uint32_t reg_data = 0;
	int32_t ret = 0;
	uint8_t reg_rd_access = 0;

	if (p_cap->p_reg_list == NULL) {
		AWLOGE(p_cap->dev, "reg list is null");
		return len;
	}

	reg_rd_access = p_cap->p_reg_list->reg_rd_access;

	for (i = 0; i < p_cap->p_reg_list->reg_num; i++) {
		if (p_cap->p_reg_list->reg_perm[i].rw & reg_rd_access) {
			ret = aw9380x_i2c_read(p_cap->i2c, p_cap->p_reg_list->reg_perm[i].reg,
					&reg_data);
			if (ret < 0)
				len += snprintf(buf + len, PAGE_SIZE - len,
						"i2c read error ret = %d\n", ret);
			len += snprintf(buf + len, PAGE_SIZE - len,
					"%x,%x\n",
					p_cap->p_reg_list->reg_perm[i].reg,
					reg_data);
		}
	}
	AWLOGI(p_cap->dev, "len %d", (int)len);

	return len;
}

static ssize_t
reg_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	uint16_t i = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	uint32_t databuf[2] = {0, 0};
	uint8_t reg_wd_access = 0;

	if (p_cap->p_reg_list == NULL) {
		AWLOGE(p_cap->dev, "AW_INVALID_PARA");
		return count;
	}

	reg_wd_access = p_cap->p_reg_list->reg_wd_access;

	if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) != 2)
		return count;

	for (i = 0; i < p_cap->p_reg_list->reg_num; i++) {
		if ((uint16_t)databuf[0] == p_cap->p_reg_list->reg_perm[i].reg) {
			if (p_cap->p_reg_list->reg_perm[i].rw & reg_wd_access) {
				aw9380x_i2c_write(p_cap->i2c,
						(uint16_t)databuf[0], (uint32_t)databuf[1]);
			}
			break;
		}
	}

	return count;
}
// reg node end

static ssize_t
update_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	uint32_t flag = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	if (kstrtouint(buf, 0, &flag) != 0) {
		AWLOGE(p_cap->dev, "kstrtouint parse error");
		return count;
	}

	if (flag == AW_TRUE) {
		aw9380x_disable_irq(p_cap);
		aw9380x_update_no_wait(p_cap);
		aw9380x_enable_irq(p_cap);
		AWLOGI(p_cap->dev, "update code ram by node end");
	}

	return count;
}

static ssize_t chip_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;
	uint32_t reg_data = 0;

	len += snprintf(buf + len, PAGE_SIZE - len,
			"driver version: %s\n", AW9380X_DRIVER_VERSION);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"reg_bin_load_state: %d\n", p_cap->reg_bin_load_flag);

	aw9380x_i2c_read(p_cap->i2c, REG_CHIP_ID0, &reg_data);
	len += snprintf(buf + len, PAGE_SIZE - len, "chipid is 0x%08x\n", reg_data);

	aw9380x_i2c_read(p_cap->i2c, REG_IRQEN, &reg_data);
	len += snprintf(buf + len, PAGE_SIZE - len, "REG_HOSTIRQEN is 0x%08x\n", reg_data);

	len += snprintf(buf + len, PAGE_SIZE - len,
			"firmware bin version:0x%08x\n", p_cap->fw_bin_version);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"INT status:%d\n", p_cap->aot_done_flag);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"code ram bin data version:0x%08x\n", p_cap->code_ram_info->bin_data_ver);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"reg bin data version:0x%08x\n", p_cap->reg_bin_info->bin_data_ver);

	return len;
}

static ssize_t aw9380x_get_diff(struct aw9380x_cap *p_cap, char *buf)
{
	uint32_t i = 0;
	int32_t ret = 0;
	ssize_t len = 0;
	uint32_t data = 0;
	int32_t diff_val = 0;
	const struct aw9380x_diff *diff = p_cap->p_diff;

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		ret = aw9380x_i2c_read(p_cap->i2c, REG_DIFF_CH0 + i * AW9380X_REG_STEP, &data);
		if (ret != AW_OK) {
			AWLOGE(p_cap->dev, "read diff err: %d", ret);
			return -AW_ERR;
		}
		diff_val = (int32_t)data / (int32_t)diff->rm_float;
		len += snprintf(buf + len, PAGE_SIZE - len, "DIFF_CH%u = %d\n", i, diff_val);
	}

	return len;
}

static void aw9380x_data_update_work_func(struct work_struct *work)
{
	struct aw9380x_cap *p_cap = container_of(work, struct aw9380x_cap, data_update_work);
	uint32_t i = 0;
	int32_t ret = 0;
	int32_t diff_data[2] = {0};
	int32_t raw_data[2] = {0};
	int32_t baseline_data[2] = {0};
	int32_t rawdata_value[2] = {0};
	int32_t diff_value[2] = {0};
	int32_t baseline_value[2] = {0};

	if (!(p_cap->pm_suspended)) {
		for (i = 0; i < AW9380X_CH01_NUM; i++) {
			ret = aw9380x_i2c_read(p_cap->i2c, REG_DIFF_CH0 + i * AW9380X_REG_STEP, &diff_data[i]);
			if (ret != AW_OK) {
				AWLOGE(p_cap->dev, "read diff err: %d", ret);
			}
			ret = aw9380x_i2c_read(p_cap->i2c, AW9380X_RAW_CH0 + i * AW9380X_REG_STEP, &raw_data[i]);
			if (ret != AW_OK) {
				AWLOGE(p_cap->dev, "read rawdata err: %d", ret);
			}
			ret = aw9380x_i2c_read(p_cap->i2c, REG_BASELINE_CH0 + i * AW9380X_REG_STEP, &baseline_data[i]);
			if (ret != AW_OK) {
				AWLOGE(p_cap->dev, "read baseline err: %d", ret);
			}
			rawdata_value[i] = (int32_t)raw_data[i] / (int32_t)AW9380X_DATA_PROCESS_FACTOR;
			diff_value[i] = (int32_t)diff_data[i] / (int32_t)AW9380X_DATA_PROCESS_FACTOR;
			baseline_value[i] = (int32_t)baseline_data[i] / (int32_t)AW9380X_DATA_PROCESS_FACTOR;
		}
		AWLOGI(p_cap->dev, "force: rawdata = %d diff = %d baseline = %d, cap: rawdata = %d diff = %d baseline = %d",
			rawdata_value[0], diff_value[0], baseline_value[0], rawdata_value[1], diff_value[1], baseline_value[1]);
		mod_timer(&p_cap->timer, jiffies + msecs_to_jiffies(awlog_time));
	}

	if (awlog_time == 0) {
		del_timer(&p_cap->timer);
	}
}

static void aw9380x_irq_state_work_handler(struct work_struct *work)
{
	struct aw9380x_cap *p_cap = container_of(work, struct aw9380x_cap, irq_state_work);
	int32_t gpio_state = 0;
	int32_t irq = 0;

	gpio_state = gpio_get_value(p_cap->dts_info.irq_gpio);
	if (gpio_state == 0) {
		gpio_low_count = gpio_low_count + 1;
	}

	if (p_cap->pm_suspended)
		return;

	if (gpio_low_count >= 3) {
		aw9380x_cap_default_irq_handle(irq, p_cap);
		gpio_low_count = 0;
	}

	if (!(p_cap->pm_suspended)) {
		mod_timer(&p_cap->irq_state_timer, jiffies + msecs_to_jiffies(AW9380X_IRQ_STATE_TIME));
	}
}

static void aw9380x_readbase_timer_handler(struct timer_list *t)
{
	struct aw9380x_cap *p_cap = from_timer(p_cap, t, timer);

	schedule_work(&p_cap->data_update_work);
	//timer handle
}

static void aw9380x_irq_state_handler(struct timer_list *t)
{
	struct aw9380x_cap *p_cap = from_timer(p_cap, t, irq_state_timer);

	schedule_work(&p_cap->irq_state_work);
	// irq state timer handle
}

static ssize_t log_timer_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	uint32_t timer_value = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	if (kstrtouint(buf, 10, &timer_value) != 0) {
		AWLOGE(p_cap->dev, "kstrtouint parse error");
		return count;
	}
	awlog_time = timer_value;
	AWLOGI(p_cap->dev, "set logtimer = %d\n", awlog_time);

	if (!(p_cap->pm_suspended)) {
		mod_timer(&p_cap->timer, jiffies + msecs_to_jiffies(awlog_time));
	}

	if (awlog_time == 0) {
		del_timer(&p_cap->timer);
	}

	return count;
}

static ssize_t log_timer_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "log_timer: %d\n", awlog_time);
	return len;
}

static ssize_t diff_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	return aw9380x_get_diff(p_cap, buf);
}


static int aw9380x_read_afedata0(struct aw9380x_cap *p_cap, int32_t *afedata0_val)
{
	uint32_t i = 0;
	unsigned int reg_val;
	int ret;
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		ret = aw9380x_i2c_read(p_cap->i2c,  REG_AEFDATA0_CH0 + i * REG_AEFDATA0_STEP, &reg_val);
		if (ret != AW_OK) {
			AWLOGE(p_cap->dev, "read afedata0 err: %d", ret);
			return -AW_ERR;
		}
		*(afedata0_val + i) = (int32_t)reg_val / AW9380X_DATA_PROCESS_FACTOR;
	}
	return AW_OK;
}

static ssize_t aw9380x_get_afedata0(struct aw9380x_cap *p_cap, char *buf)
{
	uint32_t i = 0;
	ssize_t len = 0;
	int32_t afedata0_val[AW9380X_CH_NUM_MAX] = {0};

	aw9380x_read_afedata0(p_cap, afedata0_val);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len, "AFEDATA0_CH%u = %d\n", i, afedata0_val[i]);
	}

	return len;
}

static ssize_t afedata0_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	return aw9380x_get_afedata0(p_cap, buf);
}

static void aw9380x_touch_th_write(struct aw9380x_cap *p_cap, uint32_t slide_index,
		uint32_t val, uint32_t level)
{
	uint32_t reg_addr = 0;

	if (slide_index > 11) {
		AWLOGE(p_cap->dev, "slide num too large!");
		return;
	}

	if (level) {
		reg_addr = REG_TOUCH_TH_Y_SLD0 + AW_SLD_REG_STEP * slide_index;
	} else {
		reg_addr = REG_TOUCH_TH_X_SLD0 + AW_SLD_REG_STEP * slide_index;
	}
	aw9380x_i2c_write(p_cap->i2c, reg_addr, val);
	AWLOGI(p_cap->dev, "slide_index:%d, addr:0x%08X, value:0x%08X, level:%d",
			slide_index, reg_addr, val, level);
}

static ssize_t touch_th_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	uint32_t slide_index = 0;
	uint32_t val = 0, level = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	if (sscanf(buf, "%u %u %u", &slide_index, &level, &val) != 3) {
		AWLOGE(p_cap->dev, "sscanf0 parse error!");
		return -AW_ERR;
	}
	aw9380x_touch_th_write(p_cap, slide_index, val, level);

	return count;
}

static ssize_t touch_th_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i = 0;
	uint32_t th_x_val = 0, th_y_val = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	for (i = 0; i < 12; i++) {
		aw9380x_i2c_read(p_cap->i2c, REG_TOUCH_TH_X_SLD0 + AW_SLD_REG_STEP * i, &th_x_val);
		aw9380x_i2c_read(p_cap->i2c, REG_TOUCH_TH_Y_SLD0 + AW_SLD_REG_STEP * i, &th_y_val);
		len += snprintf(buf + len, PAGE_SIZE - len, "sld:%d, touch_th_x: %u, touch_th_y:%u\r\n",
				i, th_x_val, th_y_val);
	}

	return len;
}

static ssize_t leave_th_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	uint32_t val = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	if (kstrtouint(buf, 10, &val) != 0) {
		AWLOGE(p_cap->dev, "kstrtouint parse error");
		return count;
	}

	AWLOGI(p_cap->dev, "leave_th cfg: %d\n", val);
	aw9380x_i2c_write(p_cap->i2c, REG_LEAVE_TH_X_SLD1, val);

	return count;
}

static ssize_t leave_th_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	uint32_t th_x_val = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	aw9380x_i2c_read(p_cap->i2c, REG_LEAVE_TH_X_SLD1, &th_x_val);
	len += snprintf(buf + len, PAGE_SIZE - len, "leave_th: %d\n", th_x_val);

	return len;
}

static ssize_t touch_cfg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i = 0;
	uint32_t reg_val = 0, start_index = 0, ch_num_x = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	for (i = 0; i < 12; i++) {
		aw9380x_i2c_read(p_cap->i2c, REG_BUTTON_CFG0_SLD0 + AW_SLD_REG_STEP * i, &reg_val);
		start_index = reg_val & 0xFF;
		ch_num_x = (reg_val & 0xF00) >> 8;
		len += snprintf(buf + len, PAGE_SIZE - len, "sld:%d, start_index:%u, ch_num_x:%u\r\n",
				i, start_index, ch_num_x);
	}

	return len;
}

static ssize_t touch_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i = 0;
	uint32_t reg_val = 0, touch0_st = 0, touch1_st = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	for (i = 0; i < 12; i++) {
		aw9380x_i2c_read(p_cap->i2c, REG_BUTTON_STATE_SLD0 + AW_SLD_REG_STEP * i, &reg_val);
		touch0_st = reg_val & 0x01;
		touch1_st = (reg_val & 0x02) >> 1;
		len += snprintf(buf + len, PAGE_SIZE - len, "sld:%d, touch0_st:%u, touch1_st:%u\r\n",
				i, touch0_st, touch1_st);
	}

	return len;

}

static ssize_t diff_cal_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	uint32_t chx = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	if (kstrtouint(buf, 0, &chx) != 0) {
		AWLOGE(p_cap->dev, "kstrtouint parse error");
		return count;
	}

	aw9380x_i2c_read(p_cap->i2c, REG_DIFFCAL_CH0 + chx * AW9380X_REG_STEP, &g_diff_cal_val);
	AWLOGI(p_cap->dev, "addr:0x%08X, diffcal:%d",
			REG_DIFFCAL_CH0 + chx * AW9380X_REG_STEP, g_diff_cal_val);

	return count;
}

static ssize_t diff_cal_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "diffcal = %u\n", g_diff_cal_val);

	return len;
}


/********************************* debug node end ***********************************/

/********************************* factory test node start ***********************************/
#define AW_SWITCH_MODE_DELAY_TIMES	(100)
void aw9380x_short_circuit_detect_get_stat(struct aw9380x_cap *p_cap,
		uint32_t *p_gnd_stat, uint32_t *p_vcc_stat)
{
	int i;
	uint32_t wst_val, up_st, down_st;

	aw9380x_mode_set(p_cap, AW9380X_SLEEP_MODE);
	for (i = 0; i < AW_SWITCH_MODE_DELAY_TIMES; i++) {
		aw9380x_i2c_read(p_cap->i2c, REG_WST, &wst_val);
		if (((wst_val >> 24) & 0xFF) == 0x03)
			break;
		usleep_range(1000, 1010);
	}
	if (i == AW_SWITCH_MODE_DELAY_TIMES) {
		AWLOGI(p_cap->dev, "set sleep mode error");
		return;
	}
	aw9380x_update_ch_en(p_cap, true);
	aw9380x_mode_set(p_cap, AW9380X_ACTIVE_MODE);
	msleep(AW_SHORT_DETECT_DELAY);

	aw9380x_i2c_read(p_cap->i2c, REG_CSERR_UP_ST, &up_st);
	aw9380x_i2c_read(p_cap->i2c, REG_CSERR_DOWN_ST, &down_st);
	*p_vcc_stat = up_st;
	*p_gnd_stat = down_st;

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		p_cap->fct_data.cap_cs_status[i] = AW_CS_OK;
		if (down_st & (1 << i)) {
			AWLOGI(p_cap->dev, "aw9380x channel %d connect the ground.", i);
			p_cap->fct_data.cap_cs_status[i] = AW_CS_TO_GND;
		}
		if (up_st & (1 << i)) {
			AWLOGI(p_cap->dev, "aw9380x channel %d connect the power supply.", i);
			p_cap->fct_data.cap_cs_status[i] = AW_CS_TO_VDD;
		}
	}


	if (AW_CS_ERR_DET == 0) {
		aw9380x_mode_set(p_cap, AW9380X_SLEEP_MODE);
		for (i = 0; i < AW_SWITCH_MODE_DELAY_TIMES; i++) {
			aw9380x_i2c_read(p_cap->i2c, REG_WST, &wst_val);
			if (((wst_val >> 24) & 0xFF) == 0x03)
				break;
			usleep_range(1000, 1010);
		}
		if (i == AW_SWITCH_MODE_DELAY_TIMES) {
			AWLOGI(p_cap->dev, "set sleep mode error");
			return;
		}
		aw9380x_update_ch_en(p_cap, false);
		aw9380x_mode_set(p_cap, AW9380X_ACTIVE_MODE);
	}
}

static void aw9380x_fct_short_circuit_detect(struct aw9380x_cap *p_cap)
{
	uint32_t gnd_stat = 0;
	uint32_t vcc_stat = 0;

	aw9380x_short_circuit_detect_get_stat(p_cap, &gnd_stat, &vcc_stat);
	AWLOGI(p_cap->dev, "gnd_stat: 0x%08X, vcc_stat:0x%08X", gnd_stat, vcc_stat);
}

static void aw9380x_cali_get_offset(struct aw9380x_cap *p_cap, uint32_t *offset_buf, int len)
{
	uint32_t cnt = 0, offset_f = 0, offset_c = 0, reg = 0, i = 0;
	uint32_t offset[AW9380X_CH_NUM_MAX];
	uint32_t reg_afe_cfg1[] = {REG_AFECFG1_CH0, REG_AFECFG1_CH1, REG_AFECFG1_CH2,
		REG_AFECFG1_CH3, REG_AFECFG1_CH4, REG_AFECFG1_CH5,
		REG_AFECFG1_CH6, REG_AFECFG1_CH7, REG_AFECFG1_CH8,
		REG_AFECFG1_CH9, REG_AFECFG1_CH10, REG_AFECFG1_CH11,};

	//step 1: Parasitic capacitance calibration
	aw9380x_i2c_write(p_cap->i2c, REG_SCANCTRL0, 0x00000fff);
	aw9380x_i2c_write(p_cap->i2c, REG_SCANCTRL1, 0x00000fff);

	//step 2: Waiting 1s for calibration complete flag
	while (1) {
		if (cnt >= 100) {
			AWLOGE(p_cap->dev, "aw9380x get calibration complete flag error");
			break;
		}
		aw9380x_i2c_read(p_cap->i2c, REG_STAT7, &reg);
		if (reg == 0)
			break;
		cnt++;
		usleep_range(10000, 10010);
	}

	//step 3: Read offset data and restore
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		aw9380x_i2c_read(p_cap->i2c, reg_afe_cfg1[i], &offset[i]);
		offset[i] >>= AW_BIT8;
		offset[i] &= 0x0000ffff;
		AWLOGI(p_cap->dev, "aw9380x reg = 0x%x  data = 0x%x", reg_afe_cfg1[i], offset[i]);
		offset_c = offset[i] & 0xff;
		offset_f = (offset[i] >> 8) & 0xff;
		offset[i] = (offset_c << 8) | offset_f;
		AWLOGI(p_cap->dev, "aw9380x offset[ch%d] = 0x%x(%d)",
				i, offset[i], offset[i]);
		p_cap->fct_data.cap_offset[i] = offset[i];
		if (i < len)
			offset_buf[i] = offset[i];
	}
}

static void aw9380x_fct_get_cap_offset(struct aw9380x_cap *p_cap)
{
	uint32_t offset[AW9380X_CH_NUM_MAX];

	memset(offset, 0, sizeof(offset));
	aw9380x_cali_get_offset(p_cap, offset, AW9380X_CH_NUM_MAX);
}

int aw9380x_read_diff(struct aw9380x_cap *p_cap, int *diff)
{
	int ret, i;

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		ret = aw9380x_i2c_read(p_cap->i2c, AW9380X_DIFF_CH0 + (i * AW_REG_STEP),
				(uint32_t *)diff + i);
		if (ret < 0) {
			AWLOGE(p_cap->dev, "read reg[0x%04X] diff failed: %d",
					AW9380X_DIFF_CH0 + (i * AW_REG_STEP), ret);
			return ret;
		}
		AWLOGI(p_cap->dev, "diff: ch[%d]: %d", i, (*(diff + i) >> 10));
	}

	return 0;
}

void aw9380x_diff_to_air(struct aw9380x_cap *p_cap, int *diff_max_buf,
		int *diff_min_buf, int len)
{
	uint32_t sample_cnt = AW_DIFF_TO_ARI_DATA_NUMS;
	int diff[AW9380X_CH_NUM_MAX];
	int diff_max[AW9380X_CH_NUM_MAX];
	int diff_min[AW9380X_CH_NUM_MAX];
	int i, index;

	memset(diff, 0, sizeof(diff));
	memset(diff_max, 0, sizeof(diff_max));
	memset(diff_min, 0, sizeof(diff_min));

	aw9380x_read_diff(p_cap, diff);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		diff[i] >>= 10;
		diff_max[i] = diff[i];
		diff_min[i] = diff[i];
	}

	while (1) {
		sample_cnt--;
		if (sample_cnt <= 0)
			break;
		aw9380x_read_diff(p_cap, diff);
		for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
			diff[i] >>= 10;
			index = AW_DIFF_TO_ARI_DATA_NUMS - sample_cnt - 1;
			p_cap->fct_data.cap_diff_to_air_data[i][index] = diff[i];
			if (diff_max[i] < diff[i])
				diff_max[i] = diff[i];
			if (diff_min[i] > diff[i])
				diff_min[i] = diff[i];
		}
		usleep_range(AW_DIFF_TO_ARI_DELAY, AW_DIFF_TO_ARI_DELAY + 10);
	}
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		p_cap->fct_data.cap_diff_to_air_noise_pp[i] = diff_max[i] - diff_min[i];
		AWLOGI(p_cap->dev, "aw9380x cap_diff_to_air_noise_pp[%d] = 0x%x(%d)",
				i, p_cap->fct_data.cap_diff_to_air_noise_pp[i],
				p_cap->fct_data.cap_diff_to_air_noise_pp[i]);
		AWLOGI(p_cap->dev, "aw9380x diff_max[%d] = 0x%x(%d)",
				i, diff_max[i], diff_max[i]);
		AWLOGI(p_cap->dev, "aw9380x diff_min[%d] = 0x%x(%d)",
				i, diff_min[i], diff_min[i]);
		if (i < len) {
			diff_max_buf[i] = diff_max[i];
			diff_min_buf[i] = diff_min[i];
		}
	}
	AWLOGI(p_cap->dev, "leave");
}

static void aw9380x_fct_get_cap_diff_to_air(struct aw9380x_cap *p_cap)
{
	int diff_max[AW9380X_CH_NUM_MAX];
	int diff_min[AW9380X_CH_NUM_MAX];

	memset(diff_max, 0, sizeof(diff_max));
	memset(diff_min, 0, sizeof(diff_min));
	aw9380x_diff_to_air(p_cap, diff_max, diff_min, AW9380X_CH_NUM_MAX);
}

void aw9380x_diff_approach(struct aw9380x_cap *p_cap, int *diff_buf, int len)
{
	uint32_t sample_cnt = AW_DIFF_TO_APPROACH_DATA_NUMS;
	int diff[AW9380X_CH_NUM_MAX];
	long long  diff_sum[AW9380X_CH_NUM_MAX];
	int i, index;

	memset(diff_sum, 0, sizeof(diff_sum));
	memset(diff, 0, sizeof(diff));

	while (1) {
		sample_cnt--;
		if (sample_cnt <= 0) {
			sample_cnt = AW_DIFF_TO_APPROACH_DATA_NUMS;
			break;
		}
		aw9380x_read_diff(p_cap, diff);
		for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
			diff[i] >>= 10;
			index = AW_DIFF_TO_APPROACH_DATA_NUMS - sample_cnt - 1;
			p_cap->fct_data.cap_diff_approach_data[i][index] = diff[i];
			diff_sum[i] += diff[i];
		}
		usleep_range(AW_DIFF_TO_APPROACH_DELAY, AW_DIFF_TO_APPROACH_DELAY + 10);
	}

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		p_cap->fct_data.cap_diff_avg[i] = diff_sum[i] / sample_cnt;
		AWLOGI(p_cap->dev, "aw9380x cap_diff_avg[%d] = 0x%x",
				i, p_cap->fct_data.cap_diff_avg[i]);
		if (p_cap->fct_data.cap_diff_avg[i] >= AW9380X_DIFF_PROX_MIN &&
			p_cap->fct_data.cap_diff_avg[i] <= AW9380X_DIFF_PROX_MAX)
			AWLOGI(p_cap->dev, "aw9380x diff avg check: cap_diff_avg[%d] = 0x%x",
					i, p_cap->fct_data.cap_diff_avg[i]);
		if (i < len)
			diff_buf[i] = p_cap->fct_data.cap_diff_avg[i];
	}
}

static void aw9380x_fct_get_cap_diff_to_approach(struct aw9380x_cap *p_cap)
{
	int diff_avg[AW9380X_CH_NUM_MAX];

	memset(diff_avg, 0, sizeof(diff_avg));
	aw9380x_diff_approach(p_cap, diff_avg, AW9380X_CH_NUM_MAX);
}

static void aw9380x_fct_get_force_offset(struct aw9380x_cap *p_cap, int chx)
{
	unsigned int cfg3_reg_val, cfg1_reg_val;
	int64_t cap_ofst, offset_vol_temp;
	int reg_addr, cin;
	uint32_t off_m = 0;

	aw9380x_i2c_read(p_cap->i2c,  REG_AFECFG3_CH0 + chx * AW9380X_REG_STEP, &cfg3_reg_val);
	cin = (cfg3_reg_val >> 25) & 0x0F;
	AWLOGI(p_cap->dev, "cfg3_val=%d, cin = %d", cfg3_reg_val, cin);

	reg_addr = REG_AFECFG1_CH0 + chx * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0);
	aw9380x_i2c_read(p_cap->i2c, reg_addr, &cfg1_reg_val);
	AWLOGI(p_cap->dev, "cfg1_val=%d", cfg1_reg_val);

	off_m = cfg1_reg_val & 0xFF;
	if (off_m == 0xFF)
		cap_ofst = aw9380x_get_unsigned_cap_minus(p_cap, reg_addr);  /* Expand 10000 */
	else
		cap_ofst = aw9380x_get_unsigned_cap(p_cap, reg_addr);  /* Expand 10000 */
	AWLOGI(p_cap->dev, "cap_ofst=%lld", cap_ofst);
	offset_vol_temp = (cap_ofst - 5928) * AW9380X_VREF * 1000 /
		(105 * 44 * (10 * cin + 7));

	AWLOGI(p_cap->dev, "offset_vol_temp=%lld", offset_vol_temp);
	AWLOGI(p_cap->dev, "off_m=%d", off_m);
	/* uv */
	p_cap->fct_data.force_offset_vol = off_m ? (-offset_vol_temp) : offset_vol_temp;
	AWLOGI(p_cap->dev, "force_offset_vol=%llduV", p_cap->fct_data.force_offset_vol);
}

static int aw9380x_get_rawcal_data(struct aw9380x_cap *p_cap, int chx, int *raw_out)
{
	unsigned int reg_val;
	int ret;

	ret = aw9380x_i2c_read(p_cap->i2c, AW9380X_RAWCAL_CH0 + chx * AW9380X_REG_STEP, &reg_val);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "read rawcal_data err: %d", ret);
		return -AW_ERR;
	}
	*raw_out = (int32_t)reg_val / AW9380X_DATA_PROCESS_FACTOR;

	return 0;
}

static void aw9380x_adc_transfer_vol_coef(struct aw9380x_cap *p_cap, unsigned int *adc_coef,
					unsigned int chx)
{
	int cfb_map[16] = { 11, 22, 33, 44, 66, 77, 88, 99,
			110, 121, 132, 143, 165, 176, 187, 198 };   // * 10
	unsigned int reg_val;
	long long temp_adc_coef, cfb, cin;

	aw9380x_i2c_read(p_cap->i2c,  REG_AFECFG3_CH0 + chx * AW9380X_REG_STEP, &reg_val);
	cin = (reg_val >> 25) & 0x0F;

	aw9380x_i2c_read(p_cap->i2c, REG_AFECFG0_CH0 + chx * AW9380X_REG_STEP, &reg_val);
	cfb = cfb_map[(reg_val >> 12) & 0x0F];

	temp_adc_coef = ((long long)AW9380X_VREF * 10000000LL * cfb) /
		(1048576LL * (880 * cin + 616));
	*adc_coef = (unsigned int)temp_adc_coef;
	AWLOGI(p_cap->dev, "cin = 0x%x, cfb = 0x%x, adc_coef(nV) = %d, chx = %d",
			(int)cin, (int)cfb, *adc_coef, chx);
}

static int aw9380x_save_cali_coef_to_fw(struct aw9380x_cap *p_cap, int chx,
	unsigned int reg_val)
{
	return aw9380x_i2c_write(p_cap->i2c, 0x01BC + chx * AW9380X_REG_STEP, reg_val);
}

static void aw9380x_fct_force_cali_coef_save(struct aw9380x_cap *p_cap, int chx)
{
	int test_weight = AW_SIGNAL_WEIGHT / 1000000;
	int adc_diff = p_cap->fct_data.force_signal_code;
	unsigned int reg_val = ((adc_diff & 0x3FFFFF) << 10) | (test_weight & 0x3FF);

	aw9380x_save_cali_coef_to_fw(p_cap, chx, reg_val);
}

static void aw9380x_read_cali_coef_from_fw(struct aw9380x_cap *p_cap, int chx,
				int *test_weight, int *adc_diff, int *cali_coef)
{
	unsigned int reg_val;
	int test_weight_t, adc_diff_t;

	aw9380x_i2c_read(p_cap->i2c, 0x01BC + chx * AW9380X_REG_STEP, &reg_val);
	adc_diff_t = (reg_val >> 10) & 0x3FFFFF;
	test_weight_t = (reg_val & 0x3FF) * 1000000;

	*cali_coef = test_weight_t / adc_diff_t;
	*test_weight = test_weight_t;
	*adc_diff = adc_diff_t;
}

static void aw9380x_fct_cali_coef_def_read(struct aw9380x_cap *p_cap)
{
	int i, test_weight, adc_diff, cali_coef;

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		aw9380x_read_cali_coef_from_fw(p_cap, i, &test_weight, &adc_diff, &cali_coef);
		p_cap->fct_data.force_cali_coef_def[i] = cali_coef;
		AWLOGI(p_cap->dev, "ch=%d, diff=%d, weight=%d, cali_coef_def=%d",
				i, adc_diff, test_weight,
				p_cap->fct_data.force_cali_coef_def[i]);
	}
}

static void aw9380x_fct_cali_coef_now_read(struct aw9380x_cap *p_cap)
{
	int i, test_weight, adc_diff, cali_coef;

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		aw9380x_read_cali_coef_from_fw(p_cap, i, &test_weight, &adc_diff, &cali_coef);
		p_cap->fct_data.force_cali_coef_now[i] = cali_coef;
		p_cap->fct_data.force_test_weight_now[i] = test_weight;
		p_cap->fct_data.force_signal_code_now[i] = adc_diff;
		AWLOGI(p_cap->dev, "ch=%d, signal_code_now=%d, test_weight_now=%d, cali_coef_now=%d",
				i, adc_diff, test_weight, cali_coef);
	}
}

#define SQRT_PRECISION			(0)
#define SQRT_ITERATION_BASE		(31)
#define SQRT_REMLOW_DECAY		(SQRT_ITERATION_BASE << 1)

static uint64_t aw_sqrt(uint64_t x)
{
	uint16_t iterations = SQRT_ITERATION_BASE + (SQRT_PRECISION >> 1);
	uint16_t i = 0;
	uint64_t res = 0;
	uint64_t remainder_high = 0;
	uint64_t divisor = 0;
	uint64_t remainder_low = x;

	for (i = 0; i <= iterations; i++) {
		remainder_high = (remainder_high << 2) | (remainder_low >> SQRT_REMLOW_DECAY);
		remainder_low <<= 2;
		res <<= 1;
		divisor = (res << 1) + 1;
		if (remainder_high >= divisor) {
			remainder_high -= divisor;
			res += 1;
		}
	}
	return res;
}

static void aw9380x_fct_get_force_afe_noise(struct aw9380x_cap *p_cap, int chx)
{
	int i;
	int32_t noise_min, noise_max;
	int32_t delta, old_data;
	long long data_sum = 0;
	unsigned int adc_coef;

	int32_t afedata0_val[AW9380X_CH_NUM_MAX] = {0};

	for (i = 0; i < AW9380X_NOISE_DATA_NUMS; i++) {
		aw9380x_read_afedata0(p_cap, afedata0_val);
		AWLOGI(p_cap->dev, "------%d-----afedata0_val[%d]=0x%X(%d)-------",
			i, chx, afedata0_val[chx], afedata0_val[chx]);
		p_cap->fct_data.force_noise_afe_data[i] = afedata0_val[chx];
		if (i == 0) {
			noise_min = afedata0_val[chx];
			noise_max = afedata0_val[chx];
			old_data = afedata0_val[chx];
			p_cap->fct_data.force_afe_noise_peak = 0;
			p_cap->fct_data.force_afe_noise_pp = 0;
		} else {
			delta = old_data > afedata0_val[chx] ?
				old_data - afedata0_val[chx] :
				afedata0_val[chx] - old_data;
			p_cap->fct_data.force_afe_noise_peak = p_cap->fct_data.force_afe_noise_peak > delta ?
					p_cap->fct_data.force_afe_noise_peak : delta;

			data_sum += (delta * delta);
			if (noise_min > afedata0_val[chx])
				noise_min = afedata0_val[chx];
			if (noise_max < afedata0_val[chx])
				noise_max = afedata0_val[chx];

			old_data = afedata0_val[chx];
		}
		AWLOGI(p_cap->dev, "noise_min[%d]= %d, noise_max[%d]= %d",
			chx, noise_min, chx, noise_max);
		usleep_range(AW_NOISE_DELAY, AW_NOISE_DELAY + 10);
	}
	p_cap->fct_data.force_afe_noise_pp = noise_max - noise_min;
	p_cap->fct_data.force_afe_noise_std = aw_sqrt(data_sum / (AW9380X_NOISE_DATA_NUMS - 1));
	AWLOGI(p_cap->dev, "data_sum[%d]= %lld", chx, data_sum);
	AWLOGI(p_cap->dev, "force_afe_noise_pp[%d]= %d", chx, p_cap->fct_data.force_afe_noise_pp);
	AWLOGI(p_cap->dev, "force_afe_noise_peak[%d]= %d", chx, p_cap->fct_data.force_afe_noise_peak);
	AWLOGI(p_cap->dev, "force_afe_noise_std[%d]= %d", chx, p_cap->fct_data.force_afe_noise_std);

	aw9380x_adc_transfer_vol_coef(p_cap, &adc_coef, chx);
	p_cap->fct_data.force_afe_noise_pp_vol = p_cap->fct_data.force_afe_noise_pp * adc_coef;	/* nv */
	p_cap->fct_data.force_afe_noise_peak_vol = p_cap->fct_data.force_afe_noise_peak * adc_coef;	/* nv */
	p_cap->fct_data.force_afe_noise_std_vol = p_cap->fct_data.force_afe_noise_std * adc_coef;	/* nv */
	AWLOGI(p_cap->dev, "force_afe_noise_pp_vol[%d]= %lldnV", chx, p_cap->fct_data.force_afe_noise_pp_vol);
	AWLOGI(p_cap->dev, "force_afe_noise_peak_vol[%d]= %lldnV", chx, p_cap->fct_data.force_afe_noise_peak_vol);
	AWLOGI(p_cap->dev, "force_afe_noise_std_vol[%d]= %lldnV", chx, p_cap->fct_data.force_afe_noise_std_vol);
}

static void aw9380x_fct_get_force_noise(struct aw9380x_cap *p_cap, int chx)
{
	int i, delta, old_data;
	int rawcal_data = 0, noise_min = 0, noise_max = 0;
	long long data_sum = 0;
	unsigned int adc_coef;

	for (i = 0; i < AW9380X_NOISE_DATA_NUMS; i++) {
		aw9380x_get_rawcal_data(p_cap, chx, &rawcal_data);
		AWLOGI(p_cap->dev, "------%d-----rawcal_data=0x%X(%d)-------",
				i, rawcal_data, rawcal_data);
		p_cap->fct_data.force_noise_rawcal_data[i] = rawcal_data;
		if (i == 0) {
			noise_min = rawcal_data;
			noise_max = rawcal_data;
			old_data = rawcal_data;
			p_cap->fct_data.force_noise_peak = 0;
			p_cap->fct_data.force_noise_pp = 0;
		} else {
			delta = old_data > rawcal_data ?
				old_data - rawcal_data :
				rawcal_data - old_data;
			p_cap->fct_data.force_noise_peak = p_cap->fct_data.force_noise_peak > delta ?
					p_cap->fct_data.force_noise_peak : delta;

			data_sum += (delta * delta);
			if (noise_min > rawcal_data)
				noise_min = rawcal_data;
			if (noise_max < rawcal_data)
				noise_max = rawcal_data;

			old_data = rawcal_data;
		}
		AWLOGI(p_cap->dev, "noise_min[%d]= 0x%X, noise_max[%d]= 0x%X",
			chx, noise_min, chx, noise_max);
		usleep_range(AW_NOISE_DELAY, AW_NOISE_DELAY + 10);
	}
	p_cap->fct_data.force_noise_pp = noise_max - noise_min;
	p_cap->fct_data.force_noise_std = aw_sqrt(data_sum / (AW9380X_NOISE_DATA_NUMS - 1));
	AWLOGI(p_cap->dev, "data_sum[%d]= %lld", chx, data_sum);
	AWLOGI(p_cap->dev, "force_noise_pp[%d]= %d", chx, p_cap->fct_data.force_noise_pp);
	AWLOGI(p_cap->dev, "force_noise_peak[%d]= %d", chx, p_cap->fct_data.force_noise_peak);
	AWLOGI(p_cap->dev, "force_noise_std[%d]= %d", chx, p_cap->fct_data.force_noise_std);

	aw9380x_adc_transfer_vol_coef(p_cap, &adc_coef, chx);
	p_cap->fct_data.force_noise_pp_vol = p_cap->fct_data.force_noise_pp * adc_coef;	/* nv */
	p_cap->fct_data.force_noise_peak_vol = p_cap->fct_data.force_noise_peak * adc_coef;	/* nv */
	p_cap->fct_data.force_noise_std_vol = p_cap->fct_data.force_noise_std * adc_coef;	/* nv */
	AWLOGI(p_cap->dev, "force_noise_pp_vol[%d]= %lldnV", chx, p_cap->fct_data.force_noise_pp_vol);
	AWLOGI(p_cap->dev, "force_noise_peak_vol[%d]= %lldnV", chx, p_cap->fct_data.force_noise_peak_vol);
	AWLOGI(p_cap->dev, "force_noise_std_vol[%d]= %lldnV", chx, p_cap->fct_data.force_noise_std_vol);
}

static void aw9380x_raw_data_avg_get(struct aw9380x_cap *p_cap, int chx, unsigned int *raw_data)
{
	unsigned int reg_val;
	int i, process_data;
	long long sum_data = 0;

	for (i = 0; i < AW_RAW_DATA_NUM; i++) {
		aw9380x_i2c_read(p_cap->i2c, AW9380X_RAWCAL_CH0 + chx * AW9380X_REG_STEP, &reg_val);
		process_data = (int)reg_val / AW9380X_DATA_PROCESS_FACTOR;
		sum_data += process_data;
		usleep_range(AW_RAWCAL_DATA_DELAY, AW_RAWCAL_DATA_DELAY + 10);
	}
	*raw_data = sum_data / AW_RAW_DATA_NUM;
	AWLOGI(p_cap->dev, "chx = %d, raw_data=0x%X", chx, *raw_data);
}

static void aw9380x_fct_get_force_signal_step_1(struct aw9380x_cap *p_cap, int chx)
{
	int raw_data1;

	aw9380x_raw_data_avg_get(p_cap, chx, &raw_data1);
	p_cap->fct_data.force_signal_raw_data1 = raw_data1;
	AWLOGI(p_cap->dev, "force_signal_raw_data1=%d", p_cap->fct_data.force_signal_raw_data1);
}

static void aw9380x_fct_get_force_signal_step_2(struct aw9380x_cap *p_cap, int chx)
{
	unsigned int adc_coef;
    int raw_data2;
    int signal_code;
	int64_t signal_vol;
    int cali_coef;

	aw9380x_raw_data_avg_get(p_cap, chx, &raw_data2);
	p_cap->fct_data.force_signal_raw_data2 = raw_data2;
	AWLOGI(p_cap->dev, "force_signal_raw_data2=%d", p_cap->fct_data.force_signal_raw_data2);

	aw9380x_adc_transfer_vol_coef(p_cap, &adc_coef, chx);
	AWLOGI(p_cap->dev, "adc_coef=%dnV/Code", adc_coef);

	signal_code = p_cap->fct_data.force_signal_raw_data2 - p_cap->fct_data.force_signal_raw_data1;
	signal_vol = (int64_t)(signal_code) * adc_coef;

	p_cap->fct_data.force_signal_code = signal_code;
	p_cap->fct_data.force_signal_vol = signal_vol;
	AWLOGI(p_cap->dev, "force_signal_code=%d, force_signal_vol=%lldnV",
			p_cap->fct_data.force_signal_code, p_cap->fct_data.force_signal_vol);

	cali_coef = AW_SIGNAL_WEIGHT / signal_code;
	p_cap->fct_data.force_cali_coef = cali_coef;
	AWLOGI(p_cap->dev, "force_cali_coef=%d(ug/code)", p_cap->fct_data.force_cali_coef);
}

static void aw9380x_fct_veri_force_coef_step_1(struct aw9380x_cap *p_cap, int chx)
{
	int veri_raw_data1;

	aw9380x_raw_data_avg_get(p_cap, chx, &veri_raw_data1);
	p_cap->fct_data.force_veri_raw_data1 = veri_raw_data1;
	AWLOGI(p_cap->dev, "force_veri_raw_data1=%d", p_cap->fct_data.force_veri_raw_data1);
}

static void aw9380x_fct_veri_force_coef_step_2(struct aw9380x_cap *p_cap, int chx)
{
	int veri_raw_data2;

	aw9380x_raw_data_avg_get(p_cap, chx, &veri_raw_data2);
	p_cap->fct_data.force_veri_raw_data2 = veri_raw_data2;
	AWLOGI(p_cap->dev, "force_veri_raw_data2=%d", p_cap->fct_data.force_veri_raw_data2);

	p_cap->fct_data.force_veri_signal_code = p_cap->fct_data.force_veri_raw_data2 - p_cap->fct_data.force_veri_raw_data1;
    AWLOGI(p_cap->dev, "force_veri_signal_code=%d", p_cap->fct_data.force_veri_signal_code);

	if(p_cap->fct_data.force_cali_coef <= 0) {
		AWLOGE(p_cap->dev, "fct_data.force_cali_coef read from FW, Need FTC calibration first!");
        aw9380x_fct_cali_coef_now_read(p_cap);
		p_cap->fct_data.force_cali_coef = p_cap->fct_data.force_cali_coef_now[chx];
	}

	p_cap->fct_data.force_mass_weight = p_cap->fct_data.force_cali_coef * p_cap->fct_data.force_veri_signal_code;
	p_cap->fct_data.force_mass_deviation = ((p_cap->fct_data.force_mass_weight - (long long)AW_VERI_WEIGHT) *
			10000) / (long long)AW_VERI_WEIGHT;

	AWLOGI(p_cap->dev, "p_cap->fct_data.force_mass_weight=%lldug", p_cap->fct_data.force_mass_weight);
	AWLOGI(p_cap->dev, "p_cap->fct_data.force_mass_deviation=%d.%d%%", 
		p_cap->fct_data.force_mass_deviation/100, abs(p_cap->fct_data.force_mass_deviation%100));
}

static void aw9380x_fct_data_save_to_flash(struct aw9380x_cap *p_cap)
{
	/*
	 * TODO:
	 * The user needs to implement a function to wirte flash and
	 * fill the data retrieved into struct.
	 * 	struct aw9380x_factory_force_data factory_force_data[AW9380X_CH_NUM_MAX];
	    struct aw9380x_factory_cap_data factory_cap_data[AW9380X_CH_NUM_MAX];
	 */

	 AWLOGI(p_cap->dev, "Enter");
	 /* example start */
	 /* example end */

}

static void aw9380x_fct_data_read_from_flash(struct aw9380x_cap *p_cap)
{
	/*
	 * TODO:
	 * The user needs to implement a function to read from flash and
	 * fill the data retrieved into struct.
	 * 	struct aw9380x_factory_force_data factory_force_data[AW9380X_CH_NUM_MAX];
	 *  struct aw9380x_factory_cap_data factory_cap_data[AW9380X_CH_NUM_MAX];
	 */
	AWLOGI(p_cap->dev, "Enter");
	/* example start */
	/* example end */

}

static void aw9380x_fct_data_write_back_to_fw(struct aw9380x_cap *p_cap)
{
	/*
	 * TODO:
	 * The user needs to implement a function to read factory data from flash and wirte into FW.
	 * 	struct aw9380x_factory_force_data factory_force_data[AW9380X_CH_NUM_MAX];
	 *  struct aw9380x_factory_cap_data factory_cap_data[AW9380X_CH_NUM_MAX];
	 */

	AWLOGI(p_cap->dev, "Enter");
	aw9380x_fct_data_read_from_flash(p_cap);
	/* example start */
	/*
	 * force: fct_force_factory_coeff_signal_store
     * cap:   fct_cap_factory_touch_th_store
	*/
	/* example end */
}

static void aw9380x_factory_test(unsigned int cmd, int chx)
{
	struct aw9380x_cap *p_cap = g_p_cap;

	AWLOGI(p_cap->dev, "Enter cmd = %d", cmd);

	switch (cmd) {
	case AW_FCT_CAP_SHORT_CIRCUIT_DETECT:
		aw9380x_fct_short_circuit_detect(p_cap);
	break;

	case AW_FCT_CAP_OFFSET_CALI:
	    aw9380x_fct_get_cap_offset(p_cap);
	break;

	case AW_FCT_CAP_DIFF_TO_AIR_NOISE:
		aw9380x_fct_get_cap_diff_to_air(p_cap);
	break;

	case AW_FCT_CAP_DIFF_TO_APPROACH:
		aw9380x_fct_get_cap_diff_to_approach(p_cap);
	break;

	case AW_FCT_FORCE_OFFSET:
		aw9380x_fct_get_force_offset(p_cap, chx);
	break;

	case AW_FCT_FORCE_NOISE:
		aw9380x_fct_get_force_noise(p_cap, chx);
	break;
    
	case AW_FCT_FORCE_AFE_NOISE:
	aw9380x_fct_get_force_afe_noise(p_cap, chx);
	break;

	case AW_FCT_FORCE_SIGNAL_1:
	aw9380x_fct_get_force_signal_step_1(p_cap, chx);
	break;

	case AW_FCT_FORCE_SIGNAL_2:
	aw9380x_fct_get_force_signal_step_2(p_cap, chx);
	break;

	case AW_FCT_FORCE_VERI_COEF_1:
		aw9380x_fct_veri_force_coef_step_1(p_cap, chx);
	break;

	case AW_FCT_FORCE_VERI_COEF_2:
	aw9380x_fct_veri_force_coef_step_2(p_cap, chx);
	break;

	case AW_FCT_FORCE_CALI_COEF_SAVE:
		aw9380x_fct_force_cali_coef_save(p_cap, chx);
	break;

	default:
		AWLOGI(p_cap->dev, "Unsupport cmd!!");
	break;
	}
}

static ssize_t fct_cap_short_circuit_detect_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;
	AWLOGI(p_cap->dev, "Enter");
	mutex_lock(&aw9380x_lock);
	aw9380x_factory_test(AW_FCT_CAP_SHORT_CIRCUIT_DETECT, 0);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		if (p_cap->fct_data.cap_cs_status[i] == AW_CS_TO_VDD)
			len += snprintf(buf + len, PAGE_SIZE - len,
			"cs[%d] connect to VDD\r\n", i);
		else if (p_cap->fct_data.cap_cs_status[i] == AW_CS_TO_GND)
			len += snprintf(buf + len, PAGE_SIZE - len,
			"cs[%d] connect to GND\r\n", i);
		else
			len += snprintf(buf + len, PAGE_SIZE - len,
			"cs[%d] ok\r\n", i);
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_cap_offset_cali_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int i, offset_f, offset_c;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	aw9380x_factory_test(AW_FCT_CAP_OFFSET_CALI, 0);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		offset_f = p_cap->fct_data.cap_offset[i] & 0xFF;
		offset_c = (p_cap->fct_data.cap_offset[i] & 0xFF00) >> 8;
		len += snprintf(buf + len, PAGE_SIZE - len,
				"aw9380x offset[ch%d] = 0x%x(%d)(%dpf)\r\n",
				i, p_cap->fct_data.cap_offset[i],
				p_cap->fct_data.cap_offset[i],
				(offset_f * 152 + offset_c * 9900) / 10000);
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_cap_diff_to_air_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	aw9380x_factory_test(AW_FCT_CAP_DIFF_TO_AIR_NOISE, 0);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"aw9380x cap_diff_to_air_noise_pp[%d] = 0x%x(%d)\r\n",
				i, p_cap->fct_data.cap_diff_to_air_noise_pp[i],
				p_cap->fct_data.cap_diff_to_air_noise_pp[i]);
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_cap_diff_approach_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	aw9380x_factory_test(AW_FCT_CAP_DIFF_TO_APPROACH, 0);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"aw9380x cap_diff_avg[%d] = 0x%x\r\n",
				i, p_cap->fct_data.cap_diff_avg[i]);
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_offset_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int chx;

	AWLOGI(p_cap->dev, "Enter");

	if (kstrtoint(buf, 0, &chx) != 0) {
		AWLOGE(p_cap->dev, "kstrtoint parse error!");
		return -AW_ERR;
	}

	mutex_lock(&aw9380x_lock);
	aw9380x_factory_test(AW_FCT_FORCE_OFFSET, chx);
	mutex_unlock(&aw9380x_lock);

	return count;
}

static ssize_t fct_force_offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"force_offset_vol = %llduv\r\n", p_cap->fct_data.force_offset_vol);
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_noise_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int chx;

	AWLOGI(p_cap->dev, "Enter");

	if (kstrtoint(buf, 0, &chx) != 0) {
		AWLOGE(p_cap->dev, "kstrtoint parse error!");
		return -AW_ERR;
	}
	mutex_lock(&aw9380x_lock);
	aw9380x_factory_test(AW_FCT_FORCE_NOISE, chx);
	mutex_unlock(&aw9380x_lock);

	return count;
}

static ssize_t fct_force_noise_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int i = 0, j = 0, k = 0, raw_data_nums = 10;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"noise_pp = %d, noise_peak = %d, noise_std = %d\r\n",
			p_cap->fct_data.force_noise_pp, p_cap->fct_data.force_noise_peak,
			p_cap->fct_data.force_noise_std);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"noise_pp_nV = %lld, noise_peak_nV = %lld, noise_std_nV = %lld\r\n",
			p_cap->fct_data.force_noise_pp_vol, p_cap->fct_data.force_noise_peak_vol,
			p_cap->fct_data.force_noise_std_vol);

	len += snprintf(buf + len, PAGE_SIZE - len, "rawcal_data:\r\n");
	for (i = 0; i < AW9380X_NOISE_DATA_NUMS / raw_data_nums; i++) {
		for (k = 0; k < raw_data_nums; k++) {
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%X,",
				p_cap->fct_data.force_noise_rawcal_data[i * raw_data_nums + k]);
			if (k == raw_data_nums - 1)
				len += snprintf(buf + len, PAGE_SIZE - len, "\r\n");
		}
	}
	if (AW9380X_NOISE_DATA_NUMS % raw_data_nums) {
		for (j = 0; j < AW9380X_NOISE_DATA_NUMS % raw_data_nums; j++)
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%X,",
				p_cap->fct_data.force_noise_rawcal_data[i * raw_data_nums + j]);
		len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
	} else {
		len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
	}

	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_afe_noise_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
int chx;

AWLOGI(p_cap->dev, "Enter");

if (kstrtoint(buf, 0, &chx) != 0) {
AWLOGE(p_cap->dev, "kstrtoint parse error!");
return -AW_ERR;
}
mutex_lock(&aw9380x_lock);
aw9380x_factory_test(AW_FCT_FORCE_AFE_NOISE, chx);
mutex_unlock(&aw9380x_lock);

return count;
}

static ssize_t fct_force_afe_noise_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int i = 0, j = 0, k = 0, afe_data_nums = 10;
	int t = 0;
	t = AW9380X_NOISE_DATA_NUMS % afe_data_nums;
	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	len += snprintf(buf + len, PAGE_SIZE - len,
	    "force_afe_noise_pp = %d, force_afe_noise_peak = %d, force_afe_noise_std = %d\r\n",
	    p_cap->fct_data.force_afe_noise_pp, p_cap->fct_data.force_afe_noise_peak, 
	    p_cap->fct_data.force_afe_noise_std);
	len += snprintf(buf + len, PAGE_SIZE - len,
	    "force_afe_noise_pp_nV = %lld, force_afe_noise_peak_nV = %lld, force_afe_noise_std_nV = %lld\r\n",
	    p_cap->fct_data.force_afe_noise_pp_vol, p_cap->fct_data.force_afe_noise_peak_vol,
	    p_cap->fct_data.force_afe_noise_std_vol);

	len += snprintf(buf + len, PAGE_SIZE - len, "afe_data:\r\n");
	for (i = 0; i < AW9380X_NOISE_DATA_NUMS / afe_data_nums; i++) {
	    for (k = 0; k < afe_data_nums; k++) {
	        len += snprintf(buf + len, PAGE_SIZE - len, "%d,",
	            p_cap->fct_data.force_noise_afe_data[i * afe_data_nums + k]);
	        if (k == afe_data_nums - 1)
	            len += snprintf(buf + len, PAGE_SIZE - len, "\r\n");
	    }
	}
	if (AW9380X_NOISE_DATA_NUMS % afe_data_nums) {
		for (j = 0; j < t; j++) {
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%X,",
			p_cap->fct_data.force_noise_afe_data[(i * afe_data_nums) + j]);
			len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
			}
	} else {
		len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
	}

	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_signal_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int chx, ops_flag;

	AWLOGI(p_cap->dev, "Enter");

	if (sscanf(buf, "%d %d", &chx, &ops_flag) != 2) {
		AWLOGE(p_cap->dev, "sscanf0 parse error!");
		return -AW_ERR;
	}

	AWLOGI(p_cap->dev, "test %d, %d!", chx, ops_flag);
	mutex_lock(&aw9380x_lock);
	if (ops_flag == 1)
		aw9380x_factory_test(AW_FCT_FORCE_SIGNAL_1, chx);

	if (ops_flag == 2)
		aw9380x_factory_test(AW_FCT_FORCE_SIGNAL_2, chx);
	mutex_unlock(&aw9380x_lock);

	return count;
}

static ssize_t fct_force_signal_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"force_signal_code = %d, force_signal_vol = %lld, force_cali_coef=0x%X\r\n",
			p_cap->fct_data.force_signal_code, p_cap->fct_data.force_signal_vol,
			p_cap->fct_data.force_cali_coef);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"signal_weight = %d, force_signal_raw_data1 = %d, force_signal_raw_data2 = %d\r\n",
			AW_SIGNAL_WEIGHT, p_cap->fct_data.force_signal_raw_data1,
			p_cap->fct_data.force_signal_raw_data2);
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_cali_coef_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int chx;

	AWLOGI(p_cap->dev, "Enter");

	if (kstrtoint(buf, 0, &chx) != 0) {
		AWLOGE(p_cap->dev, "kstrtoint parse error!");
		return -AW_ERR;
	}

	mutex_lock(&aw9380x_lock);
	aw9380x_factory_test(AW_FCT_FORCE_CALI_COEF_SAVE, chx);
	mutex_unlock(&aw9380x_lock);

	return count;
}

static ssize_t fct_force_cali_coef_show(struct device *dev, struct device_attribute *attr, char *buf)
{

	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	aw9380x_fct_cali_coef_now_read(p_cap);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
			"chx=%d, cali_coef_now(ug/code)=%d, test_weight_now=%d, signal_code_now=%d\r\n",
				i, p_cap->fct_data.force_cali_coef_now[i],
				p_cap->fct_data.force_test_weight_now[i],
				p_cap->fct_data.force_signal_code_now[i]);
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_cali_coef_default_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"fct_force_cali_coef_def(ug/code)[%d] = %d\r\n",
				i, p_cap->fct_data.force_cali_coef_def[i]);
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_mass_deviation_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int chx, ops_flag;

	AWLOGI(p_cap->dev, "Enter");

	if (sscanf(buf, "%d %d", &chx, &ops_flag) != 2) {
		AWLOGE(p_cap->dev, "sscanf0 parse error!");
		return -AW_ERR;
	}

	mutex_lock(&aw9380x_lock);
	if (ops_flag == 1)
		aw9380x_factory_test(AW_FCT_FORCE_VERI_COEF_1, chx);

	if (ops_flag == 2)
		aw9380x_factory_test(AW_FCT_FORCE_VERI_COEF_2, chx);
	mutex_unlock(&aw9380x_lock);

	return count;
}

static ssize_t fct_force_mass_deviation_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"mass_deviation = %d, mass_weight = %lld\r\n", p_cap->fct_data.force_mass_deviation,
			 p_cap->fct_data.force_mass_weight);
	len += snprintf(buf + len, PAGE_SIZE - len,
			"veri_weight = %d, force_veri_raw_data1 = %d, force_veri_raw_data2 = %d\r\n",
			AW_VERI_WEIGHT, p_cap->fct_data.force_veri_raw_data1,
			p_cap->fct_data.force_veri_raw_data2);
	mutex_unlock(&aw9380x_lock);

	return len;
}
static ssize_t fct_data_save_flash_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int flag;

	AWLOGI(p_cap->dev, "Enter");

	if (kstrtoint(buf, 0, &flag) != 0) {
		AWLOGE(p_cap->dev, "kstrtoint parse error!");
		return -AW_ERR;
	}

	if (flag)
		aw9380x_fct_data_save_to_flash(p_cap);

	return count;
}

static ssize_t fct_data_save_flash_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int slide_index;
	int force_chx;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	aw9380x_fct_data_read_from_flash(p_cap);
	/*
	 * TODO:
     * The user needs to implement a function to read from flash and display the data of struct .
	 * struct aw9380x_factory_force_data factory_force_data[AW9380X_CH_NUM_MAX];
	 * struct aw9380x_factory_cap_data factory_cap_data[AW9380X_CH_NUM_MAX];
	 */

	for (slide_index = 0; slide_index < AW9380X_CH_NUM_MAX; slide_index++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"sld:%d, touch_th_x: %u, touch_th_y:%u\r\n",
				slide_index,
				p_cap->factory_cap_data[slide_index].cap_touch_th_x,
				p_cap->factory_cap_data[slide_index].cap_touch_th_y);
	}

	for (force_chx = 0; force_chx < AW9380X_CH_NUM_MAX; force_chx++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"force_chx:%d, force_cali_coef_signal_val: %u, force_cali_coef_reg_val:0x%x\r\n",
				force_chx,
				p_cap->factory_force_data[force_chx].force_cali_coef_signal_val,
				p_cap->factory_force_data[force_chx].force_cali_coef_reg_val);
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_cap_diff_to_air_data_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int chx;

	AWLOGI(p_cap->dev, "Enter");

	if (kstrtoint(buf, 0, &chx) != 0) {
		AWLOGE(p_cap->dev, "kstrtoint parse error!");
		return -AW_ERR;
	}
	p_cap->fct_data.cap_diff_to_air_data_chx = chx;

	return count;
}

static ssize_t fct_cap_diff_to_air_data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int i, j, k, chx, raw_data_nums = 10;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	chx = p_cap->fct_data.cap_diff_to_air_data_chx;
	len += snprintf(buf + len, PAGE_SIZE - len, "diff_to_air_data:ch%d\r\n",
				chx);
	for (i = 0; i < AW_DIFF_TO_ARI_DATA_NUMS / raw_data_nums; i++) {
		for (k = 0; k < raw_data_nums; k++) {
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%X,",
				p_cap->fct_data.cap_diff_to_air_data[chx][i * raw_data_nums + k]);
			if (k == raw_data_nums - 1)
				len += snprintf(buf + len, PAGE_SIZE - len, "\r\n");
		}
	}
	if (AW_DIFF_TO_ARI_DATA_NUMS % raw_data_nums) {
		for (j = 0; j < AW_DIFF_TO_ARI_DATA_NUMS % raw_data_nums; j++)
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%X,",
				p_cap->fct_data.cap_diff_to_air_data[chx][i * raw_data_nums + j]);
		len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
	} else {
		len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_cap_diff_approach_data_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int chx;

	AWLOGI(p_cap->dev, "Enter");

	if (kstrtoint(buf, 0, &chx) != 0) {
		AWLOGE(p_cap->dev, "kstrtoint parse error!");
		return -AW_ERR;
	}
	p_cap->fct_data.cap_diff_approach_data_chx = chx;

	return count;
}

static ssize_t fct_cap_diff_approach_data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	int i, j, k, chx, raw_data_nums = 10;

	AWLOGI(p_cap->dev, "Enter");

	mutex_lock(&aw9380x_lock);
	chx = p_cap->fct_data.cap_diff_approach_data_chx;
	len += snprintf(buf + len, PAGE_SIZE - len, "diff_approach_data:ch%d\r\n",
				chx);
	for (i = 0; i < AW_DIFF_TO_APPROACH_DATA_NUMS / raw_data_nums; i++) {
		for (k = 0; k < raw_data_nums; k++) {
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%X,",
				p_cap->fct_data.cap_diff_approach_data[chx][i * raw_data_nums + k]);
			if (k == raw_data_nums - 1)
				len += snprintf(buf + len, PAGE_SIZE - len, "\r\n");
		}
	}
	if (AW_DIFF_TO_APPROACH_DATA_NUMS % raw_data_nums) {
		for (j = 0; j < AW_DIFF_TO_APPROACH_DATA_NUMS % raw_data_nums; j++)
			len += snprintf(buf + len, PAGE_SIZE - len, "0x%X,",
				p_cap->fct_data.cap_diff_approach_data[chx][i * raw_data_nums + j]);
		len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
	} else {
		len += snprintf(buf + len, PAGE_SIZE - len, "END\r\n");
	}
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t fct_force_factory_coeff_signal_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	uint32_t test_weight = AW_SIGNAL_WEIGHT / 1000000;
	uint32_t signal_val, force_chx, reg_val;

	AWLOGI(p_cap->dev, "Enter");

	if (sscanf(buf, "%u %u", &force_chx, &signal_val) != 2) {
		AWLOGE(p_cap->dev, "sscanf0 parse error!");
		return -AW_ERR;
	}

	reg_val = ((signal_val & 0x3FFFFF) << 10) | (test_weight & 0x3FF);

	p_cap->factory_force_data[force_chx].force_cali_coef_signal_val = signal_val;
	p_cap->factory_force_data[force_chx].force_cali_coef_reg_val = reg_val;

	aw9380x_save_cali_coef_to_fw(p_cap, force_chx, reg_val);

	return count;
}

static ssize_t fct_force_factory_coeff_signal_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int force_chx = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	AWLOGI(p_cap->dev, "Enter");

	for (force_chx = 0; force_chx < AW9380X_CH_NUM_MAX; force_chx++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"force_chx:%d, force_cali_coef_signal_val: %u, force_cali_coef_reg_val:0x%x\r\n",
				force_chx,
				p_cap->factory_force_data[force_chx].force_cali_coef_signal_val,
				p_cap->factory_force_data[force_chx].force_cali_coef_reg_val);
	}

	return len;
}


static ssize_t fct_cap_factory_touch_th_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	uint32_t slide_index = 0;
	uint32_t val = 0, level = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	AWLOGI(p_cap->dev, "Enter");

	if (sscanf(buf, "%u %u %u", &slide_index, &level, &val) != 3) {
		AWLOGE(p_cap->dev, "sscanf0 parse error!");
		return -AW_ERR;
	}

	if (level)
		p_cap->factory_cap_data[slide_index].cap_touch_th_y = val;
	else
		p_cap->factory_cap_data[slide_index].cap_touch_th_x = val;

	aw9380x_touch_th_write(p_cap, slide_index, val, level);

	return count;
}

static ssize_t fct_cap_factory_touch_th_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int slide_index = 0;
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

	AWLOGI(p_cap->dev, "Enter");
	for (slide_index = 0; slide_index < AW9380X_CH_NUM_MAX; slide_index++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"sld:%d, touch_th_x: %u, touch_th_y:%u\r\n",
				slide_index,
				p_cap->factory_cap_data[slide_index].cap_touch_th_x,
				p_cap->factory_cap_data[slide_index].cap_touch_th_y);
	}

	return len;
}

static ssize_t calibration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct aw9380x_cap *p_cap = dev_get_drvdata(dev);
	ssize_t len = 0;

	mutex_lock(&aw9380x_lock);
	len += snprintf(buf + len, PAGE_SIZE - len, "%d\r\n",p_cap->fct_data.force_signal_code);
	mutex_unlock(&aw9380x_lock);

	return len;
}

static ssize_t touch_scanperiod_store(struct device *dev, struct device_attribute *attr,
    const char *buf, size_t count)
{
    uint32_t scanperiod_ms = 0;
    struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

    if (sscanf(buf, "%u", &scanperiod_ms) != 1) {
        AWLOGE(p_cap->dev, "sscanf0 parse error!");
        return -AW_ERR;
    }
    aw9380x_i2c_write_bits(p_cap->i2c, REG_SCANCTRL3, ~(0xffff), scanperiod_ms);
    AWLOGI(p_cap->dev, "scanperiod_ms:%d", scanperiod_ms);

    return count;
}

static ssize_t touch_scanperiod_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t len = 0;
    uint32_t scanperiod_ms = 0;
    uint32_t reg_val = 0;
    struct aw9380x_cap *p_cap = dev_get_drvdata(dev);

    aw9380x_i2c_read(p_cap->i2c, REG_SCANCTRL3, &reg_val);
    scanperiod_ms = reg_val & 0xffff;
    len += snprintf(buf + len, PAGE_SIZE - len, "touch scanperiod_ms:%u\r\n", scanperiod_ms);

    return len;
}

/********************************* factory test node end ***********************************/
static DEVICE_ATTR_RO(fct_cap_short_circuit_detect);
static DEVICE_ATTR_RO(fct_cap_offset_cali);
static DEVICE_ATTR_RO(fct_cap_diff_to_air);
static DEVICE_ATTR_RO(fct_cap_diff_approach);

static DEVICE_ATTR_RW(fct_force_offset);
static DEVICE_ATTR_RW(fct_force_noise);
static DEVICE_ATTR_RW(fct_force_afe_noise);
static DEVICE_ATTR_RW(fct_force_signal);
static DEVICE_ATTR_RW(fct_force_cali_coef);
static DEVICE_ATTR_RW(fct_force_mass_deviation);
static DEVICE_ATTR_RW(fct_data_save_flash);
static DEVICE_ATTR_RW(fct_force_factory_coeff_signal);
static DEVICE_ATTR_RW(fct_cap_factory_touch_th);

static DEVICE_ATTR_RO(fct_force_cali_coef_default);
static DEVICE_ATTR_RW(fct_cap_diff_to_air_data);
static DEVICE_ATTR_RW(fct_cap_diff_approach_data);


static DEVICE_ATTR_WO(soft_rst);
static DEVICE_ATTR_WO(aot);
static DEVICE_ATTR_WO(update);
static DEVICE_ATTR_RO(offset);
static DEVICE_ATTR_RO(diff);
static DEVICE_ATTR_RO(chip_info);
static DEVICE_ATTR_RW(mode_operation);
static DEVICE_ATTR_RW(awrw);
static DEVICE_ATTR_RW(reg);
static DEVICE_ATTR_RW(touch_th);
static DEVICE_ATTR_RW(leave_th);
static DEVICE_ATTR_RO(touch_cfg);
static DEVICE_ATTR_RO(touch_state);
static DEVICE_ATTR_RW(diff_cal);
static DEVICE_ATTR_RO(afedata0);
static DEVICE_ATTR_RO(calibration);
static DEVICE_ATTR_RW(touch_scanperiod);
static DEVICE_ATTR_RW(log_timer);

static struct attribute *aw9380x_attributes[] = {
	&dev_attr_fct_force_cali_coef_default.attr,
	&dev_attr_fct_cap_diff_to_air_data.attr,
	&dev_attr_fct_cap_diff_approach_data.attr,

	&dev_attr_fct_force_mass_deviation.attr,
	&dev_attr_fct_force_cali_coef.attr,
	&dev_attr_fct_force_signal.attr,
	&dev_attr_fct_force_noise.attr,
	&dev_attr_fct_force_afe_noise.attr,
	&dev_attr_fct_force_offset.attr,

	&dev_attr_fct_cap_offset_cali.attr,
	&dev_attr_fct_cap_short_circuit_detect.attr,
	&dev_attr_fct_cap_diff_approach.attr,
	&dev_attr_fct_cap_diff_to_air.attr,
	&dev_attr_fct_data_save_flash.attr,
	&dev_attr_fct_force_factory_coeff_signal.attr,
	&dev_attr_fct_cap_factory_touch_th.attr,

	&dev_attr_awrw.attr,
	&dev_attr_reg.attr,
	&dev_attr_soft_rst.attr,
	&dev_attr_aot.attr,
	&dev_attr_update.attr,
	&dev_attr_mode_operation.attr,
	&dev_attr_offset.attr,
	&dev_attr_diff.attr,
	&dev_attr_chip_info.attr,
	&dev_attr_touch_th.attr,
	&dev_attr_leave_th.attr,
	&dev_attr_touch_cfg.attr,
	&dev_attr_touch_state.attr,
	&dev_attr_diff_cal.attr,
	&dev_attr_afedata0.attr,
	&dev_attr_calibration.attr,
	&dev_attr_touch_scanperiod.attr,
	&dev_attr_log_timer.attr,
	NULL,
};

static const struct attribute_group aw9380x_attribute_group = {.attrs = aw9380x_attributes};

static int aw9380x_create_sysclass_group_register(struct aw9380x_cap *p_cap)
{
	int ret = AW_OK;

	if (!p_cap){
		AWLOGE(p_cap->dev,"Error: p_cap is NULL\n");
		return AW_ERR;
	}

    p_cap->sysfs_class = class_create("aw_sensor");
	if(!p_cap->sysfs_class){
		AWLOGE(p_cap->dev,"sysfs_class could not be created\n");
		ret = AW_ERR;
	} else {
		AWLOGI(p_cap->dev,"sysfs_class have be created");
		ret = AW_OK;
	}

	if(ret == AW_OK){
		p_cap->sysfs_dev = device_create(p_cap->sysfs_class, NULL, 0, p_cap, "sensor_dev");
		if(!p_cap->sysfs_dev){
			AWLOGE(p_cap->dev,"sysfs_dev could not be created\n");
			ret = AW_ERR;
			class_destroy(p_cap->sysfs_class);
			p_cap->sysfs_class = NULL;
		} else {
           AWLOGI(p_cap->dev,"sysfs_dev have be created");
		}
	}
	if(ret == AW_OK){
		ret = sysfs_create_group(&p_cap->sysfs_dev->kobj, &aw9380x_attribute_group);
		if(ret != AW_OK) {
			AWLOGE(p_cap->dev,"sysfs group could not be created\n");
			ret = AW_ERR;
			device_destroy(p_cap->sysfs_class, 0);
			p_cap->sysfs_dev = NULL;
			class_destroy(p_cap->sysfs_class);
			p_cap->sysfs_class = NULL;
		}else {
           AWLOGI(p_cap->dev,"sysfs_create have be created");
		}
	}

    return ret;
}

static int32_t aw9380x_create_node(struct aw9380x_cap *p_cap)
{
	int ret = AW_OK;
	AWLOGE(p_cap->dev, "enter");
	//i2c_set_clientdata(p_cap->i2c, p_cap); //tronchen after test remove 
	ret = sysfs_create_group(&p_cap->i2c->dev.kobj, &aw9380x_attribute_group);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev,"creating i2c obj attr err !!!");
		return ret;
	}

	ret = aw9380x_create_sysclass_group_register(p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev,"creating sysclass group register err !!!");
		return ret;
	}

	return ret;
}
#if 0
static void aw9380x_set_pin_out_level(struct aw9380x_cap *p_cap, uint32_t level)
{
	//AWLOGI(p_cap->dev, "enter");
	if (p_cap->pinctrl.pinctrl) {
		if (level == 0)
			pinctrl_select_state(p_cap->pinctrl.pinctrl, p_cap->pinctrl.int_out_low);
		else if (level == 1)
			pinctrl_select_state(p_cap->pinctrl.pinctrl, p_cap->pinctrl.int_out_high);
	} else {
		AWLOGE(p_cap->dev, "failed set pin out :%u, uninit pinctrl", level);
	}
}
#endif

static uint32_t aw9380x_rc_irqscr(struct i2c_client *i2c, uint32_t *val)
{
	int32_t ret = 0;
	ret = aw9380x_i2c_read(i2c, REG_IRQSRC, val);
	AWLOGI(&i2c->dev, "i2c read reg: 0x%04x, val= %d", REG_IRQSRC, *val);
	if (ret != AW_OK) {
		AWLOGE(&i2c->dev, "i2c read error reg: 0x%04x, val= %d", REG_IRQSRC, *val);
	}
	return ret;
}

static int32_t aw9380x_input_init(struct aw9380x_cap *p_cap)
{
	uint32_t i = 0;
	int32_t ret = 0;

	p_cap->channels_arr = devm_kzalloc(p_cap->dev,
			sizeof(struct aw9380x_channels_info) *
			AW9380X_MAX_SLD_NUMS_PLUS_WEAR,
			GFP_KERNEL);
	if (p_cap->channels_arr == NULL) {
		AWLOGE(p_cap->dev, "devm_kzalloc err");
		return -AW_ERR;
	}

	for (i = 0; i < AW9380X_MAX_SLD_NUMS_PLUS_WEAR; i++) {
		if (i == 12) /* wear input */
			snprintf(p_cap->channels_arr[i].name,
					sizeof(p_cap->channels_arr->name),
					"aw9380x_%u_wear",
					p_cap->dts_info.cap_num);
		else
			snprintf(p_cap->channels_arr[i].name,
					sizeof(p_cap->channels_arr->name),
					"aw9380x_%u_ch%u",
					p_cap->dts_info.cap_num, i);

		p_cap->channels_arr[i].last_channel_info = 0;

		if ((p_cap->dts_info.channel_use_flag >> i) & 0x01) {
			p_cap->channels_arr[i].used = AW_TRUE;
			p_cap->channels_arr[i].input = devm_input_allocate_device(p_cap->dev);
			if (p_cap->channels_arr[i].input == NULL)
				return -AW_ERR;
			p_cap->channels_arr[i].input->name = p_cap->channels_arr[i].name;
			__set_bit(EV_SYN, p_cap->channels_arr[i].input->evbit);
			__set_bit(EV_ABS, p_cap->channels_arr[i].input->evbit);
			__set_bit(EV_KEY, p_cap->channels_arr[i].input->evbit);
			/* if i == 12 --> ware  else  --> approch_status */
			__set_bit(ABS_DISTANCE, p_cap->channels_arr[i].input->absbit);
			/* BTN_POS_X_SLD: The key position in the X direction for sliders */
			__set_bit(ABS_X, p_cap->channels_arr[i].input->absbit);
			/* BTN_POS_Y_SLD: The key position in the Y direction for sliders */
			__set_bit(ABS_Y, p_cap->channels_arr[i].input->absbit);
			/* CLICK_NUM_SLD: Click on the number of times */
			__set_bit(ABS_Z, p_cap->channels_arr[i].input->absbit);
			/* TOUCH0ST_SLD: touch0 state */
			__set_bit(ABS_RX, p_cap->channels_arr[i].input->absbit);

			/* TOUCH1ST_SLD: touch1 state */
			__set_bit(ABS_RY, p_cap->channels_arr[i].input->absbit);
			/*CSERR_ST: 0 -> gnd, 1 -> vcc */
			__set_bit(ABS_RZ, p_cap->channels_arr[i].input->absbit);
			__set_bit(PRESS_SCANCODE, p_cap->channels_arr[i].input->keybit);
			input_set_abs_params(p_cap->channels_arr[i].input, ABS_X, 0, 11, 0, 0);
			input_set_abs_params(p_cap->channels_arr[i].input, ABS_Y, 0, 11, 0, 0);
			input_set_abs_params(p_cap->channels_arr[i].input, ABS_Z, 0, 100, 0, 0);
			input_set_abs_params(p_cap->channels_arr[i].input, ABS_RX, 0, 100, 0, 0);
			input_set_abs_params(p_cap->channels_arr[i].input, ABS_RY, 0, 100, 0, 0);
			input_set_abs_params(p_cap->channels_arr[i].input, ABS_RZ, 0, 100, 0, 0);
			input_set_abs_params(p_cap->channels_arr[i].input,
					ABS_DISTANCE, 0, 100, 0, 0);
			ret = input_register_device(p_cap->channels_arr[i].input);
			if (ret) {
				AWLOGE(p_cap->dev, "failed to register input device");
				return -AW_ERR;
			}
		} else {
			p_cap->channels_arr[i].used = AW_FALSE;
			p_cap->channels_arr[i].input = NULL;
		}
	}

	return AW_OK;
}

static void aw9380x_irq_free(struct aw9380x_cap *p_cap)
{
	AWLOGI(p_cap->dev, "irq auto free");
}

static void aw9380x_sldx_handle_button_state(struct aw9380x_cap *p_cap, uint32_t x)
{
	uint32_t reg = 0;
	uint8_t touch0_st = 0, touch1_st = 0;

	aw9380x_i2c_read(p_cap->i2c, REG_BUTTON_STATE_SLD0 + x * AW9380X_SLDX_STEP, &reg);

	AWLOGI(p_cap->dev, "handle_button reg:0x%08X", reg);
	touch0_st = reg & (1 << AW9380X_EVENT_TOUCH0ST_IDX);
	touch1_st = reg & (1 << AW9380X_EVENT_TOUCH1ST_IDX);
	if (touch0_st > p_cap->touch0_state[x]) {
		AWLOGI(p_cap->dev, "SLD%d touch0", x);
		p_cap->event.touch0_state = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_RX, 1);
	} else if (touch0_st < p_cap->touch0_state[x]) {
		AWLOGI(p_cap->dev, "SLD%d untouch0", x);
		p_cap->event.touch0_state = 0;
		input_report_abs(p_cap->channels_arr[x].input, ABS_RX, 0);
	}
	p_cap->touch0_state[x] = touch0_st;

	if (touch1_st > p_cap->touch1_state[x]) {
		AWLOGI(p_cap->dev, "SLD%d touch1", x);
		p_cap->event.touch1_state = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_RY, 1);
	} else if (touch1_st < p_cap->touch1_state[x]) {
		AWLOGI(p_cap->dev, "SLD%d untouch1", x);
		p_cap->event.touch1_state = 0;
		input_report_abs(p_cap->channels_arr[x].input, ABS_RY, 0);
	}
	p_cap->touch1_state[x] = touch1_st;
	input_sync(p_cap->channels_arr[x].input);
}

static void aw9380x_noth_button_key_report(struct aw9380x_cap *p_cap)
{
	uint8_t btn_state = p_cap->noth_btn_state;
	struct timespec64 ts;
	struct rtc_time tm;

	AWLOGI(p_cap->dev, "touch0_state Cap %d Force %d currentBtn %d", p_cap->touch0_state[0], p_cap->touch0_state[1], p_cap->noth_btn_state);
	/*
	* p_cap->touch0_state[0] Cap
	* p_cap->touch0_state[1] Force
	* When cap & force happen, report the down event
	* But when force fall, since user may touch the button, report the up event.
	* User can force the button then report the down event again
	*/
	if (p_cap->touch0_state[0] && p_cap->touch0_state[1])
		btn_state = KEY_EVENT_DOWN;
	else if ((!p_cap->touch0_state[0]) || (!p_cap->touch0_state[1]))
		btn_state = KEY_EVENT_UP;

	if (btn_state != p_cap->noth_btn_state) {
		p_cap->noth_btn_state = btn_state;
		input_report_key(p_cap->channels_arr[0].input, PRESS_SCANCODE, p_cap->noth_btn_state);
		input_sync(p_cap->channels_arr[0].input);
		ktime_get_real_ts64(&ts);
		rtc_time64_to_tm(ts.tv_sec, &tm);
		AWLOGI(p_cap->dev, "KeyCode %d newBtn(0/UP) %d %d-%02d-%02d %02d:%02d:%02d.%09lu UTC",
			PRESS_SCANCODE, btn_state, tm.tm_year + 1900, tm.tm_mon + 1,tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
	}
}

static void aw9380x_sldx_parse(struct aw9380x_cap *p_cap, uint32_t x)
{
	uint32_t btn_event = 0;
	uint32_t prs_event = 0;
	uint32_t sld_event = 0;
	uint32_t btn_pos_x = 0;
	uint32_t btn_pos_y = 0;
	uint32_t reg = 0;

	aw9380x_i2c_read(p_cap->i2c, REG_BAR_REPORT_SLD0 + x * AW9380X_SLDX_STEP, &reg);

	AWLOGI(p_cap->dev, "sld %d, reg = 0x%08X", x, reg);

	prs_event = (reg >> AW9380X_PRESS_STAT_IDX) & AW9380X_PRESS_VALID_DAT;
	btn_event = (reg >> AW9380X_CLICK_STAT_IDX) & AW9380X_CLICK_VALID_DAT;
	sld_event = (reg >> AW9380X_SLIDE_STAT_IDX) & AW9380X_SLIDE_VALID_DAT;
	btn_pos_x = (reg >> AW9380X_BTN_POS_X_IDX) & AW9380X_BTN_POS_MASK;
	btn_pos_y = (reg >> AW9380X_BTN_POS_Y_IDX) & AW9380X_BTN_POS_MASK;

	AWLOGI(p_cap->dev, "prs_event = 0x%x", prs_event);
	AWLOGI(p_cap->dev, "btn_event = 0x%x", btn_event);
	AWLOGI(p_cap->dev, "sld_event = 0x%x", sld_event);

	if (btn_pos_x) {
		AWLOGI(p_cap->dev, "btn_pos_x = ch%d", btn_pos_x - 1);
		input_report_abs(p_cap->channels_arr[x].input, ABS_X,
				AW9380X_MAX_SLD_NUMS_PLUS_WEAR);
		input_report_abs(p_cap->channels_arr[x].input, ABS_X, btn_pos_x - 1);
	}

	if (btn_pos_y) {
		AWLOGI(p_cap->dev, "btn_pos_y = ch%d", btn_pos_y - 1);
		input_report_abs(p_cap->channels_arr[x].input, ABS_Y,
				AW9380X_MAX_SLD_NUMS_PLUS_WEAR);
		input_report_abs(p_cap->channels_arr[x].input, ABS_Y, btn_pos_y - 1);
	}

	if (btn_event) {
		AWLOGI(p_cap->dev, "AW9380X_EVENT_CLICK = %d", btn_event);
		p_cap->event.click = btn_event;
		input_report_abs(p_cap->channels_arr[x].input, ABS_Z,
				AW9380X_MAX_SLD_NUMS_PLUS_WEAR);
		input_report_abs(p_cap->channels_arr[x].input, ABS_Z, btn_event);
	}

	switch (prs_event) {
	case AW9380X_EVENT_PRESS:
		AWLOGI(p_cap->dev, "AW9380X_EVENT_PRESS_SHORT");
		p_cap->event.press = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_DISTANCE, 4);
		break;
	case AW9380X_EVENT_PRESS_LONG:
		AWLOGI(p_cap->dev, "AW9380X_EVENT_PRESS_LONG");
		p_cap->event.long_press = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_DISTANCE, 5);
		break;
	case AW9380X_EVENT_PRESS_SUPER_LONG:
		AWLOGI(p_cap->dev, "AW9380X_EVENT_PRESS_SUPER_LONG");
		p_cap->event.super_long_press = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_DISTANCE, 6);
		break;
	default:
		break;
	}

	switch (sld_event) {
	case AW9380X_EVENT_SLIDE_DIR_UP:
		AWLOGI(p_cap->dev, "AW9380X_EVENT_SLIDE_DIR_UP");
		p_cap->event.up_wareds = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_DISTANCE, 7);
		break;
	case AW9380X_EVENT_SLIDE_DIR_DOWN:
		AWLOGI(p_cap->dev, "AW9380X_EVENT_SLIDE_DIR_DOWN");
		p_cap->event.down_wareds = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_DISTANCE, 8);
		break;
	case AW9380X_EVENT_SLIDE_DIR_LEFT:
		AWLOGI(p_cap->dev, "AW9380X_EVENT_SLIDE_DIR_LEFT");
		p_cap->event.left_wareds = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_DISTANCE, 9);
		break;
	case AW9380X_EVENT_SLIDE_DIR_RIGHT:
		AWLOGI(p_cap->dev, "AW9380X_EVENT_SLIDE_DIR_RIGHT");
		p_cap->event.right_wareds = 1;
		input_report_abs(p_cap->channels_arr[x].input, ABS_DISTANCE, 10);
		break;
	default:
		break;
	}
	input_sync(p_cap->channels_arr[x].input);
}

static void aw9380x_wera_state_check(struct aw9380x_cap *p_cap)
{
	int32_t ret = 0;
	uint32_t reg = 0;

	ret = aw9380x_i2c_read(p_cap->i2c, REG_WEAR_STATE, &reg);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "read REG_WEAR_STATE error(%d)", ret);
		return;
	}
	if (reg & AW9380X_SLIDE_WEAR_STATE_MASK) {
		AWLOGI(p_cap->dev, "AW9380X_EVENT_WEAR");
		input_report_abs(p_cap->channels_arr[12].input, ABS_DISTANCE, 1);
	} else {
		AWLOGI(p_cap->dev, "AW9380X_EVENT_UNWEAR");
		input_report_abs(p_cap->channels_arr[12].input, ABS_DISTANCE, 0);
	}
	input_sync(p_cap->channels_arr[12].input);
}

static void aw9380x_cs_pin_err_handle(struct aw9380x_cap *p_cap)
{
	uint32_t err_up = 0, err_down = 0, i = 0;

	aw9380x_i2c_read(p_cap->i2c, REG_CSERR_UP_ST, &err_up);
	aw9380x_i2c_read(p_cap->i2c, REG_CSERR_DOWN_ST, &err_down);

	for (i = 0; i < AW9380X_CH_NUM_MAX; i++) {
		if (err_up & (1 << i)) {
			input_report_abs(p_cap->channels_arr[i].input, ABS_RZ, 2);
			AWLOGI(p_cap->dev, "cs%d connected to VCC", i);
			input_report_abs(p_cap->channels_arr[i].input, ABS_RZ, 1);
			input_sync(p_cap->channels_arr[i].input);
		}
		if (err_down & (1 << i)) {
			input_report_abs(p_cap->channels_arr[i].input, ABS_RZ, 2);
			AWLOGI(p_cap->dev, "cs%d connected to GND", i);
			input_report_abs(p_cap->channels_arr[i].input, ABS_RZ, 0);
			input_sync(p_cap->channels_arr[i].input);
		}
	}
}

static void aw9380x_approch_status_check(struct aw9380x_cap *p_cap)
{
	uint32_t status0 = 0, status1 = 0;
	int32_t ch;
	uint32_t curr_status = 0;

	aw9380x_i2c_read(p_cap->i2c, REG_STAT0, &status0);
	aw9380x_i2c_read(p_cap->i2c, REG_STAT1, &status1);
	for (ch = 0; ch < AW9380X_CH_NUM_MAX; ch++) {
		if (p_cap->channels_arr[ch].input == NULL ||
				p_cap->channels_arr[ch].used == AW_FALSE) {
			AWLOGI(p_cap->dev, "channel %u not used", ch);
			continue;
		}

		curr_status = ((status0 >> ch) & 0x1) | (((status1 >> ch) & 0x1) << 1);
		if (curr_status == p_cap->channels_arr[ch].last_channel_info) {
			AWLOGI(p_cap->dev, "ch%u state not change", ch);
			continue;
		}
		switch (curr_status) {
		case 0:
			// far
			input_report_abs(p_cap->channels_arr[ch].input, ABS_DISTANCE, 0);
			AWLOGI(p_cap->dev, "ch%u far", ch);
			break;
		case 1:
			// prox0
			AWLOGI(p_cap->dev, "ch%u prox0 near", ch);
			input_report_abs(p_cap->channels_arr[ch].input, ABS_DISTANCE, 1);
			break;
		case 3:
			// prox1 & prox0
			AWLOGI(p_cap->dev, "ch%u prox1 & prox0 near", ch);
			input_report_abs(p_cap->channels_arr[ch].input, ABS_DISTANCE, 2);
			break;
		default:
			AWLOGE(p_cap->dev, "error abs distance");
			break;
		}
		input_sync(p_cap->channels_arr[ch].input);
		p_cap->channels_arr[ch].last_channel_info = curr_status;
	}
}

static void aw9380x_irq_handle_func(uint32_t irq_status, struct aw9380x_cap *p_cap)
{
	int8_t i = 0;
	uint32_t sld = 0;
	uint32_t irq_en_val = 0;
	aw9380x_i2c_read(p_cap->i2c, REG_IRQEN,&irq_en_val);
	AWLOGI(p_cap->dev, "IRQSEN = 0x%08X", irq_en_val);

	AWLOGI(p_cap->dev, "IRQSRC = 0x%08X", irq_status);
	if (irq_status & (1 << REG_IRQSRC_AOTDONEIRQ_BIT))
		p_cap->aot_done_flag = 1;


	if (((irq_status & 0x01) == 1) && (p_cap->driver_code_init_over_flag == 1)) {
		AWLOGI(p_cap->dev, "not healthy!\n");
		p_cap->fault_flag = AW9380X_UNHEALTHY;
	}

	if (irq_status & ((1 << REG_IRQSRC_CLOSEANYIRQ_BIT) |
				(1 << REG_IRQSRC_FARANYIRQ_BIT) |
				(1 << REG_IRQSRC_TOUCHANYIRQ_BIT) |
				(1 << REG_IRQSRC_EXITTOUCHANYIRQ_BIT)))
		aw9380x_approch_status_check(p_cap);

	if (irq_status & (1 << REG_IRQSRC_WEARIRQ_BIT))
		aw9380x_wera_state_check(p_cap);
	if (irq_status & (1 << REG_IRQSRC_ERRIRQ_BIT))
		aw9380x_cs_pin_err_handle(p_cap);

	for (i = REG_IRQSRC_SLD0IRQ_BIT; i <= REG_IRQSRC_SLD11IRQ_BIT; i++) {
		if ((irq_status >> i) & 0x01) {
			sld = i - REG_IRQSRC_SLD0IRQ_BIT;
			aw9380x_sldx_handle_button_state(p_cap, sld);
			aw9380x_sldx_parse(p_cap, sld);
		}
	}

	aw9380x_noth_button_key_report(p_cap);
	aw9380x_i2c_write(p_cap->i2c, REG_CMD, 0x0c);
}
#define AW9380X_TIMEOUT_COMERR_PM 2000
#define AW9380X_WAKELOCK_TIMEOUT 2500
static irqreturn_t aw9380x_cap_default_irq_handle(int32_t irq, void *data)
{
	struct aw9380x_cap *p_cap = (struct aw9380x_cap *)data;
	uint32_t irq_status = 0;
	int ret = 0;
	int retirq = 0;

	retirq = aw9380x_rc_irqscr(p_cap->i2c, &irq_status);
	AWLOGI(p_cap->dev, "gpio_low_count %d", gpio_low_count);
	if ((p_cap->pm_suspended)) {
		__pm_wakeup_event(aw9380x_wakeup_source, jiffies_to_msecs(AW9380X_WAKELOCK_TIMEOUT));
		ret = wait_for_completion_timeout(
					&p_cap->pm_complete,
					msecs_to_jiffies(AW9380X_TIMEOUT_COMERR_PM));
		if (!ret) {
			AWLOGE(p_cap->dev, "Bus don't resume from pm(deep),timeout,skip irq");
		}
	}

	// step1: read clear interrupt
	if (!(retirq == AW_OK)) {
		retirq = aw9380x_rc_irqscr(p_cap->i2c, &irq_status);
		if (!(retirq == AW_OK)) {
			AWLOGE(p_cap->dev,  "i2c read error reg: irq_status= %d", irq_status);
		}
	}

	// step2: Read the status register for status reporting
	aw9380x_irq_handle_func(irq_status, p_cap);

	// step3: Check The chip status
	if (p_cap->fault_flag == AW9380X_UNHEALTHY) {
		AWLOGI(p_cap->dev, "found aw9380x unhealthy");
		p_cap->fault_flag = AW9380X_HEALTHY;
		disable_irq_nosync(p_cap->irq_init.to_irq);
		p_cap->irq_init.host_irq_stat = IRQ_DISABLE;
		schedule_delayed_work(&p_cap->update_work, msecs_to_jiffies(500));
	}

	return IRQ_HANDLED;
}

static int32_t aw9380x_irq_init(struct aw9380x_cap *p_cap)
{
	int32_t ret = 0;
	irq_handler_t thread_fn = p_cap->irq_cfg->thread_fn;

	snprintf(p_cap->irq_init.label, sizeof(p_cap->irq_init.label),
			 "aw9380x_%u_gpio", p_cap->dts_info.cap_num);
	snprintf(p_cap->irq_init.dev_id, sizeof(p_cap->irq_init.dev_id),
			 "aw9380x_%u_irq", p_cap->dts_info.cap_num);
	if (gpio_is_valid(p_cap->dts_info.irq_gpio)) {
		p_cap->irq_init.to_irq = gpio_to_irq(p_cap->dts_info.irq_gpio);
		ret = devm_gpio_request_one(p_cap->dev,
				p_cap->dts_info.irq_gpio,
				p_cap->irq_cfg->flags,
				p_cap->irq_init.label);
		if (ret) {
			AWLOGI(p_cap->dev,
				   "request irq gpio failed, ret = %d", ret);
			ret = -AW_ERR;
		} else {
			if (thread_fn == NULL)
				thread_fn = aw9380x_cap_default_irq_handle;
			ret = devm_request_threaded_irq(p_cap->dev,
					p_cap->irq_init.to_irq,
					p_cap->irq_cfg->handler,
					thread_fn,
					p_cap->irq_cfg->irq_flags,
					p_cap->irq_init.dev_id,
					p_cap);
			if (ret != 0) {
				AWLOGI(p_cap->dev,
					   "failed to request IRQ %d: %d",
					   p_cap->irq_init.to_irq, ret);
				ret = -AW_ERR;
			} else {
				AWLOGI(p_cap->dev,
					   "IRQ request successfully!");
				ret = AW_OK;
			}
		}
	} else {
		AWLOGI(p_cap->dev, "irq gpio invalid!");
		return -AW_ERR;
	}

	AWLOGI(p_cap->dev, "disable_irq");
	p_cap->irq_init.host_irq_stat = IRQ_DISABLE;
	disable_irq(p_cap->irq_init.to_irq);

	AWLOGI(p_cap->dev, "irq init success!");

	return AW_OK;
}

static void aw9380x_node_free(struct aw9380x_cap *p_cap)
{
	sysfs_remove_group(&p_cap->i2c->dev.kobj, &aw9380x_attribute_group);
}

static int32_t aw9380x_init_platform_resources(struct aw9380x_cap *p_cap)
{
	uint32_t ret = -AW_ERR;
#if 0
	// internal pull-up
	if (p_cap->dts_info.use_inter_pull_up == true) {
		ret = aw9380x_pinctrl_init(p_cap);
		if (ret < 0) {
			AWLOGI(p_cap->dev, "error init pin ctrl failed");
			return ret;
		}
		aw9380x_set_pin_out_level(p_cap, 1);
	}
#endif
	ret = aw9380x_create_node(p_cap);
	if (ret != AW_OK) {
		AWLOGI(p_cap->dev, "create node error!");
		//goto err_sysfs_nodes;
		return -AW_ERR;
	}
	init_completion(&p_cap->pm_complete); // before request irq
	aw9380x_wakeup_source = wakeup_source_register(p_cap->dev, "aw9380x");
	if(!aw9380x_wakeup_source) {
		AWLOGE(p_cap->dev, "failed to register wakeup source");
		return -AW_ERR;
	}
	// step 3.Initialization interrupt
	ret = aw9380x_irq_init(p_cap);
	if (ret != AW_OK) {
		AWLOGI(p_cap->dev, "interrupt initialization error!");
		goto err_irq_init;
	}

	// step 4. init input
	ret = aw9380x_input_init(p_cap);
	if (ret != AW_OK) {
		AWLOGI(p_cap->dev, "input_init error!");
		goto err_input_init;
	}

	return AW_OK;

err_input_init:
	aw9380x_irq_free(p_cap);

err_irq_init:
	aw9380x_node_free(p_cap);

	return -AW_ERR;
}

static uint32_t aw9380x_chip_init(struct aw9380x_cap *p_cap)
{
	int32_t ret = 0;
	uint32_t val = 0;

	// 1. check chip id
	ret = aw9380x_check_chipid(p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "check chip id failed");
		return -AW_ERR;
	}

	// 2. sw reset
	aw9380x_soft_reset(p_cap);

	// 3. check init over
	ret = aw9380x_read_init_over_irq(p_cap);
	if (ret != AW_OK) {
		ret = aw9380x_i2c_read(p_cap->i2c, 0xF800, &val);
		if (ret != AW_OK) {
			AWLOGE(p_cap->dev, "read init over irq error");
			return ret;
		}
		AWLOGI(p_cap->dev, "0xF800=0x%04X", val);
		if ((val >> 16) == 0) {
			AWLOGE(p_cap->dev, "check 0xF800 error");
			return ret;
		}
	}
	// 4. update coderam and reg config
	aw9380x_update_delay(p_cap);

	return AW_OK;
}

static int32_t aw9380x_init(struct aw9380x_cap *p_cap)
{
	uint32_t ret = -AW_ERR;

	// 1. parse dts
	ret = aw9380x_parse_dts(p_cap->dev, p_cap->i2c->dev.of_node, &p_cap->dts_info);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "parse dts error!");
		return ret;
	}

	// 2. init other var
	aw9380x_init_other_val(p_cap);

	// 3. init platform resource
	ret = aw9380x_init_platform_resources(p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "init platform resourses failed");
		return ret;
	}

	// 4. init chip
	ret = aw9380x_chip_init(p_cap);
	if (ret != AW_OK) {
		aw9380x_irq_free(p_cap);
		aw9380x_node_free(p_cap);
		mutex_destroy(&aw9380x_lock);
		return -AW_ERR;
	}
	return AW_OK;
}

static int aw9380x_i2c_probe(struct i2c_client *i2c)
{
	int32_t ret = 0;
	struct aw9380x_cap *p_cap = NULL;

	pr_info("aw9380x probe entry");
	// 1. check i2c function
	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		pr_err("check_functionality failed!\n");
		return -EIO;
	}

	// 2. apply memory
	p_cap = devm_kzalloc(&i2c->dev, sizeof(struct aw9380x_cap), GFP_KERNEL);
	if (p_cap == NULL) {
		ret = -AW_ERR;
		return ret;
	}

	// 3. set client data
	p_cap->dev = &i2c->dev;
	p_cap->i2c = i2c;
	i2c_set_clientdata(i2c, p_cap);
	g_p_cap = p_cap;

	// 4. check regulator power
	ret = aw9380x_regulator_power(p_cap);
	if (ret != AW_OK) {
		AWLOGE(p_cap->dev, "regulator_power error!");
		return ret;
	}

	// 5. init driver
	ret = aw9380x_init(p_cap);
	if (ret != AW_OK) {
		aw9380x_power_deinit(p_cap);
		AWLOGE(p_cap->dev, "aw93xx init error!");
		return ret;
	}
	// 6. init timer
	INIT_WORK(&p_cap->data_update_work, aw9380x_data_update_work_func);
	timer_setup(&p_cap->timer, aw9380x_readbase_timer_handler, 0);
	mod_timer(&p_cap->timer, jiffies + msecs_to_jiffies(20 * 1000)); // 20s
	INIT_WORK(&p_cap->irq_state_work, aw9380x_irq_state_work_handler);
	timer_setup(&p_cap->irq_state_timer, aw9380x_irq_state_handler, 0);
	mod_timer(&p_cap->irq_state_timer, jiffies + msecs_to_jiffies(AW9380X_IRQ_STATE_TIME)); // 1000ms

	return 0;
}

static int aw9380x_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct aw9380x_cap *p_cap = i2c_get_clientdata(client);
	int irq_gpio_value = gpio_get_value(p_cap->dts_info.irq_gpio);

	if (enable_irq_wake(p_cap->irq_init.to_irq))
		AWLOGI(dev, "Suspend: enable_irq_wake(%d) faild", p_cap->irq_init.to_irq);

	reinit_completion(&p_cap->pm_complete);
	p_cap->pm_suspended = 1;
	AWLOGI(dev, "Suspend: irq_gpio_value %d awlog_time %d", irq_gpio_value, awlog_time);
	del_timer(&p_cap->irq_state_timer);
	if (awlog_time > 0)
		del_timer(&p_cap->timer);
	if (p_cap->dts_info.use_pm == true)
		aw9380x_mode_set(p_cap, p_cap->pm_info->suspend_set_mode);

	return 0;
}

static int aw9380x_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct aw9380x_cap *p_cap = i2c_get_clientdata(client);
	int irq_gpio_value = gpio_get_value(p_cap->dts_info.irq_gpio);

	if (disable_irq_wake(p_cap->irq_init.to_irq))
		AWLOGI(dev, "Resume: disable_irq_wake(%d) faild", p_cap->irq_init.to_irq);

	complete(&p_cap->pm_complete);
	p_cap->pm_suspended = 0;
	if (awlog_time > 0) {
		mod_timer(&p_cap->timer, jiffies + msecs_to_jiffies(AW9380X_TIME_DEBOUNCE_TIME + awlog_time));
	} else {
		del_timer(&p_cap->timer);
	}
	AWLOGI(dev, "Resume: irq_gpio_value %d awlog_time %d", irq_gpio_value, awlog_time);
	mod_timer(&p_cap->irq_state_timer, jiffies + msecs_to_jiffies(AW9380X_TIME_DEBOUNCE_TIME + AW9380X_IRQ_STATE_TIME));
	if (p_cap->dts_info.use_pm == true)
		aw9380x_mode_set(p_cap, p_cap->pm_info->suspend_set_mode);

	return 0;
}

static void aw9380x_i2c_shutdowm(struct i2c_client *i2c)
{
	struct aw9380x_cap *p_cap = i2c_get_clientdata(i2c);
	aw9380x_mode_set(p_cap, p_cap->pm_info->shutdown_set_mode);
}

static void aw9380x_i2c_remove(struct i2c_client *i2c)
{

	struct aw9380x_cap *p_cap = i2c_get_clientdata(i2c);

	// remove node
	sysfs_remove_group(&(p_cap->i2c->dev.kobj), &aw9380x_attribute_group);

	// deint power
	if (p_cap->dts_info.use_regulator_flag == true)
		aw9380x_power_deinit(p_cap);

	AWLOGI(p_cap->dev, "%s ok!", __func__);

}

const struct of_device_id aw9380x_dt_match[] = {
	{
		.compatible = "awinic,aw9380x_cap",
	},
	{},
};

static const struct i2c_device_id aw9380x_i2c_id[] = {
	{AW9380X_I2C_NAME, 0},
	{},
};

static const struct dev_pm_ops g_aw9380x_pm_ops = {
	.suspend = aw9380x_suspend,
	.resume = aw9380x_resume,
};

MODULE_DEVICE_TABLE(i2c, aw9380x_i2c_id);

static struct i2c_driver aw9380x_i2c_driver = {
	.driver = {
		.name = AW9380X_I2C_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(aw9380x_dt_match),
		.pm = &g_aw9380x_pm_ops,
	},
	.probe = aw9380x_i2c_probe,
	.remove = aw9380x_i2c_remove,
	.shutdown = aw9380x_i2c_shutdowm,
	.id_table = aw9380x_i2c_id,
};

static int32_t __init aw9380x_i2c_init(void)
{
	int32_t ret = 0;

	pr_info("aw9380x driver version %s\n", AW9380X_DRIVER_VERSION);

	ret = i2c_add_driver(&aw9380x_i2c_driver);
	if (ret) {
		pr_err("fail to add aw9380x device into i2c\n");
		return ret;
	}

	return 0;
}

module_init(aw9380x_i2c_init);
static void __exit aw9380x_i2c_exit(void)
{
	i2c_del_driver(&aw9380x_i2c_driver);
}
module_exit(aw9380x_i2c_exit);
MODULE_DESCRIPTION("AWINIC AW9380X Driver");

MODULE_LICENSE("GPL v2");
