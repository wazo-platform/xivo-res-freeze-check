#include <asterisk.h>
#include <asterisk/module.h>
#include <asterisk/manager.h>

static int check_freeze_action(struct mansession *s, const struct message *m)
{
	return AMI_SUCCESS;
}

static int load(void)
{
	ast_manager_register2(
		"CheckFreeze",
		EVENT_FLAG_SYSTEM,
		check_freeze_action,
		ast_module_info->self,
		"Check for freezes",
		"This action may be used to detect some freezes.\n"
		"This check is not in any way garanteed to succeed in detecting every freezes."
	);

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload(void)
{
	ast_manager_unregister("CheckFreeze");

	return 0;
}

static int reload(void)
{
	unload();
	load();

	return 0;
}

AST_MODULE_INFO(
	ASTERISK_GPL_KEY,
	AST_MODFLAG_LOAD_ORDER,
	"Freeze detection module",
	.load = load,
	.unload = unload,
	.reload = reload,
	.load_pri = AST_MODPRI_DEFAULT,
);
