#pragma once

namespace net::instrument::prometheus
{

// register_process_metrics registers Prometheus' standard process metrics (see instrument/system.hpp), which are
// updated each time the registry is recorded:
//
//   process_cpu_seconds_total         Total user and system CPU time spent in seconds.
//   process_open_fds                  Number of open file descriptors.
//   process_max_fds                   Maximum number of open file descriptors.
//   process_virtual_memory_bytes      Virtual memory size in bytes.
//   process_virtual_memory_max_bytes  Maximum amount of virtual memory available in bytes.
//   process_resident_memory_bytes     Resident memory size in bytes.
//   process_heap_bytes                Process heap size in bytes.
//   process_start_time_seconds        Start time of the process since unix epoch in seconds.
//   process_threads                   Number of OS threads in the process.
//
// They're opt-in: none are registered unless this is called. Those this platform can't report (whose statistic fails
// when this is called) aren't registered. A limit that's unlimited is reported as 2^64 (the largest size, as a double),
// as Go's client does.
//
// Calling it again does nothing.
void register_process_metrics();

}
