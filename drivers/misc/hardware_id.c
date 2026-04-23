#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/pm.h>

#include <linux/err.h>
#include <linux/input.h>
#include <linux/jiffies.h>

#include <linux/of_gpio.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>

#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define HWID_MAX_ADC_LEVEL  6
#define HWID_MAX_ID1_LEVEL  2
#define ADC_RANGE(mid, offset) \
			{.floor = mid - offset, .ceil = mid + offset}

enum hwid_idx {
	ID_0 = 0,
	ID_1,
	ID_MAX
};

struct hwid_data {
	int adc_mv[ID_MAX];
	int level[ID_MAX];
	struct device *dev;
	struct iio_channel *channel[ID_MAX];
};

struct hwid_adc_range {
	int floor;
	int ceil;
};

static struct hwid_adc_range adc_lutable[HWID_MAX_ADC_LEVEL] = {
	ADC_RANGE(  60, 40),                  /* 3.24KOhm */
	ADC_RANGE( 202, 60),                  /*   12KOhm */
	ADC_RANGE( 465, 80),                  /*   33KOhm */
	ADC_RANGE( 982, 80),                  /*  110KOhm */
	ADC_RANGE(1466, 80),                  /*  360KOhm */
	ADC_RANGE(1869, 80),                  /*       NC */
};

static char *hw_ver[HWID_MAX_ID1_LEVEL][HWID_MAX_ADC_LEVEL] = {
	{"EVB", "T0", "T0_Felica", "EVT", "EVT_Felica",  "EVT_Power"},
	{"EVT_CS", "EVT_Felica_CS", "DVT",  "DVT_Felica", "PVT", "PVT_Felica"},
};

struct hwid_data *hwid_data = NULL;
#ifdef _HWID_TEST_STUB_
static int g_hwid_stub = 0;
#endif

static int hwid_show(struct seq_file *m, void *v)
{
	int id_0 = hwid_data->level[0];
	int id_1 = hwid_data->level[1];
	bool valid_id = (id_0 >=0 && id_0 < HWID_MAX_ADC_LEVEL) \
					&& (id_1 >=0 && id_1 < HWID_MAX_ID1_LEVEL);

#ifdef _HWID_TEST_STUB_
	if (g_hwid_stub != 0 && g_hwid_stub < (HWID_MAX_ADC_LEVEL * HWID_MAX_ID1_LEVEL)) {
		id_0 = g_hwid_stub % HWID_MAX_ADC_LEVEL;
		id_1 = g_hwid_stub / HWID_MAX_ADC_LEVEL;
	}
	if (g_hwid_stub == (HWID_MAX_ADC_LEVEL * HWID_MAX_ID1_LEVEL)) g_hwid_stub = 0;
	g_hwid_stub++;
#endif

	seq_printf(m, "version = %-13s  gpio3_id1_adc=%dmV  gpio4_id0_adc=%dmV\n",
				valid_id ? hw_ver[id_1][id_0] : "Unknown",
				hwid_data->adc_mv[1],
				hwid_data->adc_mv[0]);

	return 0;
}

static int hwid_open(struct inode *inode, struct file *file)
{
	return single_open(file, hwid_show, inode->i_private);
}

static const struct proc_ops hwid_proc_ops = {
	.proc_open		= hwid_open,
	.proc_read		= seq_read,
};

static int hwid_create_proc_file(void)
{
	struct proc_dir_entry *dir;

	dir = proc_create("hwid", 0440, NULL, &hwid_proc_ops);
	if (!dir) {
		pr_err("Unable to create /proc/hwid");
		return -1;
	}

	return 0;
}

static void hwid_get_id_value(struct hwid_data *data)
{
	int i, j;
	for (i = 0; i < ID_MAX; ++i) {
		for (j = 0; j < HWID_MAX_ADC_LEVEL; ++j) {
			if (data->adc_mv[i] >= adc_lutable[j].floor
				&& data->adc_mv[i] <= adc_lutable[j].ceil)
				break;
		}
		data->level[i] = j;
	}
}

static void hwid_read_id(struct hwid_data *data)
{
	int i, ret = 0;
	int adc_uv = 0;

	for (i = 0; i < ID_MAX; ++i) {
		ret = iio_read_channel_processed(data->channel[i], &adc_uv);
		if (unlikely(ret < 0)) {
			dev_err(data->dev, "%s(): iio read channel[ID_%d] failed, return %d\n",
				__func__, i, ret);
			data->adc_mv[i] = -1;
			continue;
		}
		data->adc_mv[i] = adc_uv / 1000;
		dev_info(data->dev, "%s(): id_%d adv val %dmv\n",
					__func__, i, data->adc_mv[i]);
	}

	if (data->adc_mv[0] != -1 && data->adc_mv[1] != -1)
		hwid_get_id_value(data);
	else
		data->level[0] = data->level[1] = -1;
}

static int hwid_probe(struct platform_device *pdev)
{
	int i, ret;
	enum iio_chan_type type;
	struct device *dev = &pdev->dev;
	struct hwid_data *data = NULL;
	/* io-channel-names in device tree */
	const char *io_chan_names[ID_MAX] = {"id_0", "id_1"};

	data = devm_kzalloc(dev, sizeof(struct hwid_data), GFP_KERNEL);
	if (unlikely(!data)) {
		dev_err(dev, "%s(): memory insufficient\n", __func__);
		return -ENOMEM;
	}

	data->dev = dev;
	for (i = 0; i < ID_MAX; ++i) {
		data->channel[i] = devm_iio_channel_get(dev, io_chan_names[i]);
		if (unlikely(IS_ERR(data->channel[i]))) {
			dev_err(dev, "%s(): get iio channel %s failed, return %ld\n",
					__func__, io_chan_names[i], PTR_ERR(data->channel[i]));
			return PTR_ERR(data->channel[i]);
		}

		ret = iio_get_channel_type(data->channel[i], &type);
		if (unlikely(type != IIO_VOLTAGE)) {
			dev_warn(dev, "%s(): %s channel is not VOLTAGE channel, but %d, retval %d\n",
					__func__, io_chan_names[i], type, ret);
		}
	}

	hwid_read_id(data);

	/* create proc for hardware id */
	hwid_create_proc_file();

	hwid_data = data;
	dev_info(dev, "%s() OK\n", __func__);
	return 0;
}

static const struct of_device_id hwid_of_match[] = {
	{ .compatible = "noth,hwid", },
	{ },
};

MODULE_DEVICE_TABLE(of, hwid_of_match);
static struct platform_driver hwid_driver = {
	.probe		= hwid_probe,
	.driver		= {
		.name	= "noth-hwid",
		.of_match_table = hwid_of_match,
	}
};

static int __init hwid_init(void)
{
	return platform_driver_register(&hwid_driver);
}

static void __exit hwid_exit(void)
{
	remove_proc_entry("hwid", NULL);
	platform_driver_unregister(&hwid_driver);
}

module_exit(hwid_exit);
module_init(hwid_init);
MODULE_DESCRIPTION("Noth Hardware ID Driver");
MODULE_LICENSE("GPL v2");
