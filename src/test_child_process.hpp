#pragma once

// Running a body that is expected to abort, in a forked child.
//
// Extracted from test_replay_producer.cpp when a second test binary
// needed the same thing. The reporting wrapper stays in each test file,
// because it calls that file's own check() and names that file's own
// suite; only the process control is shared, and the process control is
// the part with the subtlety in it.
//
// Preconditions abort, so they cannot be checked in-process directly.
//
// The first attempt registered separate ctest entries with WILL_FAIL.
// That does not work and the reason is worth keeping: WILL_FAIL inverts a
// non-zero *exit code*, but a process killed by a signal is classified by
// ctest as "Subprocess aborted" — an exception, and exceptions fail
// regardless of the property. std::abort raises SIGABRT, so it lands in
// the one category WILL_FAIL cannot reach.
//
// Forking is better than a property anyway. The parent captures the
// child's stderr and matches the diagnostic, so a test verifies *which*
// precondition fired rather than merely that the process died — a check
// that any abort satisfies would pass if the guards were swapped.

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <iostream>
#include <string>


struct ChildOutcome {
    bool aborted = false;
    std::string diagnostic;
};


inline ChildOutcome run_in_child(void (*body)())
{
    ChildOutcome outcome;

    int pipe_fds[2];

    if (pipe(pipe_fds) != 0) {
        std::cerr << "pipe() failed\n";
        return outcome;
    }

    const pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "fork() failed\n";
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return outcome;
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);

        body();

        // Only reached if the precondition failed to fire.
        _exit(0);
    }

    close(pipe_fds[1]);

    char buffer[512];
    ssize_t n = 0;

    while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        outcome.diagnostic.append(buffer, static_cast<std::size_t>(n));
    }

    close(pipe_fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    outcome.aborted =
        WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;

    return outcome;
}