// provenance.hpp: checks the commit and dirty flag a timed program was
// given against the git state it can observe, before it does any work.
//
// Used by harness_a, harness_b, measure_condvar_wakeup,
// measure_parse_cost and measure_pacing_floor. Each takes the commit and
// dirty flag as its first two arguments and writes them into its output,
// and each calls verify_provenance straight after parsing them, before it
// applies QoS, opens a file or starts a thread, and exits if it fails.
// convert_capture has its own check, behind --require-clean.
//
// Outputs: none on success; one diagnostic on stderr per refusal.
// Related: env/dump_environment.sh, which computes the dirty flag by the
// same rule for the environment dump recorded beside each session.
//
// The rule is one-sided. The commit must be HEAD, and a flag of 0 is
// refused if the tree is dirty. A flag of 1 on a clean tree is accepted:
// a result that understates how clean its build was misleads nobody, and
// the tail dump passes 1 on purpose.
//
// What counts as dirty: any change to a tracked file, anywhere, and any
// untracked file outside results/ and env/. Those two directories hold
// what the programs and the environment dump write, and nothing in them
// feeds the build. Without the exception the README's own sequence would
// refuse itself: the first harness_b run leaves an untracked CSV in
// results/, and the next would see a dirty tree. An untracked source file
// anywhere else still counts.
//
// git runs in the working directory, as for convert_capture. Outside a
// repository nothing can be verified, so the program refuses.

#pragma once

#include <sys/wait.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>


namespace provenance_detail {

struct CommandOutput {
    // popen and pclose both succeeded, so exit_status means something.
    bool ran = false;

    int exit_status = -1;
    std::string output;
};


// popen goes through /bin/sh. Both commands passed here are literals in
// this file with nothing interpolated, so no argument reaches the shell.
inline CommandOutput run_command(const char* command)
{
    CommandOutput result;

    std::FILE* pipe = ::popen(command, "r");

    if (pipe == nullptr) {
        return result;
    }

    char buffer[4096];

    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result.output += buffer;
    }

    const int status = ::pclose(pipe);

    if (status == -1 || !WIFEXITED(status)) {
        return result;
    }

    result.ran = true;
    result.exit_status = WEXITSTATUS(status);

    return result;
}


// Porcelain v1 paths are relative to the top of the repository whatever
// the working directory, so the prefix test does not depend on where the
// program was started. With --untracked-files=all every untracked file is
// listed by its own path rather than collapsed into its directory.
inline bool counts_as_dirty(std::string_view line)
{
    if (line.substr(0, 3) != "?? ") {
        return true;
    }

    const std::string_view path = line.substr(3);

    return path.substr(0, 8) != "results/" && path.substr(0, 4) != "env/";
}

} // namespace provenance_detail


// Returns false, after printing why, if the arguments claim a git state
// this process cannot observe.
inline bool verify_provenance(const std::string& commit, bool dirty)
{
    using provenance_detail::CommandOutput;
    using provenance_detail::run_command;

    const CommandOutput head = run_command("git rev-parse HEAD 2>/dev/null");

    if (!head.ran || head.exit_status != 0) {
        std::cerr
            << "error: cannot verify provenance: git rev-parse HEAD failed;"
            << " run from inside the repository\n";
        return false;
    }

    std::string observed = head.output;

    while (!observed.empty() &&
           (observed.back() == '\n' || observed.back() == '\r')) {
        observed.pop_back();
    }

    if (observed != commit) {
        std::cerr
            << "error: the commit argument is not HEAD\n"
            << "  argument: " << commit << '\n'
            << "  HEAD:     " << observed << '\n';
        return false;
    }

    if (dirty) {
        return true;
    }

    const CommandOutput status = run_command(
        "git status --porcelain --untracked-files=all 2>/dev/null");

    if (!status.ran || status.exit_status != 0) {
        std::cerr
            << "error: cannot verify provenance: git status failed\n";
        return false;
    }

    std::size_t shown = 0;
    std::size_t start = 0;

    while (start < status.output.size()) {
        std::size_t end = status.output.find('\n', start);

        if (end == std::string::npos) {
            end = status.output.size();
        }

        const std::string_view line(
            status.output.data() + start, end - start);

        if (provenance_detail::counts_as_dirty(line)) {
            if (shown == 0) {
                std::cerr
                    << "error: dirty flag is 0 but the working tree is"
                    << " not clean\n";
            }

            if (shown < 10) {
                std::cerr << "  " << line << '\n';
            }

            ++shown;
        }

        start = end + 1;
    }

    if (shown > 10) {
        std::cerr << "  ... and " << (shown - 10) << " more\n";
    }

    return shown == 0;
}
