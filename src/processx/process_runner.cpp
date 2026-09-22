#include <qiven/runtime/processx/process_runner.hpp>

#include <qiven/error.hpp>

#include <windows.h>

#include <thread>
#include <utility>

namespace qiven::runtime::processx
{
namespace
{
qiven::Error spawn_error(std::string_view detail)
{
    return qiven::Error::make(qiven::error_category::unavailable, err_spawn,
                              "processx: spawn failed: " + std::string(detail));
}

qiven::Error output_limit_error()
{
    return qiven::Error::make(qiven::error_category::resource_exhausted, err_output_limit,
                              "processx: captured stream exceeded its cap");
}

std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
    {
        return std::wstring {};
    }
    std::wstring out(static_cast<usize>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

// Windows command-line quoting per the argument-spawn contract: an
// argument with spaces, tabs, or quotes is wrapped, embedded quotes are
// doubled, and backslashes that precede a quote (or end the argument
// inside quotes) are doubled. Bare arguments pass through untouched.
void append_argument(std::wstring& command, const std::wstring& argument)
{
    const bool needs_quotes = argument.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes)
    {
        command.append(argument);
        return;
    }
    command.push_back(L'"');
    usize backslashes = 0;
    for (const wchar_t c : argument)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'"')
        {
            command.append(backslashes * 2 + 1, L'\\');
            command.push_back(L'"');
        }
        else
        {
            command.append(backslashes, L'\\');
            command.push_back(c);
        }
        backslashes = 0;
    }
    command.append(backslashes * 2, L'\\');
    command.push_back(L'"');
}

std::wstring build_command_line(const ProcessSpec& spec)
{
    std::wstring command;
    append_argument(command, spec.executable.wstring());
    for (usize i = 1; i < spec.argv.size(); ++i)
    {
        command.push_back(L' ');
        append_argument(command, widen(spec.argv[i]));
    }
    return command;
}

struct Pipe
{
    HANDLE read_side  = nullptr;
    HANDLE write_side = nullptr;

    ~Pipe()
    {
        if (read_side != nullptr)
        {
            CloseHandle(read_side);
        }
        if (write_side != nullptr)
        {
            CloseHandle(write_side);
        }
    }

    [[nodiscard]] bool create()
    {
        SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        return CreatePipe(&read_side, &write_side, &inheritable, 64 * 1024) != FALSE;
    }
};

// One reader thread's state: appends bytes into the cap-bounded buffer
// and flags overflow (the reader keeps draining so the child can never
// block on a full pipe — the run is classified after the join).
struct Capture
{
    HANDLE source = nullptr;
    u64 cap       = 0;
    std::string bytes;
    bool overflowed = false;
};

void read_stream(Capture& capture)
{
    char buffer[8192];
    while (true)
    {
        DWORD read_bytes = 0;
        if (!ReadFile(capture.source, buffer, sizeof(buffer), &read_bytes, nullptr) ||
            read_bytes == 0)
        {
            return; // broken pipe (child end closed) or error: either way, done
        }
        if (capture.bytes.size() + read_bytes <= capture.cap)
        {
            capture.bytes.append(buffer, read_bytes);
        }
        else
        {
            capture.overflowed = true; // keep draining; classification happens after join
        }
    }
}
} // namespace

qiven::Result<ProcessRun> ProcessRunner::run(const ProcessSpec& spec) const
{
    if (spec.executable.empty() || spec.argv.empty())
    {
        return qiven::Result<ProcessRun>::fail(spawn_error("executable and argv are required"));
    }
    if (spec.deadline_ms == 0)
    {
        return qiven::Result<ProcessRun>::fail(spawn_error("deadline must be explicit"));
    }

    Pipe out_pipe;
    Pipe err_pipe;
    if (!out_pipe.create() || !err_pipe.create())
    {
        return qiven::Result<ProcessRun>::fail(spawn_error("pipe creation failed"));
    }
    // The child's stdin is a pipe whose write end is closed immediately —
    // the child sees EOF, never a console, never our stdin.
    Pipe in_pipe;
    if (!in_pipe.create())
    {
        return qiven::Result<ProcessRun>::fail(spawn_error("stdin pipe creation failed"));
    }

    STARTUPINFOW startup {};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = in_pipe.read_side;
    startup.hStdOutput = out_pipe.write_side;
    startup.hStdError  = err_pipe.write_side;

    PROCESS_INFORMATION process {};
    const std::wstring command = build_command_line(spec);
    const std::wstring working_dir =
        spec.working_dir.empty() ? L"" : spec.working_dir.wstring();

    // CreateProcessW may write into the command line (argv[0] rewriting);
    // hand it a mutable buffer.
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL spawned = CreateProcessW(
        spec.executable.wstring().c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
        working_dir.empty() ? nullptr : working_dir.c_str(), &startup, &process);
    if (!spawned)
    {
        return qiven::Result<ProcessRun>::fail(
            spawn_error("CreateProcessW failed for " + spec.executable.string() + " (error " +
                        std::to_string(GetLastError()) + ")"));
    }

    // The parent holds only the child-facing write ends and the read ends
    // it drains; close everything else so EOF propagates when the child dies.
    CloseHandle(process.hThread);
    CloseHandle(out_pipe.write_side);
    out_pipe.write_side = nullptr;
    CloseHandle(err_pipe.write_side);
    err_pipe.write_side = nullptr;
    CloseHandle(in_pipe.write_side);
    in_pipe.write_side = nullptr;
    CloseHandle(in_pipe.read_side);
    in_pipe.read_side = nullptr;

    Capture out_capture { out_pipe.read_side, spec.output_cap_bytes, {}, false };
    Capture err_capture { err_pipe.read_side, spec.output_cap_bytes, {}, false };
    std::thread out_reader([&out_capture] { read_stream(out_capture); });
    std::thread err_reader([&err_capture] { read_stream(err_capture); });

    bool timed_out = false;
    {
        const DWORD wait = WaitForSingleObject(process.hProcess, static_cast<DWORD>(spec.deadline_ms));
        if (wait == WAIT_TIMEOUT)
        {
            timed_out = true;
            TerminateProcess(process.hProcess, static_cast<UINT>(err_deadline));
            WaitForSingleObject(process.hProcess, 5000);
        }
        else if (wait != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, static_cast<UINT>(err_spawn));
            WaitForSingleObject(process.hProcess, 5000);
        }
    }

    // Reader threads exit once the child's write ends close (process death
    // closes them). Join before touching their buffers.
    out_reader.join();
    err_reader.join();

    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);

    if (out_capture.overflowed || err_capture.overflowed)
    {
        return qiven::Result<ProcessRun>::fail(output_limit_error());
    }

    ProcessRun run;
    run.end       = timed_out ? ProcessRun::End::TimedOut : ProcessRun::End::Exited;
    run.exit_code = static_cast<i32>(exit_code);
    run.out       = std::move(out_capture.bytes);
    run.err       = std::move(err_capture.bytes);
    return qiven::Result<ProcessRun>(std::move(run));
}
} // namespace qiven::runtime::processx
