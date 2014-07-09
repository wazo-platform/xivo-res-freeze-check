#include <asterisk.h>
#include <asterisk/module.h>

static int load(void)
{
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload(void)
{
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
