#include <asterisk.h>
#include <asterisk/app.h>
#include <asterisk/astobj2.h>
#include <asterisk/channel.h>
#include <asterisk/cli.h>
#include <asterisk/dlinkedlists.h>
#include <asterisk/lock.h>
#include <asterisk/logger.h>
#include <asterisk/module.h>
#include <asterisk/pbx.h>
#include <asterisk/vector.h>
#include <sys/eventfd.h>

#include <dlfcn.h>

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

/*
 * The following definitions are here so we can extract the library from app_queue.
 * These structures are not defined in header files so we have to redefine them here.
 * These definitions come from pbx_app.c and loader.c
 */
struct ast_module_user {
	struct ast_channel *chan;
	AST_LIST_ENTRY(ast_module_user) entry;
};

AST_DLLIST_HEAD(module_user_list, ast_module_user);
AST_VECTOR(module_vector, struct ast_module *);

struct ast_module {
	const struct ast_module_info *info;
	/*! Used to get module references into refs log */
	void *ref_debug;
	/*! The shared lib. */
	void *lib;
	/*! Number of 'users' and other references currently holding the module. */
	int usecount;
	/*! List of users holding the module. */
	struct module_user_list users;

	/*! List of required module names. */
	struct ast_vector_string requires;
	/*! List of optional api modules. */
	struct ast_vector_string optional_modules;
	/*! List of modules this enhances. */
	struct ast_vector_string enhances;

	/*!
	 * \brief Vector holding pointers to modules we have a reference to.
	 *
	 * When one module requires another, the required module gets added
	 * to this list with a reference.
	 */
	struct module_vector reffed_deps;
	struct {
		/*! The module running and ready to accept requests. */
		unsigned int running:1;
		/*! The module has declined to start. */
		unsigned int declined:1;
		/*! This module is being held open until it's time to shutdown. */
		unsigned int keepuntilshutdown:1;
		/*! The module is built-in. */
		unsigned int builtin:1;
		/*! The admin has declared this module is required. */
		unsigned int required:1;
		/*! This module is marked for preload. */
		unsigned int preload:1;
	} flags;
	AST_DLLIST_ENTRY(ast_module) entry;
	char resource[0];
};

/*! \brief ast_app: A registered application */
struct ast_app {
	int (*execute)(struct ast_channel *chan, const char *data);
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(synopsis);     /*!< Synopsis text for 'show applications' */
		AST_STRING_FIELD(since);        /*!< Since text for 'show applications' */
		AST_STRING_FIELD(description);  /*!< Description (help text) for 'show application &lt;name&gt;' */
		AST_STRING_FIELD(syntax);       /*!< Syntax text for 'core show applications' */
		AST_STRING_FIELD(arguments);    /*!< Arguments description */
		AST_STRING_FIELD(seealso);      /*!< See also */
	);
#ifdef AST_XML_DOCS
	enum ast_doc_src docsrc;		/*!< Where the documentation come from. */
#endif
	AST_RWLIST_ENTRY(ast_app) list;		/*!< Next app in list */
	struct ast_module *module;		/*!< Module this app belongs to */
	char name[0];				/*!< Name of the application */
};
/* Done with the definitions from pbx_app.c and loader.c */

static struct ast_app *app_queue;
static struct checker global_checker;
static int dangerous_commands_enabled = 0;
static int queue_checks_enabled = 0;

ast_mutex_t* (*ast_queues_get_mutex)(void);
struct ao2_container* (*ast_queues_get_container)(void);

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
	struct ao2_container *queues;
	struct ao2_iterator qiter;
	void *q;	/* We don't really care about the type, we only want to lock/unlock it */

	ret = check_mutex(ast_channels_get_mutex(), c->timeout, "global channels container");
	if (ret == CHECK_MUTEX_TIMEDOUT) {
		ast_log(LOG_ERROR, "failed to acquire the global channels container lock in under %d seconds\n", c->timeout);
		return -1;
	}

	if (queue_checks_enabled) {
		queues = ast_queues_get_container();
		/* First checking container lock */
		ret = check_mutex(ast_queues_get_mutex(), c->timeout, "global queues container");
		if (ret == CHECK_MUTEX_TIMEDOUT) {
			ast_log(LOG_ERROR, "failed to acquire the global queues container lock in under %d seconds\n", c->timeout);
			return -1;
		}
		/* Then each individual queues */
		qiter = ao2_iterator_init(queues, 0);
		while ((q = ao2_t_iterator_next(&qiter, "Iterate over queues"))) {
			ret = check_mutex(ao2_object_get_lockaddr(q), c->timeout, "individual queue");
			if (ret == CHECK_MUTEX_TIMEDOUT) {
				ast_log(LOG_ERROR, "failed to acquire the individual queue lock in under %d seconds\n", c->timeout);
				return -1;
			}
		}
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

static char *cli_queue(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	const char *what;
	struct ao2_container *queues;
	struct ao2_iterator qiter;
	void *q;	/* We don't really care about the type, we only want to lock/unlock */

	if (!queue_checks_enabled) {
		ast_cli(a->fd, "Queue lock CLI commands are disabled.\n");
		return CLI_FAILURE;
	}
	queues = ast_queues_get_container();

	switch (cmd) {
		case CLI_INIT:
			e->command = "freeze queue {global_lock|lock|global_unlock|unlock}";
			e->usage = "Usage: freeze queue {global_lock|lock|global_unlock|unlock}\n";
			return NULL;
		case CLI_GENERATE:
			return NULL;
	}

	what = a->argv[e->args - 1];

	if (!dangerous_commands_enabled) {
		ast_cli(a->fd, "Dangerous freeze CLI commands are disabled.\n");
		return CLI_FAILURE;
	}

	if (!strcasecmp(what, "global_lock")) {
		ast_mutex_lock(ast_queues_get_mutex());
		ast_cli(a->fd, "The global queue container is now LOCKED\n");
		ast_log(LOG_WARNING, "The global queue container is now LOCKED\n");
	} else if (!strcasecmp(what, "lock")) {
		qiter = ao2_iterator_init(queues, 0);
		while ((q = ao2_t_iterator_next(&qiter, "Iterate over queues"))) {
			ast_mutex_lock(ao2_object_get_lockaddr(q));
		}
		ast_cli(a->fd, "All queues are now LOCKED\n");
		ast_log(LOG_WARNING, "All queues are now LOCKED\n");
	} else if (!strcasecmp(what, "global_unlock")) {
		ast_mutex_unlock(ast_queues_get_mutex());
		ast_cli(a->fd, "The global queue container is now UNLOCKED.\n");
		ast_log(LOG_WARNING, "The global queue container is now UNLOCKED\n");
	} else if (!strcasecmp(what, "unlock")) {
		qiter = ao2_iterator_init(queues, 0);
		while ((q = ao2_t_iterator_next(&qiter, "Iterate over queues"))) {
			ast_mutex_unlock(ao2_object_get_lockaddr(q));
		}
		ast_cli(a->fd, "All queues are now UNLOCKED.\n");
		ast_log(LOG_WARNING, "All queues are now UNLOCKED\n");
	} else {
		return CLI_SHOWUSAGE;
	}

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_enable, "Enable/Disable dangerous freeze CLI commands"),
	AST_CLI_DEFINE(cli_channel, "Lock/Unlock the global channel container lock"),
	AST_CLI_DEFINE(cli_queue, "Lock/Unlock the global queue container lock"),
};

static int load_module(void)
{
	if ((app_queue = pbx_findapp("Queue"))) {
		ast_queues_get_mutex = dlsym(app_queue->module->lib, "ast_queues_get_mutex");
		ast_queues_get_container = dlsym(app_queue->module->lib, "ast_queues_get_container");
		if (!ast_queues_get_mutex || !ast_queues_get_container) {
			ast_log(LOG_WARNING, "The Queue application does not expose necessary symbols! Disabling queue checks.\n");
			queue_checks_enabled = 0;
		} else {
			queue_checks_enabled = 1;
		}
	} else {
		ast_log(LOG_WARNING, "There is no Queue application available. Disabling queue checks.\n");
		queue_checks_enabled = 0;
	}

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
	.requires = "app_queue",
);
