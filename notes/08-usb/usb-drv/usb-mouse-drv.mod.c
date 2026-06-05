#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

__visible struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0x8d42f378, __VMLINUX_SYMBOL_STR(module_layout) },
	{ 0xf3535356, __VMLINUX_SYMBOL_STR(usb_deregister) },
	{ 0xcc123635, __VMLINUX_SYMBOL_STR(usb_register_driver) },
	{ 0x50e28294, __VMLINUX_SYMBOL_STR(input_register_device) },
	{ 0xb1381620, __VMLINUX_SYMBOL_STR(usb_alloc_coherent) },
	{ 0xe63372a8, __VMLINUX_SYMBOL_STR(input_allocate_device) },
	{ 0x1bec770f, __VMLINUX_SYMBOL_STR(kmem_cache_alloc_trace) },
	{ 0x5363d73f, __VMLINUX_SYMBOL_STR(kmalloc_caches) },
	{ 0x811b510d, __VMLINUX_SYMBOL_STR(input_event) },
	{ 0x7719503a, __VMLINUX_SYMBOL_STR(usb_submit_urb) },
	{ 0x9998ef80, __VMLINUX_SYMBOL_STR(usb_alloc_urb) },
	{ 0x27e1a049, __VMLINUX_SYMBOL_STR(printk) },
	{ 0x37a0cba, __VMLINUX_SYMBOL_STR(kfree) },
	{ 0x86bec7a4, __VMLINUX_SYMBOL_STR(input_unregister_device) },
	{ 0x79c39b42, __VMLINUX_SYMBOL_STR(usb_put_dev) },
	{ 0xc711944d, __VMLINUX_SYMBOL_STR(usb_free_coherent) },
	{ 0x2e5810c6, __VMLINUX_SYMBOL_STR(__aeabi_unwind_cpp_pr1) },
	{ 0x3d38753, __VMLINUX_SYMBOL_STR(usb_free_urb) },
	{ 0xc5f0b4fa, __VMLINUX_SYMBOL_STR(usb_kill_urb) },
	{ 0xb1ad28e0, __VMLINUX_SYMBOL_STR(__gnu_mcount_nc) },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";

