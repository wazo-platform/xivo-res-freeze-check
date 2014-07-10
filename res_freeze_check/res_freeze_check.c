#include <asterisk.h>
#include <asterisk/channel.h>
#include <asterisk/module.h>
#include <asterisk/manager.h>

static int check_freeze_action(struct mansession *s, const struct message *m)
{
	unsigned int max_wait_time = 30 * 1000000;
	unsigned int wait_time = 1000;
	unsigned int n = max_wait_time / wait_time;

	const char *id = astman_get_header(m, "ActionID");

	ast_log(LOG_DEBUG, "Testing the global channel container lock...\n");
	if (!ast_channel_check_lock(n, wait_time)) {
		ast_log(LOG_DEBUG, "Success\n");
		astman_append(s, "Response: Success\r\n");
	} else {
		ast_log(LOG_DEBUG, "Fail\n");
		ast_log(LOG_ERROR, "Failed to acquire the global channel container lock, asterisk is most likely deadlocked\n");
		astman_append(s, "Response: Fail\r\n");
	}

	if (!ast_strlen_zero(id)) {
		astman_append(s, "ActionID: %s\r\n", id);
	}

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

AST_MODULE_INFO(
	ASTERISK_GPL_KEY,
	AST_MODFLAG_LOAD_ORDER,
	"Freeze detection module",
	.load = load,
	.unload = unload,
	.load_pri = AST_MODPRI_DEFAULT,
);
