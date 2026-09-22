#include <qiven/runtime/processx/process_runner.hpp>

#include <qiven/error.hpp>
#include <qiven/hashing_sha256.hpp>
#include <qiven/runtime/identity.hpp>

#include <windows.h>

#include <fstream>
#include <iterator>
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

qiven::Error allowlist_error(std::string_view detail)
{
    return qiven::Error::make(qiven::error_category::invalid_argument, err_allowlist,
                              "processx: " + std::string(detail));
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

    // Executable admission: exact path, and digest preflight when the
    // allowlist records one (cpp-design section 11; ARCH section 16.4
    // executable-replacement class). Fail closed BEFORE any spawn.
    if (!spec.allowed_executables.empty())
    {
        std::error_code compare_error;
        const AllowlistedExecutable* admitted = nullptr;
        for (const auto& candidate : spec.allowed_executables)
        {
            if (std::filesystem::exists(candidate.path, compare_error) &&
                std::filesystem::exists(spec.executable, compare_error) &&
                std::filesystem::equivalent(candidate.path, spec.executable, compare_error))
            {
                admitted = &candidate;
                break;
            }
        }
        if (admitted == nullptr)
        {
            return qiven::Result<ProcessRun>::fail(
                allowlist_error("executable is not admitted: " + spec.executable.string()));
        }
        if (!admitted->expected_sha256_hex.empty())
        {
            std::ifstream image(spec.executable, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(image)),
                              std::istreambuf_iterator<char> {});
            const std::string digest = to_hex_sha256(qiven::sha256(bytes));
            if (digest != admitted->expected_sha256_hex)
            {
                return qiven::Result<ProcessRun>::fail(allowlist_error(
                    "executable digest mismatch: " + spec.executable.string()));
            }
        }
    }

    // Kill-on-close Job Object: every descendant dies with the handle —
    // a deadline kill reclaims the whole tree (exit gate 4). A job that
    // cannot be created denies the spawn (fail closed, never unmanaged).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr)
    {
        return qiven::Result<ProcessRun>::fail(spawn_error("job object creation failed"));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        CloseHandle(job);
        return qiven::Result<ProcessRun>::fail(spawn_error("job object configuration failed"));
    }

    Pipe out_pipe;
    Pipe err_pipe;
    if (!out_pipe.create() || !err_pipe.create())
    {
        CloseHandle(job);
        return qiven::Result<ProcessRun>::fail(spawn_error("pipe creation failed"));
    }
    // The child's stdin is a pipe whose write end is closed immediately —
    // the child sees EOF, never a console, never our stdin.
    Pipe in_pipe;
    if (!in_pipe.create())
    {
        CloseHandle(job);
        return qiven::Result<ProcessRun>::fail(spawn_error("stdin pipe creation failed"));
    }

    STARTUPINFOW startup {};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = in_pipe.read_side;
    startup.hStdOutput = out_pipe.write_side;
    startup.hStdError  = err_pipe.write_side;

    // Assign the child into the job AT CREATION (no adoption window).
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::byte> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.data());
    if (attribute_size == 0 ||
        !InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job,
                                   sizeof(job), nullptr, nullptr))
    {
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(job);
        return qiven::Result<ProcessRun>::fail(spawn_error("process attribute list failed"));
    }

    // Explicit environment: allowlist + OS floor, or inherited parent env.
    std::wstring environment;
    LPVOID environment_pointer = nullptr;
    if (!spec.env_allowlist.empty())
    {
        environment = L"SystemRoot=";
        wchar_t system_root[MAX_PATH] {};
        GetEnvironmentVariableW(L"SystemRoot", system_root, MAX_PATH);
        environment += system_root;
        environment += L'\0';
        environment += L"SystemDrive=";
        wchar_t system_drive[4] {};
        GetEnvironmentVariableW(L"SystemDrive", system_drive, 4);
        environment += system_drive;
        environment += L'\0';
        for (const auto& [name, value] : spec.env_allowlist)
        {
            environment += widen(name);
            environment += L'=';
            environment += widen(value);
            environment += L'\0';
        }
        environment += L'\0';
        environment_pointer = environment.data();
    }

    PROCESS_INFORMATION process {};
    const std::wstring command = build_command_line(spec);
    const std::wstring working_dir =
        spec.working_dir.empty() ? L"" : spec.working_dir.wstring();

    // CreateProcessW may write into the command line (argv[0] rewriting);
    // hand it a mutable buffer.
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    startup.cb = sizeof(STARTUPINFOW);
    {
        STARTUPINFOEXW extended {};
        extended.StartupInfo     = startup;
        extended.StartupInfo.cb  = sizeof(extended);
        extended.lpAttributeList = attributes;
        const BOOL spawned       = CreateProcessW(
            spec.executable.wstring().c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment_pointer,
            working_dir.empty() ? nullptr : working_dir.c_str(),
            &extended.StartupInfo, &process);
        if (!spawned)
        {
            DeleteProcThreadAttributeList(attributes);
            CloseHandle(job);
            return qiven::Result<ProcessRun>::fail(
                spawn_error("CreateProcessW failed for " + spec.executable.string() + " (error " +
                            std::to_string(GetLastError()) + ")"));
        }
    }
    DeleteProcThreadAttributeList(attributes);

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
            // Kill the JOB: the direct child AND its whole tree die together
            // (exit gate 4); kill-on-close also covers every later path.
            TerminateJobObject(job, static_cast<UINT>(err_deadline));
            WaitForSingleObject(process.hProcess, 5000);
        }
        else if (wait != WAIT_OBJECT_0)
        {
            TerminateJobObject(job, static_cast<UINT>(err_spawn));
            WaitForSingleObject(process.hProcess, 5000);
        }
    }

    // Closing the job handle is itself a kill-on-close for anything that
    // somehow survived; do it before joining the readers so every pipe
    // write end in the tree is closed and EOF propagates.
    CloseHandle(job);

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
