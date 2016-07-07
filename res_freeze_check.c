#include <asterisk.h>
#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/lock.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <sys/eventfd.h>

#define DEFAULT_CHECK_INTERVAL_SECS 60
#define DEFAULT_CHECK_TIMEOUT_SECS 30

#define CHECK_MUTEX_TIMEDOUT 1

/*
 * asterisk defines a macro "pthread_mutex_unlock" to force people to use
 * ast_mutex_unlock, but since we are directly locking a pthread_mutex_t with
 * pthread_mutex_timedlock, we also need to unlock it with pthread_mutex_unlock,
 * so we have to undef pthread_mutex_unlock here
 */
#undef pthread_mutex_unlock

struct checker {
	pthread_t thread;
	int eventfd;
	int interval;	/* check interval in seconds */
	int timeout;	/* check timeout in seconds */
};

static struct checker global_checker;
static int dangerous_commands_enabled = 0;

static int check_mutex(ast_mutex_t *mutex, int timeout, const char *name)
{
	struct timespec abs_timeout;
	int result;
	int ret;

	ret = clock_gettime(CLOCK_REALTIME, &abs_timeout);
	if (ret == -1) {
		ast_log(LOG_ERROR, "check mutex failed: clock_gettime: %s\n", strerror(errno));
		return -1;
	}

	abs_timeout.tv_sec += timeout;

	ast_debug(1, "Testing if mutex \"%s\" can be locked in less than %d seconds...\n", name, timeout);
	ret = pthread_mutex_timedlock(&mutex->mutex, &abs_timeout);
	switch (ret) {
	case 0:
		pthread_mutex_unlock(&mutex->mutex);
		result = 0;
		break;
	case ETIMEDOUT:
		result = CHECK_MUTEX_TIMEDOUT;
		break;
	default:
		ast_log(LOG_ERROR, "check mutex failed: pthread_mutex_timedlock: %s\n", strerror(ret));
		result = -1;
		break;
	}

	ast_debug(1, "Test completed.\n");

	return result;
}

static int checker_init(struct checker *c)
{
	c->eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (c->eventfd == -1) {
		ast_log(LOG_ERROR, "checker init failed: eventfd: %s\n", strerror(errno));
		return -1;
	}

	c->interval = DEFAULT_CHECK_INTERVAL_SECS;
	c->timeout = DEFAULT_CHECK_TIMEOUT_SECS;

	return 0;
}

static void checker_destroy(struct checker *c)
{
	close(c->eventfd);
}

static int checker_check_mutexes(struct checker *c)
{
	int ret;

	ret = check_mutex(ast_channels_get_mutex(), c->timeout, "global channels container");
	if (ret == CHECK_MUTEX_TIMEDOUT) {
		ast_log(LOG_ERROR, "failed to acquire the global channels container lock in under %d seconds\n", c->timeout);
		return -1;
	}

	return 0;
}

static void *checker_run(void *data)
{
	struct checker *c = data;
	struct pollfd fds[1];
	int poll_timeout = c->interval * 1000;
	int ret;

	fds[0].fd = c->eventfd;
	fds[0].events = POLLIN;

	for (;;) {
		ret = poll(fds, ARRAY_LEN(fds), poll_timeout);
		if (ret == -1) {
			ast_log(LOG_ERROR, "checker run failed: poll: %s\n", strerror(errno));
			goto end;
		}

		if (fds[0].revents) {
			goto end;
		}

		ret = checker_check_mutexes(c);
		if (ret) {
			ast_log(LOG_ERROR, "asterisk is most likely deadlocked: aborting...\n");
			/* sleep a little to make sure our log message is written */
			sleep(2);
			abort();
		}
	}

end:
	return NULL;
}

static int checker_start(struct checker *c)
{
	int ret;

	ret = ast_pthread_create_background(&c->thread, NULL, checker_run, c);
	if (ret) {
		ast_log(LOG_ERROR, "checker start failed: pthread_create: %s\n", strerror(ret));
		return -1;
	}

	return 0;
}

static void checker_stop(struct checker *c)
{
	static const uint64_t val = 1;
	ssize_t n;

	n = write(c->eventfd, &val, sizeof(val));
	if (n != sizeof(val)) {
		ast_log(LOG_ERROR, "checker stop failed: write returned %zd\n", n);
	}

	pthread_join(c->thread, NULL);
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

static char *cli_channel(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	const char *what;

	switch (cmd) {
		case CLI_INIT:
			e->command = "freeze channel {lock|unlock}";
			e->usage = "Usage: freeze channel {lock|unlock}\n";
			return NULL;
		case CLI_GENERATE:
			return NULL;
	}

	what = a->argv[e->args - 1];

	if (!dangerous_commands_enabled) {
		ast_cli(a->fd, "Dangerous freeze CLI commands are disabled.\n");
		return CLI_FAILURE;
	}

	if (!strcasecmp(what, "lock")) {
		ast_mutex_lock(ast_channels_get_mutex());
		ast_cli(a->fd, "The global channel container is now LOCKED\n");
		ast_log(LOG_WARNING, "The global channel container is now LOCKED\n");
	} else if (!strcasecmp(what, "unlock")) {
		ast_mutex_unlock(ast_channels_get_mutex());
		ast_cli(a->fd, "The global channel contained is now UNLOCKED.\n");
		ast_log(LOG_WARNING, "The global channel container is now UNLOCKED\n");
	} else {
		return CLI_SHOWUSAGE;
	}

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_enable, "Enable/Disable dangerous freeze CLI commands"),
	AST_CLI_DEFINE(cli_channel, "Lock/Unlock the global channel container lock"),
};

static int load_module(void)
{
	if (checker_init(&global_checker)) {
		goto fail1;
	}

	if (checker_start(&global_checker)) {
		goto fail2;
	}

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));

	return AST_MODULE_LOAD_SUCCESS;

fail2:
	checker_destroy(&global_checker);
fail1:

	return AST_MODULE_LOAD_DECLINE;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	checker_stop(&global_checker);
	checker_destroy(&global_checker);

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Freeze Detection Module",
	.load = load_module,
	.unload = unload_module,
);
