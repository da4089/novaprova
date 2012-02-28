#include "common.h"
#include <sys/wait.h>
#include <sys/poll.h>
#include "u4c_priv.h"
#include "except.h"
#include <valgrind/memcheck.h>

using namespace std;

__u4c_exceptstate_t __u4c_exceptstate;
static volatile int caught_sigchld = 0;

#define dispatch_listeners(func, ...) \
    do { \
	vector<u4c_listener_t*>::iterator _i; \
	for (_i = listeners_.begin() ; _i != listeners_.end() ; ++_i) \
	    (*_i)->func(__VA_ARGS__); \
    } while(0)

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

void
u4c_globalstate_t::add_listener(u4c_listener_t *l)
{
    /* append to the list.  The order of adding is preserved for
     * dispatching */
    listeners_.push_back(l);
}

void
u4c_globalstate_t::set_listener(u4c_listener_t *l)
{
    /* just throw away the old ones */
    listeners_.clear();
    listeners_.push_back(l);
}

static void
handle_sigchld(int sig __attribute__((unused)))
{
    caught_sigchld = 1;
}

void
u4c_globalstate_t::begin()
{
    static bool init = false;

    if (!init)
    {
	signal(SIGCHLD, handle_sigchld);
	init = true;
    }

    dispatch_listeners(begin);
}

void
u4c_globalstate_t::end()
{
    dispatch_listeners(end);
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

const u4c_event_t *
u4c_globalstate_t::normalise_event(const u4c_event_t *ev)
{
    static u4c_event_t norm;

    memset(&norm, 0, sizeof(norm));
    norm.which = ev->which;
    norm.description = xstr(ev->description);
    if (ev->lineno == ~0U)
    {
	unsigned long pc = (unsigned long)ev->filename;
	const char *classname = 0;

	if (spiegel->describe_address(pc, &norm.filename,
			&norm.lineno, &classname, &norm.function))
	{
	    static char fullname[1024];
	    if (classname)
	    {
		snprintf(fullname, sizeof(fullname), "%s::%s",
			 classname, norm.function);
		norm.function = fullname;
	    }
	}
	else
	{
	    static char pcbuf[32];
	    snprintf(pcbuf, sizeof(pcbuf), "(0x%lx)", pc);
	    norm.function = pcbuf;
	    norm.filename = "";
	}
    }
    else
    {
	norm.filename = xstr(ev->filename);
	norm.lineno = ev->lineno;
	norm.function = xstr(ev->function);
    }

    return &norm;
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

u4c_result_t
u4c_globalstate_t::raise_event(const u4c_event_t *ev, enum u4c_functype ft)
{
    ev = normalise_event(ev);
    dispatch_listeners(add_event, ev, ft);

    switch (ev->which)
    {
    case EV_ASSERT:
    case EV_EXIT:
    case EV_SIGNAL:
    case EV_FIXTURE:
    case EV_VALGRIND:
    case EV_SLMATCH:
	return R_FAIL;
    case EV_EXPASS:
	return R_PASS;
    case EV_EXFAIL:
	return R_FAIL;
    case EV_EXNA:
	return R_NOTAPPLICABLE;
    default:
	/* there was an event, but it makes no difference */
	return R_UNKNOWN;
    }
}

u4c_child_t::u4c_child_t(pid_t pid, int fd, u4c_testnode_t *tn)
 :  pid_(pid),
    event_pipe_(fd),
    node_(tn),
    result_(R_UNKNOWN),
    finished_(false)
{
}

u4c_child_t::~u4c_child_t()
{
    close(event_pipe_);
}

void
u4c_child_t::poll_setup(struct pollfd &pfd)
{
    memset(&pfd, 0, sizeof(struct pollfd));
    if (finished_)
	return;
    pfd.fd = event_pipe_;
    pfd.events = POLLIN;
}

void
u4c_child_t::poll_handle(struct pollfd &pfd)
{
    if (finished_)
	return;
    if (!(pfd.revents & POLLIN))
	return;
    if (!u4c_proxy_listener_t::handle_call(event_pipe_, &result_))
	finished_ = true;
}

void u4c_child_t::merge_result(u4c_result_t r)
{
    __u4c_merge(result_, r);
}

u4c_child_t *
u4c_globalstate_t::fork_child(u4c_testnode_t *tn)
{
    pid_t pid;
#define PIPE_READ 0
#define PIPE_WRITE 1
    int pipefd[2];
    u4c_child_t *child;
    int delay_ms = 10;
    int max_sleeps = 20;
    int r;

    r = pipe(pipefd);
    if (r < 0)
    {
	perror("u4c: pipe");
	exit(1);
    }

    for (;;)
    {
	pid = fork();
	if (pid < 0)
	{
	    if (errno == EAGAIN && max_sleeps-- > 0)
	    {
		/* rats, we fork-bombed, try again after a delay */
		fprintf(stderr, "u4c: fork bomb! sleeping %u ms.\n",
			delay_ms);
		poll(0, 0, delay_ms);
		delay_ms += (delay_ms>>1);	/* exponential backoff */
		continue;
	    }
	    perror("u4c: fork");
	    exit(1);
	}
	break;
    }

    if (!pid)
    {
	/* child process: return, will run the test */
	close(pipefd[PIPE_READ]);
	event_pipe_ = pipefd[PIPE_WRITE];
	return NULL;
    }

    /* parent process */

    fprintf(stderr, "u4c: spawned child process %d for %s\n",
	    (int)pid, tn->get_fullname().c_str());
    close(pipefd[PIPE_WRITE]);
    child = new u4c_child_t(pid, pipefd[PIPE_READ], tn);
    children_.push_back(child);

    return child;
#undef PIPE_READ
#undef PIPE_WRITE
}

void
u4c_globalstate_t::handle_events()
{
    int r;

    if (!children_.size())
	return;

    while (!caught_sigchld)
    {
	pfd_.resize(children_.size());
	vector<u4c_child_t*>::iterator citr;
	vector<struct pollfd>::iterator pitr;
	for (pitr = pfd_.begin(), citr = children_.begin() ;
	     citr != children_.end() ; ++pitr, ++citr)
	    (*citr)->poll_setup(*pitr);

	r = poll(pfd_.data(), pfd_.size(), -1);
	if (r < 0)
	{
	    if (errno == EINTR)
		continue;
	    perror("u4c: poll");
	    return;
	}
	/* TODO: implement test timeout handling here */

	for (pitr = pfd_.begin(), citr = children_.begin() ;
	     citr != children_.end() ; ++pitr, ++citr)
	    (*citr)->poll_handle(*pitr);
    }
}

void
u4c_globalstate_t::reap_children()
{
    pid_t pid;
    int status;
    char msg[1024];

    for (;;)
    {
	pid = waitpid(-1, &status, WNOHANG);
	if (pid == 0)
	    break;
	if (pid < 0)
	{
	    if (errno == ESRCH || errno == ECHILD)
		break;
	    perror("u4c: waitpid");
	    return;
	}
	if (WIFSTOPPED(status))
	{
	    fprintf(stderr, "u4c: process %d stopped on signal %d, ignoring\n",
		    (int)pid, WSTOPSIG(status));
	    continue;
	}
	vector<u4c_child_t*>::iterator itr;
	for (itr = children_.begin() ;
	     itr != children_.end() && (*itr)->get_pid() != pid ;
	     ++itr)
	    ;
	if (itr == children_.end())
	{
	    /* some other process */
	    fprintf(stderr, "u4c: reaped stray process %d\n", (int)pid);
	    /* TODO: this is probably eventworthy */
	    continue;	    /* whatever */
	}
	u4c_child_t *child = *itr;

	if (WIFEXITED(status))
	{
	    if (WEXITSTATUS(status))
	    {
		u4c_event_t ev(EV_EXIT, msg, NULL, 0, NULL);
		snprintf(msg, sizeof(msg),
			 "child process %d exited with %d",
			 (int)pid, WEXITSTATUS(status));
		child->merge_result(__u4c_raise_event(&ev, FT_UNKNOWN));
	    }
	}
	else if (WIFSIGNALED(status))
	{
	    u4c_event_t ev(EV_SIGNAL, msg, NULL, 0, NULL);
	    snprintf(msg, sizeof(msg),
		    "child process %d died on signal %d",
		    (int)pid, WTERMSIG(status));
	    child->merge_result(__u4c_raise_event(&ev, FT_UNKNOWN));
	}

	/* notify listeners */
	nfailed_ += (child->get_result() == R_FAIL);
	nrun_++;
	dispatch_listeners(finished, child->get_result());
	dispatch_listeners(end_node, child->get_node());

	/* detach and clean up */
	children_.erase(itr);
	delete child;
    }

    caught_sigchld = 0;
    /* nothing to reap here, move along */
}

void
u4c_globalstate_t::run_function(enum u4c_functype ft, spiegel::function_t *f)
{
    vector<spiegel::value_t> args;
    spiegel::value_t ret = f->invoke(args);

    if (ft == FT_TEST)
    {
	assert(ret.which == spiegel::type_t::TC_VOID);
    }
    else
    {
	assert(ret.which == spiegel::type_t::TC_SIGNED_INT);
	int r = ret.val.vsint;

	if (r)
	{
	    static char cond[64];
	    snprintf(cond, sizeof(cond), "fixture retured %d", r);
	    u4c_throw(u4c_event_t(EV_FIXTURE, cond,
			    f->get_compile_unit()->get_absolute_path().c_str(),
			    0, f->get_name()));
	}
    }
}

void
u4c_globalstate_t::run_fixtures(u4c_testnode_t *tn, enum u4c_functype type)
{
    list<spiegel::function_t*> fixtures = tn->get_fixtures(type);
    list<spiegel::function_t*>::iterator itr;
    for (itr = fixtures.begin() ; itr != fixtures.end() ; ++itr)
	run_function(type, *itr);
}

static u4c_result_t
valgrind_errors(void)
{
    unsigned long leaked = 0, dubious = 0, reachable = 0, suppressed = 0;
    unsigned long nerrors;
    u4c_result_t res = R_UNKNOWN;
    char msg[1024];

    VALGRIND_DO_LEAK_CHECK;
    VALGRIND_COUNT_LEAKS(leaked, dubious, reachable, suppressed);
    if (leaked)
    {
	u4c_event_t ev(EV_VALGRIND, msg, NULL, 0, NULL);
	snprintf(msg, sizeof(msg),
		 "%lu bytes of memory leaked", leaked);
	__u4c_merge(res, __u4c_raise_event(&ev, FT_UNKNOWN));
    }

    nerrors = VALGRIND_COUNT_ERRORS;
    if (nerrors)
    {
	u4c_event_t ev(EV_VALGRIND, msg, NULL, 0, NULL);
	snprintf(msg, sizeof(msg),
		 "%lu unsuppressed errors found by valgrind", nerrors);
	__u4c_merge(res, __u4c_raise_event(&ev, FT_UNKNOWN));
    }

    return res;
}

u4c_result_t
u4c_globalstate_t::run_test_code(u4c_testnode_t *tn)
{
    u4c_result_t res = R_UNKNOWN;
    const u4c_event_t *ev;

    u4c_try
    {
	run_fixtures(tn, FT_BEFORE);
    }
    u4c_catch(ev)
    {
	__u4c_merge(res, __u4c_raise_event(ev, FT_BEFORE));
    }

    if (res == R_UNKNOWN)
    {
	u4c_try
	{
	    run_function(FT_TEST, tn->get_function(FT_TEST));
	}
	u4c_catch(ev)
	{
	    __u4c_merge(res, __u4c_raise_event(ev, FT_TEST));
	}

	u4c_try
	{
	    run_fixtures(tn, FT_AFTER);
	}
	u4c_catch(ev)
	{
	    __u4c_merge(res, __u4c_raise_event(ev, FT_AFTER));
	}

	/* If we got this far and nothing bad
	 * happened, we might have passed */
	__u4c_merge(res, R_PASS);
    }

    __u4c_merge(res, valgrind_errors());
    return res;
}


void
u4c_globalstate_t::begin_test(u4c_testnode_t *tn)
{
    u4c_child_t *child;
    u4c_result_t res;

    {
	static int n = 0;
	if (++n > 60)
	    return;
    }

    fprintf(stderr, "%s: begin test %s\n",
	    u4c_reltimestamp(), tn->get_fullname().c_str());

    dispatch_listeners(begin_node, tn);

    child = fork_child(tn);
    if (child)
	return; /* parent process */

    /* child process */
    set_listener(new u4c_proxy_listener_t(event_pipe_));
    res = run_test_code(tn);
    dispatch_listeners(finished, res);
    fprintf(stderr, "u4c: child process %d (%s) finishing\n",
	    (int)getpid(), tn->get_fullname().c_str());
    exit(0);
}

void
u4c_globalstate_t::wait()
{
    handle_events();
    reap_children();
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/
