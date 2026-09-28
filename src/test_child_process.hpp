// test_child_process.hpp: run_in_child, which runs a function expected to
// abort in a forked child and reports how the child ended.
//
// The preconditions in replay_schedule.cpp and replay_producer.hpp abort,
// so their tests cannot run a failing case in-process. run_in_child forks,
// points the child's stderr at a pipe, runs the body there, and returns
// whether the child died of SIGABRT along with everything it wrote to
// stderr. The test then checks which precondition fired: a check that any
// abort satisfied would still pass if two guards were swapped.
//
// Only the process control is shared. Each test file keeps its own
// wrapper, because the wrapper reports through that file's own check().
//
// Not ctest's WILL_FAIL property instead: it inverts the exit code, and
// CMake's documentation says a signal abort may fail the test even with
// WILL_FAIL set. std::abort ends the process with SIGABRT.
//
// Related: test_replay_schedule.cpp, test_replay_producer.cpp (its two
// users).

#pragma once

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

    // Call only while the process has one thread. The child gets a copy
    // of the calling thread alone, so a lock another thread held at the
    // fork would stay held in the child for good.
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

        // Only reached if the precondition failed to fire. _exit, not
        // exit: exit would flush stdio buffers copied from the parent,
        // which could print their contents a second time.
        _exit(0);
    }

    // The parent's copy of the write end must be closed, or read() below
    // never sees end-of-file.
    close(pipe_fds[1]);

    // Read to end-of-file before waiting. A child that filled the pipe
    // would block in write() while the parent blocked in waitpid().
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