// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_WINDOWS_OS_HPP__
#define __STOUT_WINDOWS_OS_HPP__

#include <sys/utime.h>

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/version.hpp>
#include <stout/windows.hpp>

#include <stout/os/os.hpp>
#include <stout/os/getenv.hpp>
#include <stout/os/process.hpp>
#include <stout/os/read.hpp>

#include <stout/os/raw/environment.hpp>
#include <stout/os/windows/fd.hpp>

// NOTE: These system headers must be included after `stout/windows.hpp`
// as they may include `Windows.h`. See comments in `stout/windows.hpp`
// for why this ordering is important.
#include <direct.h>
#include <io.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <Userenv.h>

namespace os {
namespace internal {

inline Try<OSVERSIONINFOEXW> os_version()
{
  OSVERSIONINFOEXW os_version;
  os_version.dwOSVersionInfoSize = sizeof(os_version);
  if (!::GetVersionExW(reinterpret_cast<LPOSVERSIONINFO>(&os_version))) {
    return WindowsError(
        "os::internal::os_version: Call to `GetVersionEx` failed");
  }

  return os_version;
}


inline Try<std::string> nodename()
{
  // MSDN documentation states "The names are established at system startup,
  // when the system reads them from the registry." This is akin to the
  // Linux `gethostname` which calls `uname`, thus avoiding a DNS lookup.
  // The `net::getHostname` function can be used for an explicit DNS lookup.
  //
  // NOTE: This returns the hostname of the local computer, or the local
  // node if this computer is part of a cluster.
  COMPUTER_NAME_FORMAT format = ComputerNamePhysicalDnsHostname;
  DWORD size = 0;
  if (::GetComputerNameExW(format, nullptr, &size) == 0) {
    if (::GetLastError() != ERROR_MORE_DATA) {
      return WindowsError();
    }
  }

  std::vector<wchar_t> buffer;
  buffer.reserve(size);

  if (::GetComputerNameExW(format, buffer.data(), &size) == 0) {
    return WindowsError();
  }

  return stringify(std::wstring(buffer.data()));
}


inline std::string machine()
{
  SYSTEM_INFO system_info;
  ::GetNativeSystemInfo(&system_info);

  switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      return "AMD64";
    case PROCESSOR_ARCHITECTURE_ARM:
      return "ARM";
    case PROCESSOR_ARCHITECTURE_IA64:
      return "IA64";
    case PROCESSOR_ARCHITECTURE_INTEL:
      return "x86";
    default:
      return "Unknown";
  }
}


inline std::string sysname(OSVERSIONINFOEXW os_version)
{
  switch (os_version.wProductType) {
    case VER_NT_DOMAIN_CONTROLLER:
    case VER_NT_SERVER:
      return "Windows Server";
    default:
      return "Windows";
  }
}


inline std::string release(OSVERSIONINFOEXW os_version)
{
  return stringify(
      Version(os_version.dwMajorVersion, os_version.dwMinorVersion, 0));
}


inline std::string version(OSVERSIONINFOEXW os_version)
{
  std::string version = std::to_string(os_version.dwBuildNumber);

  if (os_version.szCSDVersion[0] != L'\0') {
    version.append(" ");
    version.append(stringify(os_version.szCSDVersion));
  }

  return version;
}

} // namespace internal {


// Overload of os::pids for filtering by groups and sessions. A group / session
// id of 0 will fitler on the group / session ID of the calling process.
// NOTE: Windows does not have the concept of a process group, so we need to
// enumerate all processes.
inline Try<std::set<pid_t>> pids(Option<pid_t> group, Option<pid_t> session)
{
  DWORD max_items = 4096;
  DWORD bytes_returned;
  std::vector<pid_t> processes;
  size_t size_in_bytes;

  // Attempt to populate `processes` with PIDs. We repeatedly call
  // `EnumProcesses` with increasingly large arrays until it "succeeds" at
  // populating the array with PIDs. The criteria for determining when
  // `EnumProcesses` has succeeded are:
  //   (1) the return value is nonzero.
  //   (2) the `bytes_returned` is less than the number of bytes in the array.
  do {
    // TODO(alexnaparu): Set a limit to the memory that can be used.
    processes.resize(max_items);
    size_in_bytes = processes.size() * sizeof(pid_t);
    CHECK_LE(size_in_bytes, MAXDWORD);

    BOOL result = ::EnumProcesses(
        processes.data(),
        static_cast<DWORD>(size_in_bytes),
        &bytes_returned);

    if (!result) {
      return WindowsError("os::pids: Call to `EnumProcesses` failed");
    }

    max_items *= 2;
  } while (bytes_returned >= size_in_bytes);

  std::set<pid_t> pids_set(processes.begin(), processes.end());

  // NOTE: The PID `0` will always be returned by `EnumProcesses`; however, it
  // is the PID of Windows' System Idle Process. While the PID is valid, using
  // it for anything is almost always invalid. For instance, `OpenProcess` will
  // fail with an invalid parameter error if the user tries to get a handle for
  // PID `0`. In the interest of safety, we prevent the `pids` API from ever
  // including the PID `0`.
  pids_set.erase(0);
  return pids_set;
}


inline Try<std::set<pid_t>> pids()
{
  return pids(None(), None());
}


// Sets the value associated with the specified key in the set of
// environment variables.
inline void setenv(
    const std::string& key,
    const std::string& value,
    bool overwrite = true)
{
  // Do not set the variable if already set and `overwrite` was not specified.
  //
  // Per MSDN[1], `GetEnvironmentVariable` returns 0 on error and sets the
  // error code to `ERROR_ENVVAR_NOT_FOUND` if the variable was not found.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms683188(v=vs.85).aspx
  if (!overwrite &&
      ::GetEnvironmentVariableW(wide_stringify(key).data(), nullptr, 0) != 0 &&
      ::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
    return;
  }

  // `SetEnvironmentVariable` returns an error code, but we can't act on it.
  ::SetEnvironmentVariableW(
      wide_stringify(key).data(), wide_stringify(value).data());
}


// Unsets the value associated with the specified key in the set of
// environment variables.
inline void unsetenv(const std::string& key)
{
  // Per MSDN documentation[1], passing `nullptr` as the value will cause
  // `SetEnvironmentVariable` to delete the key from the process's environment.
  ::SetEnvironmentVariableW(wide_stringify(key).data(), nullptr);
}


// Suspends execution of the calling process until a child specified by `pid`
// has changed state. Unlike the POSIX standard function `::waitpid`, this
// function does not use -1 and 0 to signify errors and nonblocking return.
// Instead, we return `Result<pid_t>`:
//   * In case of error, we return `Error` rather than -1. For example, we
//     would return an `Error` in case of `EINVAL`.
//   * In case of nonblocking return, we return `None` rather than 0. For
//     example, if we pass `WNOHANG` in the `options`, we would expect 0 to be
//     returned in the case that children specified by `pid` exist, but have
//     not changed state yet. In this case we return `None` instead.
//
// NOTE: There are important differences between the POSIX and Windows
// implementations of this function:
//   * On POSIX, `pid_t` is a signed number, but on Windows, PIDs are `DWORD`,
//     which is `unsigned long`. Thus, if we use `DWORD` to represent the `pid`
//     argument, passing -1 as the `pid` would (on most modern servers)
//     silently convert to a really large `pid`. This is undesirable.
//   * Since it is important to be able to detect -1 has been passed to
//     `os::waitpid`, as a matter of practicality, we choose to:
//     (1) Use `long` to represent the `pid` argument.
//     (2) Disable using any value <= 0 for `pid` on Windows.
//   * This decision is pragmatic. The reasoning is:
//     (1) The Windows code paths call `os::waitpid` in only a handful of
//         places, and in none of these conditions do we need `-1` as a value.
//     (2) Since PIDs virtually never take on values outside the range of
//         vanilla signed `long` it is likely that an accidental conversion
//         will never happen.
//     (3) Even though it is not formalized in the C specification, the
//         implementation of `long` on the vast majority of production servers
//         is 2's complement, so we expect that when we accidentally do
//         implicitly convert from `unsigned long` to `long`, we will "wrap
//         around" to negative values. And since we've disabled the negative
//         `pid` in the Windows implementation, we should error out.
//   * Finally, on Windows, we currently do not check that the process we are
//     attempting to await is a child process.
inline Result<pid_t> waitpid(long pid, int* status, int options)
{
  const bool wait_for_child = (options & WNOHANG) == 0;

  // NOTE: Windows does not implement pids <= 0.
  if (pid <= 0) {
    errno = ENOSYS;
    return ErrnoError(
        "os::waitpid: Value of pid is '" + stringify(pid) +
        "'; the Windows implementation currently does not allow values <= 0");
  } else if (options != 0 && options != WNOHANG) {
    // NOTE: We only support `options == 0` or `options == WNOHANG`. On Windows
    // no flags other than `WNOHANG` are supported.
    errno = ENOSYS;
    return ErrnoError(
        "os::waitpid: Only flag `WNOHANG` is implemented on Windows");
  }

  // TODO(hausdorff): Check that `pid` is one of the child processes. If not,
  // set `errno` to `ECHILD` and return -1.

  // Open the child process as a safe `SharedHandle`.
  const HANDLE process = ::OpenProcess(
      PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
      FALSE,
      static_cast<DWORD>(pid));

  if (process == nullptr) {
    return WindowsError("os::waitpid: Failed to open process for pid '" +
                        stringify(pid) + "'");
  }

  SharedHandle scoped_process(process, ::CloseHandle);

  // If `WNOHANG` flag is set, don't wait. Otherwise, wait for child to
  // terminate.
  const DWORD wait_time = wait_for_child ? INFINITE : 0;
  const DWORD wait_results = ::WaitForSingleObject(
      scoped_process.get(),
      wait_time);

  // Verify our wait exited correctly.
  const bool state_signaled = wait_results == WAIT_OBJECT_0;
  if (options == 0 && !state_signaled) {
    // If `WNOHANG` is not set, then we should have stopped waiting only for a
    // state change in `scoped_process`.
    errno = ECHILD;
    return WindowsError(
        "os::waitpid: Failed to wait for pid '" + stringify(pid) +
        "'. `::WaitForSingleObject` should have waited for child process to " +
        "exit, but returned code '" + stringify(wait_results) +
        "' instead");
  } else if (wait_for_child && !state_signaled &&
             wait_results != WAIT_TIMEOUT) {
    // If `WNOHANG` is set, then a successful wait should report either a
    // timeout (since we set the time to wait to `0`), or a successful state
    // change of `scoped_process`. Anything else is an error.
    errno = ECHILD;
    return WindowsError(
        "os::waitpid: Failed to wait for pid '" + stringify(pid) +
        "'. `ENOHANG` flag was passed in, so `::WaitForSingleObject` should " +
        "have either returned `WAIT_OBJECT_0` or `WAIT_TIMEOUT` (the " +
        "timeout was set to 0, because we are not waiting for the child), " +
        "but instead returned code '" + stringify(wait_results) + "'");
  }

  if (!wait_for_child && wait_results == WAIT_TIMEOUT) {
    // Success. `ENOHANG` was set and we got a timeout, so return `None` (POSIX
    // `::waitpid` would return 0 here).
    return None();
  }

  // Attempt to retrieve exit code from child process. Store that exit code in
  // the `status` variable if it's `nullptr`.
  DWORD child_exit_code = 0;
  if (!::GetExitCodeProcess(scoped_process.get(), &child_exit_code)) {
    errno = ECHILD;
    return WindowsError(
        "os::waitpid: Successfully waited on child process with pid '" +
        std::to_string(pid) + "', but could not retrieve exit code");
  }

  if (status != nullptr) {
    *status = child_exit_code;
  }

  // Success. Return pid of the child process for which the status is reported.
  return pid;
}


inline std::string hstrerror(int err) = delete;


inline Try<Nothing> chown(
    uid_t uid,
    gid_t gid,
    const std::string& path,
    bool recursive) = delete;


inline Try<Nothing> chmod(const std::string& path, int mode) = delete;


inline Try<Nothing> mknod(
    const std::string& path,
    mode_t mode,
    dev_t dev) = delete;


// Suspends execution for the given duration.
// NOTE: This implementation features a millisecond-resolution sleep API, while
// the POSIX version uses a nanosecond-resolution sleep API. As of this writing,
// Mesos only requires millisecond resolution, so this is ok for now.
inline Try<Nothing> sleep(const Duration& duration)
{
  ::Sleep(static_cast<DWORD>(duration.ms()));

  return Nothing();
}


// Returns the list of files that match the given (shell) pattern.
// NOTE: Deleted on Windows, as a POSIX-API-compliant `glob` is much more
// trouble than its worth, considering our relatively simple usage.
inline Try<std::list<std::string>> glob(const std::string& pattern) = delete;


// Returns the total number of cpus (cores).
inline Try<long> cpus()
{
  SYSTEM_INFO sys_info;
  ::GetSystemInfo(&sys_info);
  return static_cast<long>(sys_info.dwNumberOfProcessors);
}

// Returns load struct with average system loads for the last
// 1, 5 and 15 minutes respectively.
// Load values should be interpreted as usual average loads from
// uptime(1).
inline Try<Load> loadavg()
{
  // No Windows equivalent, return an error until there is a need. We can
  // construct an approximation of this function by periodically polling
  // `GetSystemTimes` and using a sliding window of statistics.
  return WindowsError(ERROR_NOT_SUPPORTED,
                      "Failed to determine system load averages");
}


// Returns the total size of main and free memory.
inline Try<Memory> memory()
{
  Memory memory;

  MEMORYSTATUSEX memory_status;
  memory_status.dwLength = sizeof(MEMORYSTATUSEX);
  if (!::GlobalMemoryStatusEx(&memory_status)) {
    return WindowsError("os::memory: Call to `GlobalMemoryStatusEx` failed");
  }

  memory.total = Bytes(memory_status.ullTotalPhys);
  memory.free = Bytes(memory_status.ullAvailPhys);
  memory.totalSwap = Bytes(memory_status.ullTotalPageFile);
  memory.freeSwap = Bytes(memory_status.ullAvailPageFile);

  return memory;
}


inline Try<Version> release()
{
  OSVERSIONINFOEXW os_version;
  os_version.dwOSVersionInfoSize = sizeof(os_version);
  if (!::GetVersionExW(reinterpret_cast<LPOSVERSIONINFO>(&os_version))) {
    return WindowsError("os::release: Call to `GetVersionEx` failed");
  }

  return Version(os_version.dwMajorVersion, os_version.dwMinorVersion, 0);
}


// Return the system information.
inline Try<UTSInfo> uname()
{
  Try<OSVERSIONINFOEXW> os_version = internal::os_version();
  if (os_version.isError()) {
    return Error(os_version.error());
  }

  // Add nodename to `UTSInfo` object.
  Try<std::string> nodename = internal::nodename();
  if (nodename.isError()) {
    return Error(nodename.error());
  }

  // Populate `UTSInfo`.
  UTSInfo info;

  info.sysname = internal::sysname(os_version.get());
  info.release = internal::release(os_version.get());
  info.version = internal::version(os_version.get());
  info.nodename = nodename.get();
  info.machine = internal::machine();

  return info;
}


inline tm* gmtime_r(const time_t* timep, tm* result)
{
  return ::gmtime_s(result, timep) == ERROR_SUCCESS ? result : nullptr;
}


inline Result<PROCESSENTRY32W> process_entry(pid_t pid)
{
  // Get a snapshot of the processes in the system. NOTE: We should not check
  // whether the handle is `nullptr`, because this API will always return
  // `INVALID_HANDLE_VALUE` on error.
  HANDLE snapshot_handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, pid);
  if (snapshot_handle == INVALID_HANDLE_VALUE) {
    return WindowsError(
        "os::process_entry: Call to `CreateToolhelp32Snapshot` failed");
  }

  SharedHandle safe_snapshot_handle(snapshot_handle, ::CloseHandle);

  // Initialize process entry.
  PROCESSENTRY32W process_entry;
  memset(&process_entry, 0, sizeof(process_entry));
  process_entry.dwSize = sizeof(process_entry);

  // Get first process so that we can loop through process entries until we
  // find the one we care about.
  SetLastError(ERROR_SUCCESS);
  BOOL has_next = Process32First(safe_snapshot_handle.get(), &process_entry);
  if (has_next == FALSE) {
    // No first process was found. We should never be here; it is arguable we
    // should return `None`, since we won't find the PID we're looking for, but
    // we elect to return `Error` because something terrible has probably
    // happened.
    if (::GetLastError() != ERROR_SUCCESS) {
      return WindowsError("os::process_entry: Call to `Process32First` failed");
    } else {
      return Error("os::process_entry: Call to `Process32First` failed");
    }
  }

  // Loop through processes until we find the one we're looking for.
  while (has_next == TRUE) {
    if (process_entry.th32ProcessID == pid) {
      // Process found.
      return process_entry;
    }

    has_next = Process32Next(safe_snapshot_handle.get(), &process_entry);
    if (has_next == FALSE) {
      DWORD last_error = ::GetLastError();
      if (last_error != ERROR_NO_MORE_FILES && last_error != ERROR_SUCCESS) {
        return WindowsError(
            "os::process_entry: Call to `Process32Next` failed");
      }
    }
  }

  return None();
}


// Generate a `Process` object for the process associated with `pid`. If
// process is not found, we return `None`; error is reserved for the case where
// something went wrong.
inline Result<Process> process(pid_t pid)
{
  if (pid == 0) {
    // The 0th PID is that of the System Idle Process on Windows. However, it is
    // invalid to attempt to get a proces handle or else perform any operation
    // on this pseudo-process.
    return Error("os::process: Invalid parameter: pid == 0");
  }

  // Find process with pid.
  Result<PROCESSENTRY32W> entry = process_entry(pid);

  if (entry.isError()) {
    return WindowsError(entry.error());
  } else if (entry.isNone()) {
    return None();
  }

  HANDLE process_handle = ::OpenProcess(
      PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
      false,
      pid);

  // ::OpenProcess returns `NULL`, not `INVALID_HANDLE_VALUE` on failure.
  if (process_handle == nullptr) {
    return WindowsError("os::process: Call to `OpenProcess` failed");
  }

  SharedHandle safe_process_handle(process_handle, ::CloseHandle);

  // Get Windows Working set size (Resident set size in linux).
  PROCESS_MEMORY_COUNTERS proc_mem_counters;
  BOOL get_process_memory_info = ::GetProcessMemoryInfo(
      safe_process_handle.get_handle(),
      &proc_mem_counters,
      sizeof(proc_mem_counters));

  if (!get_process_memory_info) {
    return WindowsError("os::process: Call to `GetProcessMemoryInfo` failed");
  }

  // Get session Id.
  pid_t session_id;
  BOOL process_id_to_session_id = ::ProcessIdToSessionId(pid, &session_id);

  if (!process_id_to_session_id) {
    return WindowsError("os::process: Call to `ProcessIdToSessionId` failed");
  }

  // Get Process CPU time.
  FILETIME create_filetime, exit_filetime, kernel_filetime, user_filetime;
  BOOL get_process_times = ::GetProcessTimes(
      safe_process_handle.get_handle(),
      &create_filetime,
      &exit_filetime,
      &kernel_filetime,
      &user_filetime);

  if (!get_process_times) {
    return WindowsError("os::process: Call to `GetProcessTimes` failed");
  }

  // Get utime and stime.
  ULARGE_INTEGER lKernelTime, lUserTime; // In 100 nanoseconds.
  lKernelTime.HighPart = kernel_filetime.dwHighDateTime;
  lKernelTime.LowPart = kernel_filetime.dwLowDateTime;
  lUserTime.HighPart = user_filetime.dwHighDateTime;
  lUserTime.LowPart = user_filetime.dwLowDateTime;

  Try<Duration> utime = Nanoseconds(lKernelTime.QuadPart * 100);
  Try<Duration> stime = Nanoseconds(lUserTime.QuadPart * 100);

  return Process(
      pid,
      entry.get().th32ParentProcessID,         // Parent process id.
      0,                                       // Group id.
      session_id,
      Bytes(proc_mem_counters.WorkingSetSize),
      utime.isSome() ? utime.get() : Option<Duration>::none(),
      stime.isSome() ? stime.get() : Option<Duration>::none(),
      stringify(entry.get().szExeFile),        // Executable filename.
      false);                                  // Is not zombie process.
}


inline int random()
{
  return rand();
}


// `name_job` maps a `pid` to a `wstring` name for a job object.
// Only named job objects are accessible via `OpenJobObject`.
// Thus all our job objects must be named. This is essentially a shim
// to map the Linux concept of a process tree's root `pid` to a
// named job object so that the process group can be treated similarly.
inline Try<std::wstring> name_job(pid_t pid) {
  Try<std::string> alpha_pid = strings::internal::format("MESOS_JOB_%X", pid);
  if (alpha_pid.isError()) {
    return Error(alpha_pid.error());
  }
  return wide_stringify(alpha_pid.get());
}


// `open_job` returns a safe shared handle to the named job object `name`.
// `desired_access` is a job object access rights flag.
// `inherit_handles` if true, processes created by this
// process will inherit the handle. Otherwise, the processes
// do not inherit this handle.
inline Try<SharedHandle> open_job(
    const DWORD desired_access,
    BOOL inherit_handles,
    const std::wstring& name)
{
  SharedHandle job_handle(
      ::OpenJobObjectW(
          desired_access,
          inherit_handles,
          name.data()),
      ::CloseHandle);

  if (job_handle.get_handle() == nullptr) {
    return WindowsError(
        "os::open_job: Call to `OpenJobObject` failed for job: " +
        stringify(name));
  }

  return job_handle;
}


// `create_job` function creates a named job object using `name`.
// This returns the safe job handle, which closes the job handle
// when destructed. Because the job is destroyed when its last
// handle is closed and all associated processes have exited,
// a running process must be assigned to the created job
// before the returned handle is closed.
inline Try<SharedHandle> create_job(const std::wstring& name)
{
  SharedHandle job_handle(
      ::CreateJobObjectW(
          nullptr,       // Use a default security descriptor, and
                         // the created handle cannot be inherited.
          name.data()),  // The name of the job.
      ::CloseHandle);

  if (job_handle.get_handle() == nullptr) {
    return WindowsError(
        "os::create_job: Call to `CreateJobObject` failed for job: " +
        stringify(name));
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};

  // The job object will be terminated when the job handle closes. This allows
  // the job tree to be terminated in case of errors by closing the handle.
  // We set this flag so that the death of the agent process will
  // always kill any running jobs, as the OS will close the remaining open
  // handles if all destructors failed to run (catastrophic death).
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  const BOOL result = ::SetInformationJobObject(
      job_handle.get_handle(),
      JobObjectExtendedLimitInformation,
      &info,
      sizeof(info));

  if (result == FALSE) {
    return WindowsError(
        "os::create_job: `SetInformationJobObject` failed for job: " +
        stringify(name));
  }

  return job_handle;
}


// `assign_job` assigns a process with `pid` to the job object `job_handle`.
// Every process started by the `pid` process using `CreateProcess`
// will also be owned by the job object.
inline Try<Nothing> assign_job(SharedHandle job_handle, pid_t pid) {
  // Get process handle for `pid`.
  SharedHandle process_handle(
      ::OpenProcess(
          // Required access rights to assign to a Job Object.
          PROCESS_SET_QUOTA | PROCESS_TERMINATE,
          false, // Don't inherit handle.
          pid),
      ::CloseHandle);

  if (process_handle.get_handle() == nullptr) {
    return WindowsError(
        "os::assign_job: Call to `OpenProcess` failed");
  }

  const BOOL result = ::AssignProcessToJobObject(
      job_handle.get_handle(),
      process_handle.get_handle());

  if (result == FALSE) {
    return WindowsError(
        "os::assign_job: Call to `AssignProcessToJobObject` failed");
  };

  return Nothing();
}


// The `kill_job` function wraps the Windows sytem call `TerminateJobObject`
// for the job object `job_handle`. This will call `TerminateProcess`
// for every associated child process.
inline Try<Nothing> kill_job(SharedHandle job_handle)
{
  const BOOL result = ::TerminateJobObject(
      job_handle.get_handle(),
      // The exit code to be used by all processes in the job object.
      1);

  if (result == FALSE) {
    return WindowsError(
        "os::kill_job: Call to `TerminateJobObject` failed");
  }

  return Nothing();
}


inline Try<std::string> var()
{
  // Get the `ProgramData` path. First, find the size of the output buffer.
  // This size includes the null-terminating character.
  DWORD size = 0;
  if (::GetAllUsersProfileDirectoryW(nullptr, &size)) {
    // The expected behavior here is for the function to "fail"
    // and return `false`, and `size` receives necessary buffer size.
    return WindowsError(
        "os::var: `GetAllUsersProfileDirectoryW` succeeded unexpectedly");
  }

  std::vector<wchar_t> buffer;
  buffer.reserve(static_cast<size_t>(size));
  if (!::GetAllUsersProfileDirectoryW(buffer.data(), &size)) {
    return WindowsError("os::var: `GetAllUsersProfileDirectoryW` failed");
  }

  return stringify(std::wstring(buffer.data()));
}


// Returns a host-specific default for the `PATH` environment variable, based
// on the configuration of the host.
inline std::string host_default_path()
{
  // NOTE: On Windows, this code must run on the host where we are
  // expecting to `exec` the task, because the value of
  // `%SYSTEMROOT%` is not identical on all platforms.
  const Option<std::string> system_root_env = os::getenv("SYSTEMROOT");
  const std::string system_root = system_root_env.isSome()
    ? system_root_env.get()
    : "C:\\WINDOWS";

  return strings::join(";", system_root, path::join(system_root, "system32"));
}

} // namespace os {

#endif // __STOUT_WINDOWS_OS_HPP__
