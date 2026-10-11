#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

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



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xb91cb16b, "misc_register" },
	{ 0xeca27f0a, "misc_deregister" },
	{ 0x9c4ed43a, "alt_cb_patch_nops" },
	{ 0xaa47b76e, "__arch_copy_from_user" },
	{ 0xaa47b76e, "__arch_copy_to_user" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xe54e0a6b, "__fortify_panic" },
	{ 0x4859fb5a, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xb91cb16b,
	0xeca27f0a,
	0x9c4ed43a,
	0xaa47b76e,
	0xaa47b76e,
	0xd272d446,
	0xe54e0a6b,
	0x4859fb5a,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"misc_register\0"
	"misc_deregister\0"
	"alt_cb_patch_nops\0"
	"__arch_copy_from_user\0"
	"__arch_copy_to_user\0"
	"__stack_chk_fail\0"
	"__fortify_panic\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "1FE3174622352F67F9959E5");
