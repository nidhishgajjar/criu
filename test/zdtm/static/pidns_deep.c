/*
 * Test: 3-level nested PID namespace dump/restore
 *
 * Creates a process hierarchy spanning 3 PID namespace levels:
 *   Level 0 (root): parent process
 *   Level 1: child (PID 1 in child pidns)
 *   Level 2: grandchild (PID 1 in grandchild pidns)
 *
 * Each level calls setsid() so the session leader is visible
 * within its own PID namespace (required for CRIU dump).
 *
 * After dump/restore, verifies all PIDs are preserved at each level.
 */
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "zdtmtst.h"

const char *test_doc = "Check dump/restore of 3-level nested PID namespaces";
const char *test_author = "nidhishgajjar <hacker@scigic.com>";

int main(int argc, char **argv)
{
	int pipe_c_ready[2], pipe_c_go[2], pipe_c_result[2];
	int pipe_g_ready[2], pipe_g_go[2], pipe_g_result[2];
	pid_t child;
	pid_t my_pid;
	int status;
	char buf;

	test_init(argc, argv);

	if (pipe(pipe_c_ready) || pipe(pipe_c_go) || pipe(pipe_c_result) ||
	    pipe(pipe_g_ready) || pipe(pipe_g_go) || pipe(pipe_g_result)) {
		pr_perror("pipe");
		return 1;
	}

	my_pid = getpid();

	/* Level 1: create child in new PID namespace */
	if (unshare(CLONE_NEWPID)) {
		pr_perror("unshare level 1");
		return 1;
	}

	child = fork();
	if (child < 0) {
		pr_perror("fork level 1");
		return 1;
	}

	if (child == 0) {
		/* Child: PID 1 in level-1 pidns */
		pid_t child_pid, gc;
		char gc_result;

		close(pipe_c_ready[0]);
		close(pipe_c_go[1]);
		close(pipe_c_result[0]);

		setsid();

		child_pid = getpid();
		if (child_pid != 1) {
			fprintf(stderr, "Level 1: expected PID 1, got %d\n", child_pid);
			_exit(1);
		}

		/* Level 2: create grandchild in another new PID namespace */
		if (unshare(CLONE_NEWPID)) {
			perror("unshare level 2");
			_exit(1);
		}

		gc = fork();
		if (gc < 0) {
			perror("fork level 2");
			_exit(1);
		}

		if (gc == 0) {
			/* Grandchild: PID 1 in level-2 pidns */
			pid_t gc_pid;
			char res = '0';

			close(pipe_c_ready[1]);
			close(pipe_c_go[0]);
			close(pipe_c_result[1]);
			close(pipe_g_ready[0]);
			close(pipe_g_go[1]);
			close(pipe_g_result[0]);

			setsid();

			gc_pid = getpid();
			if (gc_pid != 1) {
				fprintf(stderr, "Level 2: expected PID 1, got %d\n", gc_pid);
				_exit(1);
			}

			/* Signal ready */
			write(pipe_g_ready[1], "R", 1);
			close(pipe_g_ready[1]);

			/* Wait for go signal (after restore) */
			read(pipe_g_go[0], &buf, 1);
			close(pipe_g_go[0]);

			/* Verify PID preserved */
			if (getpid() != 1)
				res = '2';

			write(pipe_g_result[1], &res, 1);
			close(pipe_g_result[1]);
			_exit(0);
		}

		/* Child: wait for grandchild ready, then signal parent */
		close(pipe_g_ready[1]);
		close(pipe_g_go[0]);
		close(pipe_g_result[1]);

		if (read(pipe_g_ready[0], &buf, 1) != 1 || buf != 'R') {
			_exit(1);
		}
		close(pipe_g_ready[0]);

		/* Signal parent we're ready (both levels up) */
		write(pipe_c_ready[1], "R", 1);
		close(pipe_c_ready[1]);

		/* Wait for go (after restore) */
		read(pipe_c_go[0], &buf, 1);
		close(pipe_c_go[0]);

		/* Forward go to grandchild */
		write(pipe_g_go[1], "G", 1);
		close(pipe_g_go[1]);

		/* Get grandchild result */
		read(pipe_g_result[0], &gc_result, 1);
		close(pipe_g_result[0]);

		/* Check our own PID */
		buf = '0';
		if (getpid() != 1)
			buf = '1';
		else if (gc_result != '0')
			buf = gc_result;

		write(pipe_c_result[1], &buf, 1);
		close(pipe_c_result[1]);

		waitpid(gc, NULL, 0);
		_exit(0);
	}

	/* Parent: root namespace */
	close(pipe_c_ready[1]);
	close(pipe_c_go[0]);
	close(pipe_c_result[1]);
	close(pipe_g_ready[1]);
	close(pipe_g_go[1]);
	close(pipe_g_result[1]);
	close(pipe_g_ready[0]);
	close(pipe_g_go[0]);
	close(pipe_g_result[0]);

	/* Wait for all levels ready */
	if (read(pipe_c_ready[0], &buf, 1) != 1 || buf != 'R') {
		fail("Hierarchy not ready");
		kill(child, SIGKILL);
		return 1;
	}
	close(pipe_c_ready[0]);

	test_msg("3-level PID namespace hierarchy ready. Root PID: %d, child host PID: %d\n",
		 my_pid, child);

	/* Checkpoint happens here */
	test_daemon();
	test_waitsig();

	/* After restore: verify root PID */
	if (getpid() != my_pid) {
		fail("Root PID changed: %d -> %d", my_pid, getpid());
		goto out;
	}

	/* Tell hierarchy to check their PIDs */
	write(pipe_c_go[1], "G", 1);
	close(pipe_c_go[1]);

	/* Get result from child (which includes grandchild result) */
	if (read(pipe_c_result[0], &buf, 1) != 1) {
		fail("Failed to read result from child");
		goto out;
	}
	close(pipe_c_result[0]);

	if (buf == '0')
		pass();
	else if (buf == '1')
		fail("Level 1 PID not preserved");
	else if (buf == '2')
		fail("Level 2 PID not preserved");
	else
		fail("Unknown error code: %c", buf);

out:
	waitpid(child, &status, 0);
	return 0;
}
