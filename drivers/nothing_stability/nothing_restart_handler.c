#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/panic_notifier.h>
#include <linux/nothing_restart_handler.h>

#define MAX_SZ_DIAG_ERR_MSG 	200
#define SHARED_IMEM_RESTART_INFO_SIZE 0x80

struct nothing_restart_notify {
	struct notifier_block panic_nb;
	struct notifier_block reboot_nb;
};

struct nothing_restart_info {
	char magic[4]; /* 'S' 'S' 0xDE 0xAD */
	char data[SHARED_IMEM_RESTART_INFO_SIZE - 4];
};

static struct nothing_restart_info __iomem *restart_info;
static unsigned rst_msg_size;

void set_restart_msg(const char *msg)
{
	struct nothing_restart_info local_restart_info;

	if (local_restart_info.magic[0] == 'S' && local_restart_info.magic[1] == 'S' ) {
                return;
        }
	/* Restart info magic: SS\xDE\xAD, System DEAD */
	local_restart_info.magic[0] = local_restart_info.magic[1] = 'S';
	local_restart_info.magic[2] = 0xDE;
	local_restart_info.magic[3] = 0xAD;

	if (msg) {
		pr_err("%s: %s \n", __func__, msg);
		strncpy(local_restart_info.data, msg, rst_msg_size - 1);
	}
	else {
		strncpy(local_restart_info.data, "NULL", rst_msg_size - 1);
	}

	memcpy_toio(restart_info, &local_restart_info,
				sizeof(local_restart_info.magic) + rst_msg_size);
	mb();
}
EXPORT_SYMBOL(set_restart_msg);

static int nothing_notifier_panic(struct notifier_block *this, unsigned long event,
			      void *ptr)
{
	char kernel_panic_msg[MAX_SZ_DIAG_ERR_MSG] = "Kernel Panic";

	pr_warn("%s: Get panic notify", __func__);
	if (ptr)
		snprintf(kernel_panic_msg, MAX_SZ_DIAG_ERR_MSG - 1, "%s", (char *)ptr);
	set_restart_msg(kernel_panic_msg);

	return NOTIFY_OK;
}

static int nothing_notifier_reboot(struct notifier_block *this, unsigned long event,
			      void *ptr)
{
	char reboot_msg[MAX_SZ_DIAG_ERR_MSG] = "Reboot";

	pr_warn("%s: Get reboot notify", __func__);
	if (ptr)
		snprintf(reboot_msg, MAX_SZ_DIAG_ERR_MSG - 1, "Reboot: %s", (char *)ptr);
	set_restart_msg(reboot_msg);

	return NOTIFY_OK;
}

static int nothing_restart_info_probe(struct platform_device *pdev)
{
	struct device_node *np;
	unsigned nothing_rst_info_size;
	int ret = 0;
	struct nothing_restart_notify *restart_notify;

	restart_notify = devm_kzalloc(&pdev->dev, sizeof(*restart_notify), GFP_KERNEL);
	if (!restart_notify) {
		pr_err("%s: Unable to allocate memory for restart_notify\n", __func__);
		return 0;
	}

	np = of_find_compatible_node(NULL, NULL,
				"nothing,msm-imem-restart_info");
	if (!np) {
		pr_err("unable to find imem nothing,imem-restart-info\n");
		return 0;
	} else {
		restart_info = of_iomap(np, 0);
		if (!restart_info) {
			pr_err("%s: nothing restart_info not valid\n", __func__);
			return 0;
		} else {
			ret = of_property_read_u32(np, "info_size", &nothing_rst_info_size);
			if (ret != 0) {
				pr_err("%s: Failed to find info_size property in nothing restart info device node %d\n",
						__func__, ret);
				return 0;
			}
		}
	}

	if (nothing_rst_info_size < offsetof(struct nothing_restart_info, data))
	{
		pr_err("%s: invalid nothing_rst_info_size %d\n", __func__, nothing_rst_info_size);
		return 0;
	}
	rst_msg_size = nothing_rst_info_size - offsetof(struct nothing_restart_info, data);

	memset_io(restart_info, 0x00, nothing_rst_info_size);
	wmb();
	set_restart_msg("Unknown");

	restart_notify->panic_nb.notifier_call = nothing_notifier_panic;
	restart_notify->panic_nb.priority = INT_MAX;
	atomic_notifier_chain_register(&panic_notifier_list,
				       &restart_notify->panic_nb);

	restart_notify->reboot_nb.notifier_call = nothing_notifier_reboot;
	restart_notify->reboot_nb.priority = 255;
	register_reboot_notifier(&restart_notify->reboot_nb);

	platform_set_drvdata(pdev, restart_notify);
	pr_info("%s OK\n", __func__);
	return 0;
}

static int nothing_restart_info_remove(struct platform_device *pdev)
{
	struct nothing_restart_notify *restart_notify = platform_get_drvdata(pdev);

	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &restart_notify->panic_nb);
	unregister_reboot_notifier(&restart_notify->reboot_nb);

	if (restart_info)
		iounmap(restart_info);

	return 0;
}

static const struct of_device_id of_nothing_restart_info_match[] = {
	{ .compatible = "qcom,msm-imem", },
	{},
};
MODULE_DEVICE_TABLE(of, of_nothing_restart_info_match);

static struct platform_driver nothing_restart_info_driver = {
	.probe = nothing_restart_info_probe,
	.remove = nothing_restart_info_remove,
	.driver = {
		.name = "nothing-restart-info",
		.of_match_table = of_nothing_restart_info_match,
	},
};

static int __init nothing_restart_info_driver_init(void)
{
	pr_warn("nothing_restart_info_driver_init\n");

	return platform_driver_register(&nothing_restart_info_driver);
}

module_init(nothing_restart_info_driver_init);

static void __exit nothing_restart_info_driver_exit(void)
{
	return platform_driver_unregister(&nothing_restart_info_driver);
}
module_exit(nothing_restart_info_driver_exit);

MODULE_DESCRIPTION("Nothing Restart Info Driver");
MODULE_LICENSE("GPL v2");
