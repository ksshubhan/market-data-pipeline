#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>


// End-to-end tests for the converter, driven as a child process.
//
// This is the first test in the project that runs a binary rather than
// linking a translation unit, because the things worth testing here are
// exit statuses, diagnostics on stderr and what is left on disk
// afterwards — none of which survive being called as a function.
//
// test_child_process.hpp is deliberately not used. It runs a function in
// a forked child and classifies the outcome by SIGABRT, which is right
// for an in-process precondition. The converter never aborts: it prints
// and returns 1. Same idea, different mechanism, so the runner below is
// a sibling of that header rather than a client of it.
//
// WHAT THIS SUITE DOES NOT COVER. The converter has ten guards. Seven are
// exercised here. The other three — failure of the contents fsync, of the
// link that publishes, and of the directory fsync — were each verified by
// hand with an injected invalid descriptor or an injected bad
// destination, and they cannot be reached from outside the process. There
// is no filesystem state that makes fsync fail on a descriptor this
// program just opened successfully, and the .tmp and the .bin share a
// directory by construction, so no permission state breaks the publish
// without breaking the .tmp creation first. Automating them would mean
// shipping fault injection in production code, which trades a real
// guarantee for a testable one. **A green run of this suite is not
// evidence about those three.** See §7.6 for their controls.
//
// The F_FULLFSYNC downgrade path is also untested, here and everywhere:
// APFS has never rejected it, so the fallback has never run.
//
// No assert() anywhere — every configured build defines NDEBUG.

namespace {

std::uint64_t g_failures = 0;
std::uint64_t g_skipped = 0;


void check(const char* test_name, bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL [" << test_name << "] " << what << '\n';
        ++g_failures;
    }
}


void check_exit(
    const char* test_name,
    const char* what,
    int observed,
    int expected
)
{
    if (observed != expected) {
        std::cerr
            << "FAIL [" << test_name << "] " << what
            << ": expected exit " << expected
            << ", got " << observed
            << '\n';

        ++g_failures;
    }
}


// A skip has to be as loud as a failure. A suite that silently drops a
// case under some condition reports "6 passed" either way, and the one
// number a reader takes from it stops meaning what they think.
void skip(const char* test_name, const char* why)
{
    std::cerr << "SKIP [" << test_name << "] " << why << '\n';
    ++g_skipped;
}


struct RunResult {
    // The child exited normally, so exit_status means something. False
    // if it was killed by a signal, which the converter should never do.
    bool exited = false;

    int exit_status = -1;

    // stdout and stderr interleaved into one stream, because the tests
    // care what was said, not which descriptor said it.
    std::string output;
};


// Fork, optionally chdir, exec the converter, collect everything it
// writes, and wait for it.
//
// The working directory matters: --require-clean runs git in the child's
// cwd, so testing it means controlling that directory rather than
// inheriting the one ctest happened to start in.
RunResult run_converter(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory
)
{
    RunResult result;

    int pipe_fds[2];

    if (::pipe(pipe_fds) != 0) {
        std::cerr << "pipe() failed\n";
        return result;
    }

    const pid_t pid = ::fork();

    if (pid < 0) {
        std::cerr << "fork() failed\n";
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return result;
    }

    if (pid == 0) {
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[1]);

        if (!working_directory.empty()) {
            if (::chdir(working_directory.c_str()) != 0) {
                _exit(120);
            }
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(CONVERT_CAPTURE_BINARY));

        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }

        argv.push_back(nullptr);

        ::execv(CONVERT_CAPTURE_BINARY, argv.data());

        // Only reached if execv failed.
        _exit(121);
    }

    ::close(pipe_fds[1]);

    char buffer[4096];
    ssize_t n = 0;

    while ((n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        result.output.append(buffer, static_cast<std::size_t>(n));
    }

    ::close(pipe_fds[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        result.exited = true;
        result.exit_status = WEXITSTATUS(status);
    }

    return result;
}


bool contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}


const char* kCommit = "0123456789abcdef0123456789abcdef01234567";


// A syntactically valid book ticker line, in the capture format: capture
// timestamp, a tab, then the exchange's JSON payload.
void write_sample_log(
    const std::filesystem::path& path,
    std::size_t record_count,
    bool corrupt_last_line
)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);

    for (std::size_t i = 0; i < record_count; ++i) {
        const std::uint64_t capture_ns =
            1787231293915000000ULL + static_cast<std::uint64_t>(i) * 1000ULL;

        const std::uint64_t update_id =
            11331904350110ULL + static_cast<std::uint64_t>(i);

        const std::uint64_t event_ms =
            1787231293915ULL + static_cast<std::uint64_t>(i);

        out << capture_ns << '\t';

        if (corrupt_last_line && i + 1 == record_count) {
            out << "{\"e\":\"bookTicker\",\"u\":not_a_number}\n";
            continue;
        }

        out
            << "{\"e\":\"bookTicker\",\"u\":" << update_id
            << ",\"s\":\"BTCUSDT\",\"b\":\"72009.00\",\"B\":\"3.065\""
            << ",\"a\":\"72009.10\",\"A\":\"2.180\",\"T\":" << event_ms
            << ",\"E\":" << event_ms << "}\n";
    }
}


std::size_t count_temp_files(
    const std::filesystem::path& directory,
    const std::string& output_name
)
{
    const std::string prefix = output_name + ".";

    std::size_t found = 0;
    std::error_code error;

    std::filesystem::directory_iterator iterator(directory, error);

    if (error) {
        return 0;
    }

    const std::filesystem::directory_iterator end;

    while (iterator != end) {
        const std::string name = iterator->path().filename().string();

        if (name.starts_with(prefix) && name.ends_with(".tmp")) {
            ++found;
        }

        iterator.increment(error);

        if (error) {
            break;
        }
    }

    return found;
}


std::filesystem::path g_scratch;


std::filesystem::path fresh_directory(const char* name)
{
    const std::filesystem::path directory = g_scratch / name;

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    return directory;
}


// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

void test_valid_conversion_publishes()
{
    const char* name = "valid conversion publishes";

    const std::filesystem::path directory = fresh_directory("valid");
    const std::filesystem::path log = directory / "sample.log";
    const std::filesystem::path bin = directory / "out.bin";

    write_sample_log(log, 10, false);

    const RunResult run = run_converter(
        {log.string(), bin.string(), "BTCUSDT", kCommit, "0"},
        {}
    );

    check_exit(name, "clean conversion", run.exit_status, 0);

    check(
        name,
        std::filesystem::exists(bin),
        "the .bin was not published"
    );

    std::error_code error;
    const auto size = std::filesystem::file_size(bin, error);

    check(
        name,
        !error && size == 64 + 10 * 56,
        "published file is not 64 + 10 * 56 bytes"
    );

    check(
        name,
        count_temp_files(directory, "out.bin") == 0,
        "a temporary file was left behind"
    );

    check(
        name,
        contains(run.output, "durable: contents via"),
        "the summary did not report which durability barrier was used"
    );
}


void test_refuses_to_overwrite_existing_output()
{
    const char* name = "refuses to overwrite existing output";

    const std::filesystem::path directory = fresh_directory("overwrite");
    const std::filesystem::path log = directory / "sample.log";
    const std::filesystem::path bin = directory / "out.bin";

    write_sample_log(log, 10, false);

    const std::vector<std::string> arguments = {
        log.string(), bin.string(), "BTCUSDT", kCommit, "0"
    };

    const RunResult first = run_converter(arguments, {});
    check_exit(name, "first run", first.exit_status, 0);

    const RunResult second = run_converter(arguments, {});

    check_exit(name, "second run", second.exit_status, 1);

    check(
        name,
        contains(second.output, "refusing to overwrite existing output"),
        "the second run did not name the refuse-to-overwrite guard"
    );

    check(
        name,
        count_temp_files(directory, "out.bin") == 0,
        "the refused run left a temporary file"
    );
}


void test_malformed_input_publishes_nothing()
{
    const char* name = "malformed input publishes nothing";

    const std::filesystem::path directory = fresh_directory("malformed");
    const std::filesystem::path log = directory / "sample.log";
    const std::filesystem::path bin = directory / "out.bin";

    write_sample_log(log, 10, true);

    const RunResult run = run_converter(
        {log.string(), bin.string(), "BTCUSDT", kCommit, "0"},
        {}
    );

    check_exit(name, "malformed input", run.exit_status, 1);

    check(
        name,
        !std::filesystem::exists(bin),
        "a .bin was published from a malformed input"
    );

    check(
        name,
        count_temp_files(directory, "out.bin") == 0,
        "the failed run left a temporary file"
    );
}


// Mode 300 grants write and execute without read: the .tmp can be
// created and path resolution still works, but open(dir, O_RDONLY)
// fails. Root bypasses the check entirely, so under root this is skipped
// rather than passed.
void test_unopenable_output_directory()
{
    const char* name = "unopenable output directory";

    if (::geteuid() == 0) {
        skip(name, "running as root, which bypasses directory permissions");
        return;
    }

    const std::filesystem::path directory = fresh_directory("noread");
    const std::filesystem::path log = directory / "sample.log";
    const std::filesystem::path bin = directory / "out.bin";

    write_sample_log(log, 10, false);

    if (::chmod(directory.c_str(), 0300) != 0) {
        skip(name, "chmod 300 on the scratch directory failed");
        return;
    }

    const RunResult run = run_converter(
        {log.string(), bin.string(), "BTCUSDT", kCommit, "0"},
        {}
    );

    ::chmod(directory.c_str(), 0755);

    check_exit(name, "unreadable directory", run.exit_status, 1);

    check(
        name,
        contains(run.output, "could not open the output directory"),
        "the directory-open guard did not fire"
    );

    check(
        name,
        !std::filesystem::exists(bin),
        "a .bin was published despite the directory being unopenable"
    );

    check(
        name,
        count_temp_files(directory, "out.bin") == 0,
        "the failed run left a temporary file"
    );
}


// The orphan scan is the one check in the converter that warns rather
// than aborting, so the assertion here is as much about the exit status
// as about the message.
void test_orphan_temp_files_warn_without_failing()
{
    const char* name = "orphan temporary files warn without failing";

    const std::filesystem::path directory = fresh_directory("orphans");
    const std::filesystem::path log = directory / "sample.log";
    const std::filesystem::path bin = directory / "out.bin";

    write_sample_log(log, 10, false);

    {
        std::ofstream a(directory / "out.bin.99999.tmp");
        std::ofstream b(directory / "out.bin.tmp");
    }

    const RunResult run = run_converter(
        {log.string(), bin.string(), "BTCUSDT", kCommit, "0"},
        {}
    );

    check_exit(name, "orphans present", run.exit_status, 0);

    check(
        name,
        contains(run.output, "out.bin.99999.tmp"),
        "the pid-tagged orphan was not reported"
    );

    check(
        name,
        contains(run.output, "out.bin.tmp"),
        "the old-format orphan was not reported"
    );

    check(
        name,
        std::filesystem::exists(bin),
        "the run did not publish despite the orphans being a warning"
    );
}


// The exclusive publish. A large enough input that every child gets past
// the startup refuse-to-overwrite check, so the contention lands on the
// publish rather than on process startup.
//
// The assertion is the invariant — exactly one publication — not the
// split between the two guards that produce it. Which guard catches a
// given child depends on scheduling, and a test that asserted a split
// would be a test that fails on a fast morning.
void test_concurrent_runs_publish_exactly_once()
{
    const char* name = "concurrent runs publish exactly once";

    const std::filesystem::path directory = fresh_directory("concurrent");
    const std::filesystem::path log = directory / "sample.log";
    const std::filesystem::path bin = directory / "out.bin";

    write_sample_log(log, 200000, false);

    const std::size_t children = 8;

    std::vector<pid_t> pids;
    pids.reserve(children);

    for (std::size_t i = 0; i < children; ++i) {
        const pid_t pid = ::fork();

        if (pid < 0) {
            check(name, false, "fork() failed");
            break;
        }

        if (pid == 0) {
            const int null_fd = ::open("/dev/null", O_WRONLY);

            if (null_fd >= 0) {
                ::dup2(null_fd, STDOUT_FILENO);
                ::dup2(null_fd, STDERR_FILENO);
                ::close(null_fd);
            }

            const std::string log_string = log.string();
            const std::string bin_string = bin.string();

            char* argv[] = {
                const_cast<char*>(CONVERT_CAPTURE_BINARY),
                const_cast<char*>(log_string.c_str()),
                const_cast<char*>(bin_string.c_str()),
                const_cast<char*>("BTCUSDT"),
                const_cast<char*>(kCommit),
                const_cast<char*>("0"),
                nullptr
            };

            ::execv(CONVERT_CAPTURE_BINARY, argv);
            _exit(121);
        }

        pids.push_back(pid);
    }

    std::size_t published = 0;
    std::size_t refused = 0;

    for (const pid_t pid : pids) {
        int status = 0;
        ::waitpid(pid, &status, 0);

        if (!WIFEXITED(status)) {
            check(name, false, "a child did not exit normally");
            continue;
        }

        if (WEXITSTATUS(status) == 0) {
            ++published;
        } else if (WEXITSTATUS(status) == 1) {
            ++refused;
        } else {
            check(name, false, "a child exited with an unexpected status");
        }
    }

    if (published != 1) {
        std::cerr
            << "FAIL [" << name << "] expected exactly one publication, got "
            << published
            << " (refused " << refused << ")\n";

        ++g_failures;
    }

    check(
        name,
        std::filesystem::exists(bin),
        "no .bin exists after the concurrent runs"
    );

    check(
        name,
        count_temp_files(directory, "out.bin") == 0,
        "a temporary file survived the concurrent runs"
    );
}


// --require-clean needs a repository to inspect, so these build a throw
// away one rather than inspecting the repository the suite happens to be
// running inside — which may itself be dirty, and whose state is not this
// suite's business.
bool make_scratch_repository(const std::filesystem::path& directory)
{
    const std::string quoted = "\"" + directory.string() + "\"";

    const std::string command =
        "cd " + quoted + " && "
        "git init -q . >/dev/null 2>&1 && "
        "git -c user.email=t@example.com -c user.name=t "
        "commit -q --allow-empty -m base >/dev/null 2>&1";

    return std::system(command.c_str()) == 0;
}


std::string scratch_repository_head(const std::filesystem::path& directory)
{
    const std::string quoted = "\"" + directory.string() + "\"";

    const std::string command =
        "cd " + quoted + " && git rev-parse HEAD > head.txt 2>/dev/null";

    if (std::system(command.c_str()) != 0) {
        return {};
    }

    std::ifstream in(directory / "head.txt");
    std::string head;
    in >> head;

    std::error_code error;
    std::filesystem::remove(directory / "head.txt", error);

    return head;
}


void test_require_clean()
{
    const char* name = "--require-clean";

    const std::filesystem::path repository = fresh_directory("repo");

    if (!make_scratch_repository(repository)) {
        skip(name, "could not create a scratch git repository");
        return;
    }

    const std::string head = scratch_repository_head(repository);

    if (head.size() != 40) {
        skip(name, "could not resolve HEAD in the scratch repository");
        return;
    }

    const std::filesystem::path log = repository / "sample.log";
    write_sample_log(log, 10, false);

    // sample.log is untracked, so the tree is unclean from here on. That
    // is the first case; it has to run before the file is committed.
    {
        const RunResult run = run_converter(
            {
                log.string(),
                (repository / "unclean.bin").string(),
                "BTCUSDT",
                head,
                "0",
                "--require-clean"
            },
            repository
        );

        check_exit(name, "unclean tree", run.exit_status, 1);

        check(
            name,
            contains(run.output, "working tree is not clean"),
            "the unclean-tree guard did not fire"
        );

        check(
            name,
            !std::filesystem::exists(repository / "unclean.bin"),
            "a .bin was published from an unclean tree"
        );
    }

    // Commit the log so the tree is clean, and produce the outputs
    // somewhere else so they do not dirty it again.
    const std::string commit_command =
        "cd \"" + repository.string() + "\" && "
        "git add sample.log >/dev/null 2>&1 && "
        "git -c user.email=t@example.com -c user.name=t "
        "commit -q -m sample >/dev/null 2>&1";

    if (std::system(commit_command.c_str()) != 0) {
        skip(name, "could not commit the sample log in the scratch repository");
        return;
    }

    const std::string clean_head = scratch_repository_head(repository);

    if (clean_head.size() != 40) {
        skip(name, "could not resolve HEAD after committing");
        return;
    }

    const std::filesystem::path outputs = fresh_directory("repo_out");

    {
        const RunResult run = run_converter(
            {
                log.string(),
                (outputs / "clean.bin").string(),
                "BTCUSDT",
                clean_head,
                "0",
                "--require-clean"
            },
            repository
        );

        check_exit(name, "clean tree", run.exit_status, 0);

        check(
            name,
            contains(run.output, "tree:    verified clean"),
            "the summary did not report the tree as verified"
        );
    }

    {
        const RunResult run = run_converter(
            {
                log.string(),
                (outputs / "wrongsha.bin").string(),
                "BTCUSDT",
                kCommit,
                "0",
                "--require-clean"
            },
            repository
        );

        check_exit(name, "commit argument is not HEAD", run.exit_status, 1);

        check(
            name,
            contains(run.output, "is not HEAD"),
            "the HEAD-mismatch guard did not fire"
        );
    }

    {
        const RunResult run = run_converter(
            {
                log.string(),
                (outputs / "contradiction.bin").string(),
                "BTCUSDT",
                clean_head,
                "1",
                "--require-clean"
            },
            repository
        );

        check_exit(name, "dirty flag contradiction", run.exit_status, 1);

        check(
            name,
            contains(run.output, "the dirty flag argument is 1"),
            "the contradiction guard did not fire"
        );
    }

    {
        // outputs is a plain directory, not a repository.
        const RunResult run = run_converter(
            {
                log.string(),
                (outputs / "norepo.bin").string(),
                "BTCUSDT",
                clean_head,
                "0",
                "--require-clean"
            },
            outputs
        );

        check_exit(name, "outside a repository", run.exit_status, 1);

        check(
            name,
            contains(run.output, "HEAD could not be resolved"),
            "the not-a-repository guard did not fire"
        );
    }

    {
        const RunResult run = run_converter(
            {
                log.string(),
                (outputs / "typo.bin").string(),
                "BTCUSDT",
                clean_head,
                "0",
                "--require-clen"
            },
            repository
        );

        check_exit(name, "misspelled flag", run.exit_status, 1);

        check(
            name,
            contains(run.output, "unrecognised argument"),
            "a misspelled flag was not rejected"
        );

        check(
            name,
            !std::filesystem::exists(outputs / "typo.bin"),
            "a misspelled flag was silently ignored and the run published"
        );
    }
}

} // namespace


int main()
{
    // If the compile definition is wrong the failure would otherwise
    // surface as every child exiting 121 from a failed execv, which
    // reads like a converter defect. Say what it actually is.
    if (!std::filesystem::exists(CONVERT_CAPTURE_BINARY)) {
        std::cerr
            << "the converter binary is not at "
            << CONVERT_CAPTURE_BINARY
            << '\n'
            << "  CONVERT_CAPTURE_BINARY comes from the"
            << " target_compile_definitions line in CMakeLists.txt\n";

        return 1;
    }

    std::error_code error;

    g_scratch =
        std::filesystem::temp_directory_path(error) /
        ("convert_capture_test." + std::to_string(::getpid()));

    if (error) {
        std::cerr << "could not locate a temporary directory\n";
        return 1;
    }

    std::filesystem::create_directories(g_scratch, error);

    if (error) {
        std::cerr
            << "could not create the scratch directory: "
            << g_scratch
            << '\n';

        return 1;
    }

    test_valid_conversion_publishes();
    test_refuses_to_overwrite_existing_output();
    test_malformed_input_publishes_nothing();
    test_unopenable_output_directory();
    test_orphan_temp_files_warn_without_failing();
    test_concurrent_runs_publish_exactly_once();
    test_require_clean();

    std::filesystem::remove_all(g_scratch, error);

    if (g_skipped != 0) {
        std::cerr
            << "convert_capture tests: "
            << g_skipped
            << " case(s) skipped — see SKIP lines above\n";
    }

    if (g_failures != 0) {
        std::cerr
            << "convert_capture tests FAILED: "
            << g_failures
            << " check(s) failed\n";

        return 1;
    }

    std::cout << "convert capture tests passed\n";

    return 0;
}