#include <asterisk.h>
#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/module.h>
#include <asterisk/manager.h>

static int dangerous_commands_enabled = 0;

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

static char *cli_enable(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	const char *what;

	switch (cmd) {
		case CLI_INIT:
			e->command = "freeze {enable|disable}";
			e->usage = "Usage: freeze {enable|disable}\n";
			return NULL;
		case CLI_GENERATE:
			return NULL;
	}

	what = a->argv[e->args - 1];

	if (!strcasecmp(what, "enable")) {
		dangerous_commands_enabled = 1;
		ast_cli(a->fd, "Dangerous freeze CLI commands enabled.\n");
	} else if (!strcasecmp(what, "disable")) {
		dangerous_commands_enabled = 0;
		ast_cli(a->fd, "Dangerous freeze CLI commands disabled.\n");
	} else {
		return CLI_SHOWUSAGE;
	}

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_enable, "Enable/Disable dangerous freeze CLI commands"),
};

static int load_module(void)
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

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	ast_manager_unregister("CheckFreeze");

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Freeze Detection Module",
	.load = load_module,
	.unload = unload_module,
);
