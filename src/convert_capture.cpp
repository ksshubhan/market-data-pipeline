#include "capture_file.hpp"
#include "parser.hpp"
#include "record.hpp"

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>


static_assert(sizeof(BinaryHeader) == 64);
static_assert(sizeof(CaptureRecord) == 56);
static_assert(kCaptureRecordSize == sizeof(CaptureRecord));


namespace {

const char* parse_error_name(ParseError error) noexcept
{
    switch (error) {
    case ParseError::none:
        return "none";
    case ParseError::empty_input:
        return "empty_input";
    case ParseError::invalid_character:
        return "invalid_character";
    case ParseError::multiple_decimal_points:
        return "multiple_decimal_points";
    case ParseError::missing_integer_digits:
        return "missing_integer_digits";
    case ParseError::missing_fractional_digits:
        return "missing_fractional_digits";
    case ParseError::too_many_fractional_digits:
        return "too_many_fractional_digits";
    case ParseError::overflow:
        return "overflow";
    case ParseError::malformed_json:
        return "malformed_json";
    case ParseError::missing_required_field:
        return "missing_required_field";
    case ParseError::wrong_field_type:
        return "wrong_field_type";
    case ParseError::wrong_event_type:
        return "wrong_event_type";
    case ParseError::symbol_mismatch:
        return "symbol_mismatch";
    }

    return "unknown_parse_error";
}


bool parse_uint64(
    std::string_view text,
    std::uint64_t& output
) noexcept
{
    if (text.empty()) {
        return false;
    }

    const char* const begin = text.data();
    const char* const end = text.data() + text.size();

    const auto result = std::from_chars(
        begin,
        end,
        output
    );

    return result.ec == std::errc{} && result.ptr == end;
}


int hex_digit_value(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }

    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }

    return -1;
}


bool parse_git_commit(
    std::string_view hex,
    std::uint8_t (&output)[20]
) noexcept
{
    if (hex.size() != 40) {
        return false;
    }

    for (std::size_t i = 0; i < 20; ++i) {
        const int high = hex_digit_value(hex[i * 2]);
        const int low = hex_digit_value(hex[i * 2 + 1]);

        if (high < 0 || low < 0) {
            return false;
        }

        output[i] = static_cast<std::uint8_t>(
            (high << 4) | low
        );
    }

    return true;
}


bool write_bytes(
    std::ofstream& output,
    const void* data,
    std::size_t size
)
{
    output.write(
        static_cast<const char*>(data),
        static_cast<std::streamsize>(size)
    );

    return static_cast<bool>(output);
}


// Byte offset of the first difference between two objects of the same
// size, or size when they are identical. It exists only so that a header
// read-back failure is diagnosable: "the header does not match" tells
// you nothing, "first differing byte at offset 8" points straight at
// record_count and offset 16 points at a seek that landed two bytes
// late.
std::size_t first_differing_byte(
    const void* left,
    const void* right,
    std::size_t size
) noexcept
{
    const auto* left_bytes =
        static_cast<const unsigned char*>(left);

    const auto* right_bytes =
        static_cast<const unsigned char*>(right);

    for (std::size_t i = 0; i < size; ++i) {
        if (left_bytes[i] != right_bytes[i]) {
            return i;
        }
    }

    return size;
}


void remove_temp_file(
    const std::filesystem::path& temp_path
) noexcept
{
    std::error_code error;
    std::filesystem::remove(temp_path, error);
}


// Report .tmp files left in the output directory by earlier runs
// targeting this same output.
//
// Before the pid went into the name there was exactly one temporary
// name per output, and the refuse-to-start check below doubled as the
// thing that made an orphan impossible to ignore: the next run stopped
// dead and named the file. With the pid in the name that check will
// almost never fire again, so an orphan from a crashed run would sit in
// the directory indefinitely with nothing to mention it.
//
// This warns rather than aborts, which is a deliberate exception to this
// project's abort-rather-than-warn rule. Aborting would rebuild exactly
// the cross-process coupling the pid was added to remove: a second
// converter running concurrently against the same output has a live
// .tmp in this directory, and that is not an error. A stale orphan and a
// live sibling are indistinguishable from the filename alone, and the
// pid in it is not a reliable way to tell them apart either — pids are
// reused, and a matching live process need not be this program.
//
// The match is deliberately loose enough to catch <output>.tmp as well
// as <output>.<pid>.tmp, so a temporary left behind by a converter built
// before this change is still reported.
void report_orphan_temp_files(
    const std::filesystem::path& output_path,
    const std::filesystem::path& directory_path
)
{
    const std::string prefix =
        output_path.filename().string() + ".";

    std::error_code error;

    std::filesystem::directory_iterator iterator(
        directory_path,
        error
    );

    if (error) {
        return;
    }

    const std::filesystem::directory_iterator end;

    std::size_t found = 0;

    while (iterator != end) {
        const std::string name =
            iterator->path().filename().string();

        if (name.starts_with(prefix) && name.ends_with(".tmp")) {
            if (found == 0) {
                std::cerr
                    << "warning: temporary files for this output are"
                    << " already present in "
                    << directory_path
                    << '\n';
            }

            std::cerr << "  " << name << '\n';
            ++found;
        }

        iterator.increment(error);

        if (error) {
            break;
        }
    }

    if (found != 0) {
        std::cerr
            << "  each is either a converter running right now or the"
            << " wreckage of one that died\n"
            << "  this run is unaffected — it writes to its own"
            << " pid-tagged file — but nothing will clean these up\n";
    }
}


enum class PublishResult {
    published,
    already_exists,
    failed,
};


// Publish the temporary file under its final name, refusing to replace
// anything already at that name.
//
// std::filesystem::rename replaces unconditionally, and the
// refuse-to-overwrite check near the top of main runs long before the
// publish — hundreds of milliseconds on a real dataset — so two
// converters given the same output path both pass it before either has
// written anything, and both then rename, the later silently destroying
// the earlier. Measured on forty concurrent runs against one output:
// twelve published, eleven finished datasets were overwritten, and every
// one of the twelve exited zero.
//
// link(2) is the exclusivity primitive here. It fails with EEXIST if the
// destination exists, it is atomic, and unlike renamex_np(RENAME_EXCL)
// and renameat2(RENAME_NOREPLACE) it is POSIX and does not need a
// different call on each platform. The cost is that the publish is two
// steps rather than one, so between them the same inode is reachable
// under both names. That is not observable as damage: the contents at
// both names are the same verified, fsynced bytes, and the temporary
// name is removed immediately after.
//
// There is no fallback to an overwriting rename when link fails, which
// is the opposite resolution to the F_FULLFSYNC fallback above, and
// deliberately so. Downgrading a durability barrier still publishes
// correct data under everything short of power loss. Downgrading an
// exclusivity guarantee destroys a file another process just wrote.
PublishResult publish_exclusively(
    const std::filesystem::path& temp_path,
    const std::filesystem::path& output_path,
    int& error_out
) noexcept
{
    int result = 0;

    do {
        result = ::link(temp_path.c_str(), output_path.c_str());
    } while (result != 0 && errno == EINTR);

    if (result == 0) {
        return PublishResult::published;
    }

    error_out = errno;

    if (error_out == EEXIST) {
        return PublishResult::already_exists;
    }

    return PublishResult::failed;
}


// Owns a descriptor for the rest of a scope. main() leaves through
// thirty-odd returns and the descriptor opened for the directory has to
// stay open across the publish, so the alternative is a close on every
// exit path and a leak on the one that gets missed.
//
// The destructor discards close()'s result deliberately. A close can
// only report a lost write, and neither descriptor this class holds is
// ever written through: the directory is opened O_RDONLY, and the
// contents descriptor is opened only so that fsync has something to act
// on. Every byte was written and flushed by the ofstream, which was
// closed and checked long before either of these exists.
class ScopedFd {
public:
    explicit ScopedFd(int fd) noexcept
        : fd_(fd)
    {
    }

    ~ScopedFd()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&&) = delete;
    ScopedFd& operator=(ScopedFd&&) = delete;

    int get() const noexcept
    {
        return fd_;
    }

    bool valid() const noexcept
    {
        return fd_ >= 0;
    }

private:
    int fd_;
};


// EINTR on open(2) against a regular file or a directory on a local
// filesystem is not something either target platform will produce. The
// loop is three lines and its absence is the kind of thing that fails
// once, in the field, on a filesystem nobody tested against.
int open_retrying(const char* path, int flags) noexcept
{
    int fd = -1;

    do {
        fd = ::open(path, flags);
    } while (fd < 0 && errno == EINTR);

    return fd;
}


enum class SyncBarrier {
    full_device_flush,
    fsync_only,
};


const char* barrier_name(SyncBarrier barrier) noexcept
{
    switch (barrier) {
    case SyncBarrier::full_device_flush:
        return "F_FULLFSYNC";
    case SyncBarrier::fsync_only:
        return "fsync";
    }

    return "unknown";
}


struct SyncOutcome {
    bool ok = false;
    SyncBarrier barrier = SyncBarrier::fsync_only;

    // errno from the call that failed, meaningful only when !ok.
    int error = 0;

    // Darwin only, and not a failure: the errno that made F_FULLFSYNC
    // unusable on this filesystem, when the weaker barrier was used in
    // its place. Zero when no downgrade happened.
    int fallback_error = 0;
};


// Force a descriptor's file to stable storage.
//
// On Darwin fsync(2) is not the barrier its name suggests. It writes the
// file's dirty pages out to the drive and returns; the drive is free to
// be holding them in its own volatile write cache, which a power loss
// empties. Apple documents F_FULLFSYNC as the call that additionally
// asks the drive to flush that cache, and F_BARRIERFSYNC as a middle
// option that orders this write against later ones without waiting for
// the platter. F_BARRIERFSYNC is the wrong choice here: ordering the
// data before the publish is exactly what a barrier gives, but the
// converter has no later write to be ordered against — it exits — so
// what is wanted is the stronger completion guarantee, not the cheaper
// ordering one.
//
// F_FULLFSYNC is not universally supported; network and some virtual
// filesystems reject it. A rejection falls back to fsync and records the
// errno so the caller can say which barrier it actually got rather than
// claiming the stronger one. Any errno outside the not-supported set is
// a real I/O failure and must not be quietly downgraded into a weaker
// barrier — that would turn a hardware error into a success.
SyncOutcome sync_descriptor(int fd) noexcept
{
    SyncOutcome outcome;

#if defined(__APPLE__)
    int full_result = 0;

    do {
        full_result = ::fcntl(fd, F_FULLFSYNC, 0);
    } while (full_result < 0 && errno == EINTR);

    if (full_result == 0) {
        outcome.ok = true;
        outcome.barrier = SyncBarrier::full_device_flush;
        return outcome;
    }

    const int full_errno = errno;

    // ENOTSUP and EOPNOTSUPP are the same value on Darwin. Both are
    // named because which spelling a filesystem's implementation
    // reaches for is not something to depend on.
    if (full_errno != ENOTSUP &&
        full_errno != EOPNOTSUPP &&
        full_errno != EINVAL &&
        full_errno != ENOTTY) {
        outcome.error = full_errno;
        return outcome;
    }

    outcome.fallback_error = full_errno;
#endif

    int fsync_result = 0;

    do {
        fsync_result = ::fsync(fd);
    } while (fsync_result < 0 && errno == EINTR);

    if (fsync_result != 0) {
        outcome.error = errno;
        return outcome;
    }

    outcome.ok = true;
    outcome.barrier = SyncBarrier::fsync_only;
    return outcome;
}

} // namespace


int main(int argc, char* argv[])
{
    if (argc != 6) {
        std::cerr
            << "Usage:\n"
            << "  " << argv[0]
            << " <input.log> <output.bin> <symbol>"
            << " <git-commit-40-hex> <dirty:0|1>\n";

        return 1;
    }

    const std::filesystem::path input_path = argv[1];
    const std::filesystem::path output_path = argv[2];
    const std::string symbol = argv[3];
    const std::string git_commit_hex = argv[4];
    const std::string dirty_text = argv[5];

    if (symbol.empty() || symbol.size() > 15) {
        std::cerr
            << "error: symbol must contain between 1 and 15 characters\n";
        return 1;
    }

    bool dirty = false;

    if (dirty_text == "0") {
        dirty = false;
    } else if (dirty_text == "1") {
        dirty = true;
    } else {
        std::cerr
            << "error: dirty flag must be either 0 or 1\n";
        return 1;
    }

    BinaryHeader header{};

    std::memcpy(
        header.magic,
        kCaptureMagic,
        sizeof(kCaptureMagic)
    );

    header.record_count = kUnfinalizedRecordCount;
    header.format_version = kCaptureFormatVersion;
    header.header_size = kCaptureHeaderSize;
    header.record_size = kCaptureRecordSize;
    header.scale_exponent = kCaptureScaleExponent;

    header.market_type =
        static_cast<std::uint8_t>(MarketType::futures);

    header.stream_type =
        static_cast<std::uint8_t>(StreamType::book_ticker);

    header.flags =
        dirty ? kHeaderFlagDirtyBuild : 0;

    std::memcpy(
        header.symbol,
        symbol.data(),
        symbol.size()
    );

    if (!parse_git_commit(
            git_commit_hex,
            header.git_commit
        )) {
        std::cerr
            << "error: git commit must be exactly 40 hexadecimal characters\n";
        return 1;
    }

    std::error_code filesystem_error;

    if (!std::filesystem::exists(
            input_path,
            filesystem_error
        )) {
        std::cerr
            << "error: input file does not exist: "
            << input_path << '\n';
        return 1;
    }

    if (filesystem_error) {
        std::cerr
            << "error: could not inspect input file: "
            << filesystem_error.message() << '\n';
        return 1;
    }

    if (std::filesystem::exists(
            output_path,
            filesystem_error
        )) {
        std::cerr
            << "error: refusing to overwrite existing output file: "
            << output_path << '\n';
        return 1;
    }

    if (filesystem_error) {
        std::cerr
            << "error: could not inspect output path: "
            << filesystem_error.message() << '\n';
        return 1;
    }

    // Hoisted here from the durability block at the end of main because
    // the orphan scan below needs it too. Empty parent_path means the
    // output is a bare filename in the working directory.
    std::filesystem::path directory_path = output_path.parent_path();

    if (directory_path.empty()) {
        directory_path = ".";
    }

    // §7.6: the pid goes in the temporary name so that two converters
    // given the same output path cannot write into one another's
    // temporary file.
    //
    // What this buys on its own is narrow, and measurement rather than
    // reasoning settled how narrow. The interleaved-writers failure it
    // targets needs both processes past the check below before either
    // opens its stream, a window a few instructions wide: forty
    // concurrent runs against one output, and two more released together
    // through a FIFO on a 486 MB input, never once hit it. What happened
    // instead every time was that the loser found the temporary file
    // already there and stopped.
    //
    // So the shared temporary name was doing duty as a mutex, and giving
    // each process its own name removes it. That is only safe because
    // the publish below is exclusive. Without it this change converts a
    // loud refusal into a silent overwrite — measured at twelve
    // publications out of forty where the shared name gave one.
    //
    // This does not touch §7.6's determinism rule. That rule forbids
    // non-reproducible values in header fields, because they would break
    // the byte-identical comparison of two conversions of one input. The
    // temporary filename is not in the file.
    std::filesystem::path temp_path = output_path;

    temp_path +=
        "." +
        std::to_string(static_cast<long long>(::getpid())) +
        ".tmp";

    report_orphan_temp_files(output_path, directory_path);

    if (std::filesystem::exists(
            temp_path,
            filesystem_error
        )) {
        std::cerr
            << "error: temporary file already exists: "
            << temp_path << '\n'
            << "The name carries this process's pid, so this is not a"
            << " concurrent run — it is the wreckage of an earlier one"
            << " that held the same pid.\n"
            << "Remove or inspect it before retrying.\n";
        return 1;
    }

    if (filesystem_error) {
        std::cerr
            << "error: could not inspect temporary path: "
            << filesystem_error.message() << '\n';
        return 1;
    }

    std::ifstream input(
        input_path,
        std::ios::in | std::ios::binary
    );

    if (!input) {
        std::cerr
            << "error: could not open input file: "
            << input_path << '\n';
        return 1;
    }

    std::ofstream output(
        temp_path,
        std::ios::out |
        std::ios::binary |
        std::ios::trunc
    );

    if (!output) {
        std::cerr
            << "error: could not create temporary output file: "
            << temp_path << '\n';
        return 1;
    }

    if (!write_bytes(
            output,
            &header,
            sizeof(header)
        )) {
        std::cerr << "error: failed to write binary header\n";
        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    std::string line;
    std::uint64_t line_number = 0;
    std::uint64_t record_count = 0;

    while (std::getline(input, line)) {
        ++line_number;

        const std::size_t tab_position =
            line.find('\t');

        if (tab_position == std::string::npos) {
            std::cerr
                << "error: line "
                << line_number
                << " does not contain the expected tab separator\n";

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        const std::string_view timestamp_text(
            line.data(),
            tab_position
        );

        const std::string_view json(
            line.data() + tab_position + 1,
            line.size() - tab_position - 1
        );

        std::uint64_t capture_wall_time_ns = 0;

        if (!parse_uint64(
                timestamp_text,
                capture_wall_time_ns
            )) {
            std::cerr
                << "error: invalid capture timestamp on line "
                << line_number << '\n';

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        CaptureRecord record{};

        const ParseError parse_error =
            parse_book_ticker(
                json,
                capture_wall_time_ns,
                symbol,
                record
            );

        if (parse_error != ParseError::none) {
            std::cerr
                << "error: parser failure on line "
                << line_number
                << ": "
                << parse_error_name(parse_error)
                << '\n';

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        if (!write_bytes(
                output,
                &record,
                sizeof(record)
            )) {
            std::cerr
                << "error: failed writing record from line "
                << line_number << '\n';

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        if (record_count ==
            std::numeric_limits<std::uint64_t>::max()) {
            std::cerr
                << "error: record count overflow\n";

            output.close();
            remove_temp_file(temp_path);
            return 1;
        }

        ++record_count;
    }

    if (input.bad()) {
        std::cerr
            << "error: I/O failure while reading input file\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    output.seekp(
        static_cast<std::streamoff>(
            offsetof(BinaryHeader, record_count)
        ),
        std::ios::beg
    );

    if (!output) {
        std::cerr
            << "error: failed to seek back to record_count\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    if (!write_bytes(
            output,
            &record_count,
            sizeof(record_count)
        )) {
        std::cerr
            << "error: failed to finalize record_count\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    output.flush();

    if (!output) {
        std::cerr
            << "error: failed to flush output file\n";

        output.close();
        remove_temp_file(temp_path);
        return 1;
    }

    output.close();

    if (!output) {
        std::cerr
            << "error: failed to close output file cleanly\n";

        remove_temp_file(temp_path);
        return 1;
    }

    constexpr std::uint64_t header_size =
        sizeof(BinaryHeader);

    constexpr std::uint64_t record_size =
        sizeof(CaptureRecord);

    if (
        record_count >
        (
            std::numeric_limits<std::uint64_t>::max()
            - header_size
        ) / record_size
    ) {
        std::cerr
            << "error: expected file size would overflow uint64_t\n";

        remove_temp_file(temp_path);
        return 1;
    }

    const std::uint64_t expected_file_size =
        header_size + record_count * record_size;

    const std::uintmax_t actual_file_size =
        std::filesystem::file_size(
            temp_path,
            filesystem_error
        );

    if (filesystem_error) {
        std::cerr
            << "error: could not determine temporary file size: "
            << filesystem_error.message()
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    if (actual_file_size != expected_file_size) {
        std::cerr
            << "error: binary size validation failed\n"
            << "  expected: "
            << expected_file_size
            << " bytes\n"
            << "  actual:   "
            << actual_file_size
            << " bytes\n";

        remove_temp_file(temp_path);
        return 1;
    }

    // -----------------------------------------------------------------
    // Header read-back — §7.6
    // -----------------------------------------------------------------
    //
    // record_count is the only field written by seeking backwards over
    // an already-written file rather than by appending, and three ways
    // that can go wrong leave the file's length untouched: a short
    // write, a seek to the wrong offset, and a buffer that never reached
    // the filesystem. The size check above passes in all three cases, so
    // it cannot stand in for this one.
    //
    // The read goes through a new handle, opened after the write stream
    // was closed. Re-reading through the stream that did the writing
    // would only confirm that its own buffer holds what was put into it,
    // and the buffer is not the layer that would have lost the bytes.
    //
    // The comparison covers all 64 bytes rather than record_count alone.
    // A seek landing at the wrong offset writes eight bytes of count
    // over whatever fields are there, so the damage is not confined to
    // the field being patched. BinaryHeader has no padding — all 64
    // bytes belong to a member, and every offset is statically asserted
    // in capture_file.hpp — so memcmp over it is well defined and
    // returns the same answer on every run.

    BinaryHeader expected_header = header;
    expected_header.record_count = record_count;

    BinaryHeader stored_header{};

    {
        std::ifstream readback(
            temp_path,
            std::ios::in | std::ios::binary
        );

        if (!readback) {
            std::cerr
                << "error: could not reopen the temporary file to"
                << " verify its header\n";

            remove_temp_file(temp_path);
            return 1;
        }

        readback.read(
            reinterpret_cast<char*>(&stored_header),
            static_cast<std::streamsize>(sizeof(stored_header))
        );

        // gcount, not the stream state: reading exactly 64 bytes from a
        // much longer file leaves the stream good, so its state says
        // nothing about how much came back.
        if (readback.gcount() !=
            static_cast<std::streamsize>(sizeof(stored_header))) {
            std::cerr
                << "error: short read while verifying the header\n"
                << "  expected: "
                << sizeof(stored_header)
                << " bytes\n"
                << "  read:     "
                << readback.gcount()
                << " bytes\n";

            remove_temp_file(temp_path);
            return 1;
        }
    }

    if (stored_header.record_count == kUnfinalizedRecordCount) {
        std::cerr
            << "error: the poison record_count is still on disk\n"
            << "  the seek-back-and-patch did not reach the file\n";

        remove_temp_file(temp_path);
        return 1;
    }

    if (stored_header.record_count != record_count) {
        std::cerr
            << "error: the stored record_count disagrees with the"
            << " counter\n"
            << "  counter: "
            << record_count
            << '\n'
            << "  on disk: "
            << stored_header.record_count
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    if (actual_file_size < header_size) {
        std::cerr
            << "error: the temporary file is shorter than its own"
            << " header\n";

        remove_temp_file(temp_path);
        return 1;
    }

    // §7.6 asks for the stored count to be checked against the file
    // length as well as against the counter. Given the two checks above
    // and the size check before them this cannot currently fire — it is
    // the third side of a triangle whose other two sides have already
    // been walked. It is written anyway because it is the constraint
    // §7.6 names, and because it ties the stored field to the file
    // length directly rather than through the in-memory counter, so it
    // still holds if the size check above is ever changed or moved.
    const std::uint64_t implied_record_count =
        (static_cast<std::uint64_t>(actual_file_size) - header_size)
        / record_size;

    if (stored_header.record_count != implied_record_count) {
        std::cerr
            << "error: the stored record_count disagrees with the file"
            << " length\n"
            << "  on disk:      "
            << stored_header.record_count
            << '\n'
            << "  file implies: "
            << implied_record_count
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    if (std::memcmp(
            &stored_header,
            &expected_header,
            sizeof(BinaryHeader)
        ) != 0) {
        const std::size_t offset = first_differing_byte(
            &stored_header,
            &expected_header,
            sizeof(BinaryHeader)
        );

        std::cerr
            << "error: the stored header does not match the header that"
            << " was written\n"
            << "  first differing byte at offset "
            << offset
            << '\n'
            << "  record_count already verified, so the damage is"
            << " elsewhere in the header\n";

        remove_temp_file(temp_path);
        return 1;
    }

    // -----------------------------------------------------------------
    // Durability — §7.6
    // -----------------------------------------------------------------
    //
    // Everything above this point establishes that the bytes in the page
    // cache are the bytes that were meant to be there. None of it says
    // the bytes are on the device. Publishing a name is atomic with
    // respect to the directory entry, not with respect to the file's
    // contents: the metadata operation can reach stable storage while
    // the data pages are still dirty, and a power loss in that window
    // leaves a correctly-named .bin holding whatever those blocks held
    // before. The size check and the header read-back both pass on that
    // file when it is next opened, because they read back through a
    // cache that has not lost anything — the loss happens below them.
    //
    // Order: open the directory, sync the contents, publish, sync the
    // directory. The directory is opened before the publish rather than
    // immediately before the sync that needs it because an open failure
    // is recoverable — the .tmp is still deletable and nothing has been
    // published — whereas the identical failure discovered after the
    // publish is not. Everything that can fail cleanly is made to fail
    // before the point of no return.
    //
    // One directory fsync covers both halves of the publish. link and
    // unlink are two modifications of the same directory, and the sync
    // is issued after both, so the entry that was created and the entry
    // that was removed are made durable together.
    //
    // The sync of the contents is placed after the read-back rather than
    // before it so that a file the read-back is about to reject is not
    // pushed to the device first. On the 734 MiB BTC dataset that
    // ordering is worth real time; it changes nothing about what either
    // check proves.

    // directory_path was computed near the top of main, alongside the
    // temporary name that the orphan scan needed it for.

    // A directory can only be opened O_RDONLY — O_WRONLY on a directory
    // is EISDIR. The contents descriptor below is opened O_WRONLY
    // instead, because POSIX permits fsync to fail with EBADF on a
    // descriptor that is not open for writing. Darwin and Linux both
    // accept a read-only descriptor, but the standard does not require
    // it and asking for write access on a file this process created
    // costs nothing.
    const ScopedFd directory_fd(
        open_retrying(directory_path.c_str(), O_RDONLY)
    );

    if (!directory_fd.valid()) {
        // errno is read before anything is written to cerr. Stream
        // insertion is a library call and is entitled to set errno, and
        // in `a << b << strerror(errno)` the left-hand insertions are
        // sequenced before the argument is evaluated, so reading it
        // inline would report whatever iostreams last did.
        const int open_errno = errno;

        std::cerr
            << "error: could not open the output directory to make the"
            << " publish durable: "
            << directory_path
            << ": "
            << std::strerror(open_errno)
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    SyncBarrier contents_barrier = SyncBarrier::fsync_only;

    {
        // fsync acts on the file, not on the descriptor it is handed, so
        // a descriptor opened now still forces out the pages the
        // now-closed ofstream left dirty. This is not a workaround for
        // having discarded the write handle; it is the same operation on
        // the same vnode. Doing it through the ofstream would not have
        // been possible anyway — the standard library exposes no
        // descriptor.
        const ScopedFd contents_fd(
            open_retrying(temp_path.c_str(), O_WRONLY)
        );

        if (!contents_fd.valid()) {
            const int open_errno = errno;

            std::cerr
                << "error: could not reopen the temporary file to"
                << " synchronise it: "
                << temp_path
                << ": "
                << std::strerror(open_errno)
                << '\n';

            remove_temp_file(temp_path);
            return 1;
        }

        const SyncOutcome outcome =
            sync_descriptor(contents_fd.get());

        if (!outcome.ok) {
            std::cerr
                << "error: could not synchronise the temporary file to"
                << " stable storage: "
                << std::strerror(outcome.error)
                << '\n';

            remove_temp_file(temp_path);
            return 1;
        }

        if (outcome.fallback_error != 0) {
            std::cerr
                << "warning: F_FULLFSYNC was rejected for the file ("
                << std::strerror(outcome.fallback_error)
                << ")\n"
                << "  fell back to fsync, which hands the data to the"
                << " device without flushing the device's own write"
                << " cache\n";
        }

        contents_barrier = outcome.barrier;
    }

    int publish_errno = 0;

    const PublishResult publish_result = publish_exclusively(
        temp_path,
        output_path,
        publish_errno
    );

    if (publish_result == PublishResult::already_exists) {
        std::cerr
            << "error: refusing to publish over an existing output"
            << " file: "
            << output_path
            << '\n'
            << "  the path was clear when this run started, so another"
            << " process published there while this one was"
            << " converting\n"
            << "  this run's output has been discarded; the file on disk"
            << " belongs to the other run\n";

        remove_temp_file(temp_path);
        return 1;
    }

    if (publish_result == PublishResult::failed) {
        std::cerr
            << "error: could not publish the temporary file under its"
            << " final name: "
            << std::strerror(publish_errno)
            << '\n';

        remove_temp_file(temp_path);
        return 1;
    }

    // The output name now exists and is correct. From here the .tmp is a
    // second name for the same inode, so removing it cannot lose data
    // and a failure to remove it is untidiness rather than a fault: the
    // orphan scan at the top of the next run will name the leftover.
    int unlink_result = 0;

    do {
        unlink_result = ::unlink(temp_path.c_str());
    } while (unlink_result != 0 && errno == EINTR);

    if (unlink_result != 0) {
        const int unlink_errno = errno;

        std::cerr
            << "warning: the output was published but its temporary name"
            << " could not be removed: "
            << temp_path
            << ": "
            << std::strerror(unlink_errno)
            << '\n';
    }

    const SyncOutcome directory_outcome =
        sync_descriptor(directory_fd.get());

    if (!directory_outcome.ok) {
        // The one guard in this file that cannot delete the .tmp and
        // publish nothing, because by the time it is able to fail the
        // .tmp is the .bin. Renaming back would need a second directory
        // operation on the directory that just failed one, and it would
        // destroy a file whose contents are already durable in order to
        // restore a symmetry. So the file stands, the exit status is
        // non-zero so that no script reads this as a clean run, and the
        // message names the guarantee that is missing rather than
        // implying the whole conversion failed.
        std::cerr
            << "error: could not synchronise the output directory: "
            << std::strerror(directory_outcome.error)
            << '\n'
            << "  the .bin was published and its contents are durable\n"
            << "  what is not confirmed durable is the directory entry,"
            << " so a power loss now could lose the name\n"
            << "  do not treat this dataset as reproducible: delete the"
            << " .bin and convert again\n";

        return 1;
    }

    if (directory_outcome.fallback_error != 0) {
        std::cerr
            << "warning: F_FULLFSYNC was rejected for the directory ("
            << std::strerror(directory_outcome.fallback_error)
            << "); fell back to fsync\n";
    }

    std::cout
        << "conversion complete\n"
        << "  input:   " << input_path << '\n'
        << "  output:  " << output_path << '\n'
        << "  symbol:  " << symbol << '\n'
        << "  records: " << record_count << '\n'
        << "  bytes:   " << expected_file_size << '\n'
        << "  header:  read back and verified against 64 bytes\n"
        << "  durable: contents via "
        << barrier_name(contents_barrier)
        << ", directory via "
        << barrier_name(directory_outcome.barrier)
        << '\n';

    return 0;
}