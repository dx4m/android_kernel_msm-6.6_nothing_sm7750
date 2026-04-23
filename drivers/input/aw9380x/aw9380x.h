/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AW9380X_H__
#define __AW9380X_H__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/kern_levels.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>
#include <linux/decompress/mm.h>

enum aw9380x_cs_2_irq {
	AW9380X_CS2_IRQ = 2,
	AW9380X_CS5_IRQ = 5,
};


#define AW_CS_ERR_DET				(0)
#define AW9380X_CSX_TO_IRQ			(AW9380X_CS2_IRQ)
#define AW9380X_VREF				(2800) /* mv */
#define AW_RAW_DATA_NUM				50
#define AW_RAWCAL_DATA_DELAY			(20000)	/* us */
#define AW_SIGNAL_WEIGHT			200000000   /* ug */
#define AW_VERI_WEIGHT				300000000   /* ug */
#define AW9380X_NOISE_DATA_NUMS			(201)
#define AW_NOISE_DELAY				(20000)	/* us */

#define AW_DIFF_TO_ARI_DATA_NUMS		(200)
#define AW_DIFF_TO_ARI_DELAY			(20000)  /* us */
#define AW_DIFF_TO_APPROACH_DATA_NUMS		(200)
#define AW_DIFF_TO_APPROACH_DELAY		(20000)  /* us */

#define AW_SHORT_DETECT_DELAY			(300)   /* ms */
/* It is determined by the specific number of channels of the chip. */
/* eg: 12/7/5/3 */
#define AW9380X_CH_NUM_MAX			(12)
#define AW9380X_CH01_NUM			(2)
#ifndef AW_TRUE
#define AW_TRUE					(1)
#endif

#ifndef AW_FALSE
#define AW_FALSE				(0)
#endif

#ifndef OFFSET_BIT_0
#define OFFSET_BIT_0				(0)
#endif

#ifndef OFFSET_BIT_8
#define OFFSET_BIT_8				(8)
#endif

#ifndef OFFSET_BIT_16
#define OFFSET_BIT_16				(16)
#endif

#ifndef OFFSET_BIT_24
#define OFFSET_BIT_24				(24)
#endif

#ifndef WORD_LEN
#define WORD_LEN				(4)
#endif

#ifndef GET_BITS_7_0
#define GET_BITS_7_0				(0x00FF)
#endif

#ifndef GET_BITS_15_8
#define GET_BITS_15_8				(0xFF00)
#endif

#ifndef GET_BITS_24_16
#define GET_BITS_24_16				(0x00FF0000)
#endif

#ifndef GET_BITS_31_25
#define GET_BITS_31_25				(0xFF000000)
#endif

#define HALF_WORD				(0xf)
#define ONE_WORD				(0xff)

#define AWLOGD(dev, format, arg...) \
		 dev_dbg(dev, \
			"[%s:%d] "format"\n", __func__, __LINE__, ##arg)

#define AWLOGI(dev, format, arg...) \
		dev_info(dev, \
			"[%s:%d] "format"\n", __func__, __LINE__, ##arg)

#define AWLOGE(dev, format, arg...) \
		 dev_err(dev, \
			"[%s:%d] "format"\n", __func__, __LINE__, ##arg)

#define AW9380X_VCC_MIN_UV						(3000000)
#define AW9380X_VCC_MAX_UV						(3000000)
#define AW9380X_I2C_TRANS_ONE_PACK_SIZE			(1024)
#define AW9380X_SRAM_UPDATE_ONE_PACK_SIZE		(1024)
//#define AW9380X_SRAM_UPDATE_ONE_PACK_SIZE		(32)
#define AW9380X_SRAM_UPDATE_ONE_UINT_SIZE		(4)
#define AW9380X_SRAM_START_ADDR					(0x2000)
#define AW9380X_SRAM_END_ADDR					(0x4ff4 + 4)
#define AW9380X_SRAM_SIZE						(AW9380X_SRAM_END_ADDR - AW9380X_SRAM_START_ADDR)
#define AW9380X_SRAM_DEFAULT_VAL				(0xffffffff)

/* No modify */
#define AW9380X_MAX_SLD_NUMS_PLUS_WEAR		(13)

#define AW9380X_I2C_RETRIES			(25)
#define AW9380X_REG_DATA_LEN		(4)
#define AW9380X_REG_ADDR_LEN		(2)
#define AW9380X_CPU_OSC_CTRL_MASK	(1)
#define AW9380X_OFFSET_LEN			(15)

#define AW9380X_DATA_OffSET_2		(2)
#define AW9380X_DATA_OffSET_3		(3)
#define AW9380X_AWRW_OffSET			(20)
#define AW9380X_AWRW_DATA_WIDTH		(5)
#define AW9380X_RST_DELAY_MS		(30)

#define AW9380X_DATA_PROCESS_FACTOR						(1024)
#define AW9380X_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ			(9900)
#define AW9380X_STEP_LEN_UNSIGNED_CAP_FINE_ADJ			(152)
#define AW9380X_STEP_LEN_UNSIGNED_CAP_ENLARGE			(10000)
#define AW9380X_SRAM_UPDATE_ONE_UINT_SIZE				(4)
#define AW9380X_POWER_ON_SYSFS_DELAY_MS					(5000)

#define AW93803_NAME	("aw93803")
#define AW93804_NAME	("aw93804")
#define AW93805_NAME	("AW93805")

#define AW9380X_DIFF_PROX_MIN					(0)
#define AW9380X_DIFF_PROX_MAX					(1000000)

#define AW9380X_GPIO_DIRIN_EN				(0xFFF)
#define AW9380X_GPIO_PU_EN					(0xFFF)
#define AW9380X_GPIO_PU_DIS					(0)
#define AW9380X_GPIO_PD_EN					(0xFFF)
#define AW9380X_GPIO_PD_DIS					(0)

#define PRESS_SCANCODE						(251)

enum AW9380X_KEY_EVENT {
	KEY_EVENT_UP = 0,
	KEY_EVENT_DOWN = 1,
	KEY_EVENT_UNKNOWN = 2,
};

enum AW9380X_IRQSRC {
	REG_IRQSRC_INITOVERIRQ_BIT,
	REG_IRQSRC_CLOSEANYIRQ_BIT,
	REG_IRQSRC_FARANYIRQ_BIT,
	REG_IRQSRC_AOTDONEIRQ_BIT,
	REG_IRQSRC_ERRIRQ_BIT,
	REG_IRQSRC_SLD0IRQ_BIT,
	REG_IRQSRC_SLD1IRQ_BIT,
	REG_IRQSRC_SLD2IRQ_BIT,
	REG_IRQSRC_SLD3IRQ_BIT,
	REG_IRQSRC_SLD4IRQ_BIT,
	REG_IRQSRC_SLD5IRQ_BIT,
	REG_IRQSRC_SLD6IRQ_BIT,
	REG_IRQSRC_SLD7IRQ_BIT,
	REG_IRQSRC_SLD8IRQ_BIT,
	REG_IRQSRC_SLD9IRQ_BIT,
	REG_IRQSRC_SLD10IRQ_BIT,
	REG_IRQSRC_SLD11IRQ_BIT,
	REG_IRQSRC_WEARIRQ_BIT,
	REG_IRQSRC_TOUCHANYIRQ_BIT,
	REG_IRQSRC_EXITTOUCHANYIRQ_BIT,
};


#define AW_CAP_FLIP_U16(value) ((((value) & (0x00FF)) << (8)) | (((value) & (0xFF00)) >> (8)))
#define AW_CAP_FLIP_U32(value) ((((value) & (0x000000FF)) << (24)) | ((((value) & (0x0000FF00))) << (8)) | (((value) & (0x00FF0000)) >> (8)) | (((value) & (0xFF000000)) >> (24)))

enum aw9380x_state {
	AW_OK,
	AW_ERR,
	AW_PARA_ERR,
	AW_VERS_ERR,
	AW_ERR_CHIPID,
	AW_ERR_IRQ_INIT_OVER,
	AW_PROT_UPDATE_ERR,
	AW_REG_LOAD_ERR,
	AW_INVALID_PARA,
	AW_BIN_PARA_INVALID,
	AW_FW_CHECK_ERR,
	AW_BT_CHECK_ERR,
	AW_OTEHR_CHECK_ERR,
	AW_BIN_NAME_CHECK_ERR,
};

enum aw9380x_host_irq_state {
	IRQ_ENABLE,
	IRQ_DISABLE,
};

enum aw9380x_health_check {
	AW9380X_HEALTHY = 0,
	AW9380X_UNHEALTHY = 1,
};

enum aw9380x_i2c_flag {
	AW9380X_I2C_WR,
	AW9380X_I2C_RD,
	AW9380X_PACKAGE_RD,
};

enum aw9380x_chip_id {
	AW93803_CHIP_ID = 0xA9330710,
	AW93804_CHIP_ID = 0xA9331010,
	AW93805_CHIP_ID = 0xA9331210,
};

// aw9380x operation mode
enum aw9380x_op_mode {
	AW9380X_ACTIVE_MODE = 0x01,
	AW9380X_SLEEP_MODE = 0x02,
	AW9380X_DEEPSLEEP_MODE = 0x03,
};

enum aw9380x_cap_mode {
	AW9380X_UNSIGNED_CAP = 0,
	AW9380X_SIGNED_CAP = 4,
	AW9380X_MUTUAL_CAP = 5,
};

enum aw9380x_bit {
	AW_BIT0,
	AW_BIT1,
	AW_BIT2,
	AW_BIT3,
	AW_BIT7 = 7,
	AW_BIT8 = 8,
	AW_BIT16 = 16,
	AW_BIT24 = 24,
	AW_BIT28 = 28,
	AW_BIT32 = 32,
};

struct aw9380x_dts_info {
	uint32_t cap_num;
	int32_t irq_gpio;
	uint32_t channel_use_flag;
	bool use_regulator_flag;
	bool use_inter_pull_up;
	bool use_pm;
};

struct aw9380x_pm_info {
	enum aw9380x_op_mode suspend_set_mode;
	enum aw9380x_op_mode resume_set_mode;
	enum aw9380x_op_mode shutdown_set_mode;
};

struct aw9380x_reg_data {
	unsigned char rw;
	unsigned short reg;
};

struct aw9380x_diff {
	uint16_t diff0_reg;
	uint16_t diff_step;
	//Data format:S21.10, Floating point types generally need to be removed
	uint32_t rm_float;
};

struct aw9380x_chip_mode_info {
	uint32_t init_mode;		// chip init over set mode
	uint32_t active;		// chip active mode
	uint32_t pre_init_mode; // chip mode when power on
};

struct aw9380x_bin_info {
	const uint8_t *bin_name;
	uint32_t bin_data_ver;
};


struct aw9380x_reg_list {
	uint8_t reg_none_access;
	uint8_t reg_rd_access;
	uint8_t reg_wd_access;
	const struct aw9380x_reg_data *reg_perm;
	uint32_t reg_num;
};

struct aw9380x_pinctrl {
	struct pinctrl *pinctrl;
	struct pinctrl_state *default_sta;
	struct pinctrl_state *int_out_high;
	struct pinctrl_state *int_out_low;
};

struct aw9380x_awrw_info {
	uint8_t rw_flag;
	uint8_t addr_len;
	uint8_t data_len;
	uint8_t reg_num;
	uint32_t i2c_tranfar_data_len;
	uint8_t *p_i2c_tranfar_data;
};

struct aw9380x_irq_init {
	int32_t to_irq;
	uint8_t host_irq_stat;
	void *data;
	uint8_t label[30];
	uint8_t dev_id[30];
};

struct aw9380x_para_info {
	const uint32_t *reg_arr;
	uint32_t reg_arr_len;
};

struct aw9380x_channels_info {
	uint16_t used;
	uint32_t last_channel_info;
	struct input_dev *input;
	uint8_t name[20];
};



struct aw9380x_irq_cfg {
	unsigned long flags;
	unsigned long irq_flags;
	irq_handler_t handler;
	irq_handler_t thread_fn;

};

#define AW9380X_SLDX_STEP					(0x5C)

#define AW9380X_PRESS_STAT_IDX					(0)
#define AW9380X_PRESS_VALID_DAT					(0x0f)
#define AW9380X_CLICK_STAT_IDX					(4)
#define AW9380X_CLICK_VALID_DAT					(0x3f)
#define AW9380X_SLIDE_STAT_IDX					(10)
#define AW9380X_SLIDE_VALID_DAT					(0x3f)
#define AW9380X_BTN_POS_X_IDX					(16)
#define AW9380X_BTN_POS_Y_IDX					(24)
#define AW9380X_BTN_POS_MASK					(0xFF)

#define AW9380X_SLIDE_WEAR_STATE_MASK				(0x3)

#define AW9380X_EVENT_SLIDE_DIR_UP				(1)
#define AW9380X_EVENT_SLIDE_DIR_DOWN				(2)
#define AW9380X_EVENT_SLIDE_DIR_LEFT				(4)
#define AW9380X_EVENT_SLIDE_DIR_RIGHT				(8)

#define AW9380X_EVENT_PRESS					(1)
#define AW9380X_EVENT_PRESS_LONG				(3)
#define AW9380X_EVENT_PRESS_SUPER_LONG				(7)

#define AW9380X_EVENT_TOUCH0ST_IDX				(0)
#define AW9380X_EVENT_TOUCH1ST_IDX				(1)

#define AW9380X_ADC_CHANNEL_MAX					12

enum aw9380x_cs_status_cmd {
	AW_CS_OK,
	AW_CS_TO_GND,
	AW_CS_TO_VDD,
};

enum aw9380x_cmd {
	AW_FCT_CAP_SHORT_CIRCUIT_DETECT,
	AW_FCT_CAP_OFFSET_CALI,
	AW_FCT_CAP_DIFF_TO_AIR_NOISE,
	AW_FCT_CAP_DIFF_TO_APPROACH,

	AW_FCT_FORCE_OFFSET,
	AW_FCT_FORCE_NOISE,
	AW_FCT_FORCE_AFE_NOISE,
	AW_FCT_FORCE_SIGNAL_1,
	AW_FCT_FORCE_SIGNAL_2,
	AW_FCT_FORCE_VERI_COEF_1,
	AW_FCT_FORCE_VERI_COEF_2,
	AW_FCT_FORCE_CALI_COEF_SAVE,
};

struct aw9380x_fct_data {
	int64_t force_offset_vol;		/* uV */
	int force_noise_pp;		/* code */
	int64_t force_noise_pp_vol;		/* nV */
	int force_noise_peak;		/* code */
	int64_t force_noise_peak_vol;	/* nV */
	int force_noise_std;		/* code */
	int64_t force_noise_std_vol;	/* nV */

	int force_afe_noise_pp;		/* code */
	int64_t force_afe_noise_pp_vol;		/* nV */
	int force_afe_noise_peak;		/* code */
	int64_t force_afe_noise_peak_vol;	/* nV */
	int force_afe_noise_std;		/* code */
	int64_t force_afe_noise_std_vol;	/* nV */

	int force_signal_raw_data1;	/* code */
	int force_signal_raw_data2;	/* code */
	int64_t force_signal_vol;		/* nV */
	int force_signal_code;		/* code */
	int force_cali_coef;		/* ug/code */
	int force_veri_raw_data1;	/* code */
	int force_veri_raw_data2;	/* code */
	int force_veri_signal_code;	/* code */
	int force_mass_deviation;	/* 0.01% */
	long long force_mass_weight;	/* ug */

	int cap_cs_status[AW9380X_CH_NUM_MAX];
	int cap_offset[AW9380X_CH_NUM_MAX];
	int cap_diff_to_air_noise_pp[AW9380X_CH_NUM_MAX];
	int cap_diff_avg[AW9380X_CH_NUM_MAX];

	int force_cali_coef_def[AW9380X_CH_NUM_MAX];	/* ug/code */
	int force_cali_coef_now[AW9380X_CH_NUM_MAX];	/* ug/code */
	int force_test_weight_now[AW9380X_CH_NUM_MAX];	/* ug */
	int force_signal_code_now[AW9380X_CH_NUM_MAX];	/* code */
	int force_noise_rawcal_data[AW9380X_NOISE_DATA_NUMS];
	int cap_diff_to_air_data[AW9380X_CH_NUM_MAX][AW_DIFF_TO_ARI_DATA_NUMS];
	int cap_diff_to_air_data_chx;
	int cap_diff_approach_data[AW9380X_CH_NUM_MAX][AW_DIFF_TO_APPROACH_DATA_NUMS];
	int cap_diff_approach_data_chx;
	int force_noise_afe_data[AW9380X_NOISE_DATA_NUMS];

};

struct aw9380x_factory_force_data {
	int force_cali_coef;		/* ug/code */
	uint32_t force_cali_coef_signal_val;  //code
	uint32_t force_cali_coef_reg_val;
};

struct aw9380x_factory_cap_data {
	uint32_t cap_touch_th_x;	/* code */
	uint32_t cap_touch_th_y;	/* code */
};

struct aw9380x_event {
	uint8_t click;
	uint8_t press;
	uint8_t long_press;
	uint8_t super_long_press;
	uint8_t wear;
	uint8_t right_wareds;
	uint8_t left_wareds;
	uint8_t up_wareds;
	uint8_t down_wareds;
	uint8_t in_ear;
	uint8_t btn_pos_y;
	uint8_t btn_pos_x;
	uint8_t touch0_state;
	uint8_t touch1_state;
};

struct aw9380x_cap {
	uint32_t chip_id;
	uint8_t chip_name[20];
	uint8_t reg_name[50];
	uint32_t fw_bin_version;
	uint8_t fault_flag;
	uint8_t aot_done_flag;
	bool driver_code_init_over_flag;
	bool reg_bin_load_flag;
	bool code_ram_bin_load_flag;
	bool power_enable;
	enum aw9380x_op_mode last_mode;

	struct class *sysfs_class;
	struct device *sysfs_dev;

	struct i2c_client *i2c;
	struct device *dev;
	struct regulator *vcc;
	struct aw9380x_dts_info dts_info;
	struct aw9380x_irq_init irq_init;
	struct aw9380x_pinctrl pinctrl;
	const struct aw9380x_pm_info *pm_info;
	const struct aw9380x_chip_mode_info *chip_mode_info;
	const struct aw9380x_reg_list *p_reg_list;
	const struct aw9380x_diff *p_diff;
	struct aw9380x_bin_info *code_ram_info;
	struct aw9380x_bin_info *reg_bin_info;
	struct aw9380x_irq_cfg *irq_cfg;
	// struct aw9380x_irq_init irq_init;
	struct aw9380x_channels_info *channels_arr;
	// struct aw_channels_info slider;
	const struct aw9380x_para_info *para_info;
	struct delayed_work update_work;
	struct aw9380x_awrw_info awrw_info;

	struct aw9380x_event event;
	uint8_t touch0_state[AW9380X_CH_NUM_MAX];
	uint8_t touch1_state[AW9380X_CH_NUM_MAX];
	struct aw9380x_fct_data fct_data;
	struct aw9380x_factory_force_data factory_force_data[AW9380X_CH_NUM_MAX];
	struct aw9380x_factory_cap_data factory_cap_data[AW9380X_CH_NUM_MAX];
	uint8_t noth_btn_state;
	bool pm_suspended;
	struct completion pm_complete;
	struct timer_list timer;
	struct work_struct data_update_work;
	struct timer_list irq_state_timer;
	struct work_struct irq_state_work;
};

#endif
