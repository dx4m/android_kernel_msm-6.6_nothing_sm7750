load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")
load("//msm-kernel:target_variants.bzl", "get_all_variants")
load("//build/kernel/kleaf:kernel.bzl", "ddk_module")

def define_modules(target, variant):
    kernel_build_variant = "{}_{}".format(target, variant)

    #The below will take care of the defconfig
    include_defconfig = ":{}".format(variant)

    mod_list = []


    ddk_module(
        name = "{}_r8125".format(kernel_build_variant),
        out = "r8125.ko",
        srcs = [
            "drivers/r8125/src/r8125_firmware.c",
            "drivers/r8125/src/r8125_n.c",
            "drivers/r8125/src/r8125_rss.c",
            "drivers/r8125/src/rtl_eeprom.c",
            "drivers/r8125/src/rtltool.c",
			"drivers/r8125/src/r8125_fiber.c",
        ],
        kernel_build = "//msm-kernel:{}".format(kernel_build_variant),
        deps = [
            ":r8125_headers",
            "//msm-kernel:all_headers",
        ],
        copts = [
            "-Wno-error",
            "-DENABLE_USE_FIRMWARE_FILE",
            "-DCONFIG_ASPM",
            "-DENABLE_S5WOL",
            "-DENABLE_EEE",
            "-DENABLE_TX_NO_CLOSE",
            "-DCONFIG_R8125_NAPI",
            "-DCONFIG_R8125_VLAN",
            "-DENABLE_DOUBLE_VLAN",
            "-DENABLE_MULTIPLE_TX_QUEUE",
            "-DENABLE_RSS_SUPPORT",
        ],
    )
    mod_list.append("{}_r8125".format(kernel_build_variant))

    copy_to_dist_dir(
        name = "{}_dataeth_dist".format(kernel_build_variant),
        data = mod_list,
        dist_dir = "out/target/product/{}/dlkm/lib/modules/".format(target),
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
        log = "info",
    )
