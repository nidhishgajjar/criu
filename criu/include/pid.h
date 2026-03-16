#ifndef __CR_PID_H__
#define __CR_PID_H__

#include <compel/task-state.h>
#include "stdbool.h"
#include "rbtree.h"

/*
 * Maximum PID namespace nesting depth.
 * Matches the kernel's MAX_PID_NS_LEVEL (32).
 */
#define MAX_NS_NESTING 32

/*
 * Default allocation size for ns[] entries in struct pid.
 * This covers common nested PID namespace use cases without
 * wasting excessive memory. Each extra level costs ~28 bytes.
 */
#define DEFAULT_NS_ALLOC 8

/*
 * Task states, used in e.g. struct pid's state.
 */
enum __criu_task_state {
	/* Values shared with compel */
	TASK_ALIVE = COMPEL_TASK_ALIVE,
	TASK_DEAD = COMPEL_TASK_DEAD,
	TASK_STOPPED = COMPEL_TASK_STOPPED,
	TASK_ZOMBIE = COMPEL_TASK_ZOMBIE,
	/* Own internal states */
	TASK_HELPER = COMPEL_TASK_MAX + 1,
	TASK_THREAD,
	/* new values are to be added before this line */
	TASK_UNDEF = 0xff
};

struct pid {
	struct pstree_item *item;
	/*
	 * The @real pid is used to fetch tasks during dumping stage,
	 * This is a global pid seen from the context where the dumping
	 * is running.
	 */
	pid_t real;

	int state; /* TASK_XXX constants */
	/* If an item is in stopped state it has a signal number
	 * that caused task to stop.
	 */
	int stop_signo;

	/*
	 * Number of PID namespace levels for this process.
	 * 1 = single namespace (legacy), N = nested N levels deep.
	 * ns[0] = outermost (root) PID, ns[ns_level-1] = innermost.
	 */
	unsigned int ns_level;

	/*
	 * The @virt pid is one which used in the image itself and keeps
	 * the pid value to be restored. This pid fetched from the
	 * dumpee context, because the dumpee might have own pid namespace.
	 *
	 * For N-level PID namespaces:
	 *   ns[0].virt = PID in outermost (root) namespace
	 *   ns[N-1].virt = PID in innermost namespace
	 *
	 * The rb-tree is keyed on ns[0].virt (outermost PID) which
	 * is always unique across the process tree.
	 */
	struct {
		pid_t virt;
		struct rb_node node;
	} ns[1]; /* Must be at the end of struct pid */
};

/*
 * When we have to restore a shared resource, we mush select which
 * task should do it, and make other(s) wait for it. In order to
 * avoid deadlocks, always make task with lower pid be the restorer.
 */
static inline bool pid_rst_prio(unsigned pid_a, unsigned pid_b)
{
	return pid_a < pid_b;
}

static inline bool pid_rst_prio_eq(unsigned pid_a, unsigned pid_b)
{
	return pid_a <= pid_b;
}

#endif /* __CR_PID_H__ */
