#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x635b8619, "module_layout" },
	{ 0xbc6f174a, "usb_deregister" },
	{ 0xac19e229, "usb_register_driver" },
	{ 0xc5b6f236, "queue_work_on" },
	{ 0x2d3385d3, "system_wq" },
	{ 0x722961c8, "led_classdev_register_ext" },
	{ 0xfb384d37, "kasprintf" },
	{ 0x6e718bb7, "dev_driver_string" },
	{ 0x7268c1ed, "_dev_info" },
	{ 0xab00776e, "usb_get_intf" },
	{ 0xbb161a4d, "usb_get_dev" },
	{ 0xa4e0d13e, "kmem_cache_alloc" },
	{ 0x12d76bb0, "kmalloc_caches" },
	{ 0xeddc1a29, "usb_control_msg" },
	{ 0xc5850110, "printk" },
	{ 0x37a0cba, "kfree" },
	{ 0xbc24aea6, "usb_put_dev" },
	{ 0x3c12dfe, "cancel_work_sync" },
	{ 0x67115f95, "led_classdev_unregister" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("usb:v1D50p6018d*dc*dsc*dp*ic*isc*ip*in*");

MODULE_INFO(srcversion, "8C0AF2B4225C64ED027CF80");
