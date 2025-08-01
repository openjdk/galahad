/*
 * Copyright (c) 1999, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "classfile/classLoader.hpp"
#include "interpreter/interpreter.hpp"
#include "jvm.h"
#include "jvmtifiles/jvmti.h"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "nmt/memTracker.hpp"
#include "os_posix.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/osThread.hpp"
#include "runtime/park.hpp"
#include "runtime/perfMemory.hpp"
#include "runtime/sharedRuntime.hpp"
#include "services/attachListener.hpp"
#include "utilities/align.hpp"
#include "utilities/checkedCast.hpp"
#include "utilities/debug.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/events.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include "utilities/permitForbiddenFunctions.hpp"
#include "utilities/vmError.hpp"
#if INCLUDE_JFR
#include "jfr/support/jfrNativeLibraryLoadEvent.hpp"
#endif

#ifdef AIX
#include "loadlib_aix.hpp"
#include "os_aix.hpp"
#include "porting_aix.hpp"
#endif
#ifdef LINUX
#include "os_linux.hpp"
#endif

#include <dirent.h>
#include <dlfcn.h>
#include <grp.h>
#include <locale.h>
#include <netdb.h>
#include <pwd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#ifdef __APPLE__
  #include <crt_externs.h>
#endif

#define ROOT_UID 0

#ifndef MAP_ANONYMOUS
  #define MAP_ANONYMOUS MAP_ANON
#endif

/* Input/Output types for mincore(2) */
typedef LINUX_ONLY(unsigned) char mincore_vec_t;

static jlong initial_time_count = 0;

static int clock_tics_per_sec = 100;

// Platform minimum stack allowed
size_t os::_os_min_stack_allowed = PTHREAD_STACK_MIN;

// Check core dump limit and report possible place where core can be found
void os::check_core_dump_prerequisites(char* buffer, size_t bufferSize, bool check_only) {
  if (!FLAG_IS_DEFAULT(CreateCoredumpOnCrash) && !CreateCoredumpOnCrash) {
    jio_snprintf(buffer, bufferSize, "CreateCoredumpOnCrash is disabled from command line");
    VMError::record_coredump_status(buffer, false);
  } else {
    struct rlimit rlim;
    bool success = true;
    bool warn = true;
    char core_path[PATH_MAX];
    if (get_core_path(core_path, PATH_MAX) <= 0) {
      jio_snprintf(buffer, bufferSize, "core.%d (may not exist)", current_process_id());
#ifdef LINUX
    } else if (core_path[0] == '"') { // redirect to user process
      jio_snprintf(buffer, bufferSize, "Core dumps may be processed with %s", core_path);
#endif
    } else if (getrlimit(RLIMIT_CORE, &rlim) != 0) {
      jio_snprintf(buffer, bufferSize, "%s (may not exist)", core_path);
    } else {
      switch(rlim.rlim_cur) {
        case RLIM_INFINITY:
          jio_snprintf(buffer, bufferSize, "%s", core_path);
          warn = false;
          break;
        case 0:
          jio_snprintf(buffer, bufferSize, "Core dumps have been disabled. To enable core dumping, try \"ulimit -c unlimited\" before starting Java again");
          success = false;
          break;
        default:
          jio_snprintf(buffer, bufferSize, "%s (max size " UINT64_FORMAT " k). To ensure a full core dump, try \"ulimit -c unlimited\" before starting Java again", core_path, uint64_t(rlim.rlim_cur) / K);
          break;
      }
    }
    if (!check_only) {
      VMError::record_coredump_status(buffer, success);
    } else if (warn) {
      warning("CreateCoredumpOnCrash specified, but %s", buffer);
    }
  }
}

bool os::committed_in_range(address start, size_t size, address& committed_start, size_t& committed_size) {

#ifdef _AIX
  committed_start = start;
  committed_size = size;
  return true;
#else

  int mincore_return_value;
  constexpr size_t stripe = 1024;  // query this many pages each time
  mincore_vec_t vec [stripe + 1];

  // set a guard
  DEBUG_ONLY(vec[stripe] = 'X');

  size_t page_sz = os::vm_page_size();
  uintx pages = size / page_sz;

  assert(is_aligned(start, page_sz), "Start address must be page aligned");
  assert(is_aligned(size, page_sz), "Size must be page aligned");

  committed_start = nullptr;

  int loops = checked_cast<int>((pages + stripe - 1) / stripe);
  int committed_pages = 0;
  address loop_base = start;
  bool found_range = false;

  for (int index = 0; index < loops && !found_range; index ++) {
    assert(pages > 0, "Nothing to do");
    uintx pages_to_query = (pages >= stripe) ? stripe : pages;
    pages -= pages_to_query;

    // Get stable read
    int fail_count = 0;
    while ((mincore_return_value = mincore(loop_base, pages_to_query * page_sz, vec)) == -1 && errno == EAGAIN){
      if (++fail_count == 1000){
        return false;
      }
    }

    // During shutdown, some memory goes away without properly notifying NMT,
    // E.g. ConcurrentGCThread/WatcherThread can exit without deleting thread object.
    // Bailout and return as not committed for now.
    if (mincore_return_value == -1 && errno == ENOMEM) {
      return false;
    }

    // If mincore is not supported.
    if (mincore_return_value == -1 && errno == ENOSYS) {
      return false;
    }

    assert(vec[stripe] == 'X', "overflow guard");
    assert(mincore_return_value == 0, "Range must be valid");
    // Process this stripe
    for (uintx vecIdx = 0; vecIdx < pages_to_query; vecIdx ++) {
      if ((vec[vecIdx] & 0x01) == 0) { // not committed
        // End of current contiguous region
        if (committed_start != nullptr) {
          found_range = true;
          break;
        }
      } else { // committed
        // Start of region
        if (committed_start == nullptr) {
          committed_start = loop_base + page_sz * vecIdx;
        }
        committed_pages ++;
      }
    }

    loop_base += pages_to_query * page_sz;
  }

  if (committed_start != nullptr) {
    assert(committed_pages > 0, "Must have committed region");
    assert(committed_pages <= int(size / page_sz), "Can not commit more than it has");
    assert(committed_start >= start && committed_start < start + size, "Out of range");
    committed_size = page_sz * committed_pages;
    return true;
  } else {
    assert(committed_pages == 0, "Should not have committed region");
    return false;
  }
#endif
}

int os::get_native_stack(address* stack, int frames, int toSkip) {
  int frame_idx = 0;
  int num_of_frames;  // number of frames captured
  frame fr = os::current_frame();
  while (fr.pc() && frame_idx < frames) {
    if (toSkip > 0) {
      toSkip --;
    } else {
      stack[frame_idx ++] = fr.pc();
    }
    if (fr.fp() == nullptr || fr.cb() != nullptr ||
        fr.sender_pc() == nullptr || os::is_first_C_frame(&fr)) {
      break;
    }
    fr = os::get_sender_for_C_frame(&fr);
  }
  num_of_frames = frame_idx;
  for (; frame_idx < frames; frame_idx ++) {
    stack[frame_idx] = nullptr;
  }

  return num_of_frames;
}

int os::get_last_error() {
  return errno;
}

size_t os::lasterror(char *buf, size_t len) {
  if (errno == 0)  return 0;

  const char *s = os::strerror(errno);
  size_t n = ::strlen(s);
  if (n >= len) {
    n = len - 1;
  }
  ::strncpy(buf, s, n);
  buf[n] = '\0';
  return n;
}

////////////////////////////////////////////////////////////////////////////////
// breakpoint support

void os::breakpoint() {
  BREAKPOINT;
}

extern "C" void breakpoint() {
  // use debugger to set breakpoint here
}

// Return true if user is running as root.
bool os::have_special_privileges() {
  static bool privileges = (getuid() != geteuid()) || (getgid() != getegid());
  return privileges;
}

void os::wait_for_keypress_at_exit(void) {
  // don't do anything on posix platforms
  return;
}

int os::create_file_for_heap(const char* dir) {
  int fd;

#if defined(LINUX) && defined(O_TMPFILE)
  char* native_dir = os::strdup(dir);
  if (native_dir == nullptr) {
    vm_exit_during_initialization(err_msg("strdup failed during creation of backing file for heap (%s)", os::strerror(errno)));
    return -1;
  }
  os::native_path(native_dir);
  fd = os::open(dir, O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR);
  os::free(native_dir);

  if (fd == -1)
#endif
  {
    const char name_template[] = "/jvmheap.XXXXXX";

    size_t fullname_len = strlen(dir) + strlen(name_template);
    char *fullname = (char*)os::malloc(fullname_len + 1, mtInternal);
    if (fullname == nullptr) {
      vm_exit_during_initialization(err_msg("Malloc failed during creation of backing file for heap (%s)", os::strerror(errno)));
      return -1;
    }
    int n = snprintf(fullname, fullname_len + 1, "%s%s", dir, name_template);
    assert((size_t)n == fullname_len, "Unexpected number of characters in string");

    os::native_path(fullname);

    // create a new file.
    fd = mkstemp(fullname);

    if (fd < 0) {
      warning("Could not create file for heap with template %s", fullname);
      os::free(fullname);
      return -1;
    } else {
      // delete the name from the filesystem. When 'fd' is closed, the file (and space) will be deleted.
      int ret = unlink(fullname);
      assert_with_errno(ret == 0, "unlink returned error");
    }

    os::free(fullname);
  }

  return fd;
}

// return current position of file pointer
jlong os::current_file_offset(int fd) {
  return (jlong)::lseek(fd, (off_t)0, SEEK_CUR);
}

// move file pointer to the specified offset
jlong os::seek_to_file_offset(int fd, jlong offset) {
  return (jlong)::lseek(fd, (off_t)offset, SEEK_SET);
}

// Is a (classpath) directory empty?
bool os::dir_is_empty(const char* path) {
  DIR *dir = nullptr;
  struct dirent *ptr;

  dir = ::opendir(path);
  if (dir == nullptr) return true;

  // Scan the directory
  bool result = true;
  while (result && (ptr = ::readdir(dir)) != nullptr) {
    if (strcmp(ptr->d_name, ".") != 0 && strcmp(ptr->d_name, "..") != 0) {
      result = false;
    }
  }
  ::closedir(dir);
  return result;
}

static char* reserve_mmapped_memory(size_t bytes, char* requested_addr, MemTag mem_tag) {
  char * addr;
  int flags = MAP_PRIVATE NOT_AIX( | MAP_NORESERVE ) | MAP_ANONYMOUS;
  if (requested_addr != nullptr) {
    assert((uintptr_t)requested_addr % os::vm_page_size() == 0, "Requested address should be aligned to OS page size");
    flags |= MAP_FIXED;
  }

  // Map reserved/uncommitted pages PROT_NONE so we fail early if we
  // touch an uncommitted page. Otherwise, the read/write might
  // succeed if we have enough swap space to back the physical page.
  addr = (char*)::mmap(requested_addr, bytes, PROT_NONE,
                       flags, -1, 0);

  if (addr != MAP_FAILED) {
    MemTracker::record_virtual_memory_reserve((address)addr, bytes, CALLER_PC, mem_tag);
    return addr;
  }
  return nullptr;
}

static int util_posix_fallocate(int fd, off_t offset, off_t len) {
  static_assert(sizeof(off_t) == 8, "Expected Large File Support in this file");
#ifdef __APPLE__
  fstore_t store = { F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, len };
  // First we try to get a continuous chunk of disk space
  int ret = fcntl(fd, F_PREALLOCATE, &store);
  if (ret == -1) {
    // Maybe we are too fragmented, try to allocate non-continuous range
    store.fst_flags = F_ALLOCATEALL;
    ret = fcntl(fd, F_PREALLOCATE, &store);
  }
  if(ret != -1) {
    return ftruncate(fd, len);
  }
  return -1;
#else
  return posix_fallocate(fd, offset, len);
#endif
}

// Map the given address range to the provided file descriptor.
char* os::map_memory_to_file(char* base, size_t size, int fd) {
  assert(fd != -1, "File descriptor is not valid");

  // allocate space for the file
  int ret = util_posix_fallocate(fd, 0, (off_t)size);
  if (ret != 0) {
    vm_exit_during_initialization(err_msg("Error in mapping Java heap at the given filesystem directory. error(%d)", ret));
    return nullptr;
  }

  int prot = PROT_READ | PROT_WRITE;
  int flags = MAP_SHARED;
  if (base != nullptr) {
    flags |= MAP_FIXED;
  }
  char* addr = (char*)mmap(base, size, prot, flags, fd, 0);

  if (addr == MAP_FAILED) {
    warning("Failed mmap to file. (%s)", os::strerror(errno));
    return nullptr;
  }
  if (base != nullptr && addr != base) {
    if (!os::release_memory(addr, size)) {
      warning("Could not release memory on unsuccessful file mapping");
    }
    return nullptr;
  }
  return addr;
}

char* os::replace_existing_mapping_with_file_mapping(char* base, size_t size, int fd) {
  assert(fd != -1, "File descriptor is not valid");
  assert(base != nullptr, "Base cannot be null");

  return map_memory_to_file(base, size, fd);
}

static size_t calculate_aligned_extra_size(size_t size, size_t alignment) {
  assert(is_aligned(alignment, os::vm_allocation_granularity()),
      "Alignment must be a multiple of allocation granularity (page size)");
  assert(is_aligned(size, os::vm_allocation_granularity()),
      "Size must be a multiple of allocation granularity (page size)");

  size_t extra_size = size + alignment;
  assert(extra_size >= size, "overflow, size is too large to allow alignment");
  return extra_size;
}

// After a bigger chunk was mapped, unmaps start and end parts to get the requested alignment.
static char* chop_extra_memory(size_t size, size_t alignment, char* extra_base, size_t extra_size) {
  // Do manual alignment
  char* aligned_base = align_up(extra_base, alignment);

  // [  |                                       |  ]
  // ^ extra_base
  //    ^ extra_base + begin_offset == aligned_base
  //     extra_base + begin_offset + size       ^
  //                       extra_base + extra_size ^
  // |<>| == begin_offset
  //                              end_offset == |<>|
  size_t begin_offset = aligned_base - extra_base;
  size_t end_offset = (extra_base + extra_size) - (aligned_base + size);

  if (begin_offset > 0) {
      os::release_memory(extra_base, begin_offset);
  }

  if (end_offset > 0) {
      os::release_memory(extra_base + begin_offset + size, end_offset);
  }

  return aligned_base;
}

// Multiple threads can race in this code, and can remap over each other with MAP_FIXED,
// so on posix, unmap the section at the start and at the end of the chunk that we mapped
// rather than unmapping and remapping the whole chunk to get requested alignment.
char* os::reserve_memory_aligned(size_t size, size_t alignment, MemTag mem_tag, bool exec) {
  size_t extra_size = calculate_aligned_extra_size(size, alignment);
  char* extra_base = os::reserve_memory(extra_size, mem_tag, exec);
  if (extra_base == nullptr) {
    return nullptr;
  }
  return chop_extra_memory(size, alignment, extra_base, extra_size);
}

char* os::map_memory_to_file_aligned(size_t size, size_t alignment, int file_desc, MemTag mem_tag) {
  size_t extra_size = calculate_aligned_extra_size(size, alignment);
  // For file mapping, we do not call os:map_memory_to_file(size,fd) since:
  // - we later chop away parts of the mapping using os::release_memory and that could fail if the
  //   original mmap call had been tied to an fd.
  // - The memory API os::reserve_memory uses is an implementation detail. It may (and usually is)
  //   mmap but it also may System V shared memory which cannot be uncommitted as a whole, so
  //   chopping off and unmapping excess bits back and front (see below) would not work.
  char* extra_base = reserve_mmapped_memory(extra_size, nullptr, mem_tag);
  if (extra_base == nullptr) {
    return nullptr;
  }
  char* aligned_base = chop_extra_memory(size, alignment, extra_base, extra_size);
  // After we have an aligned address, we can replace anonymous mapping with file mapping
  if (replace_existing_mapping_with_file_mapping(aligned_base, size, file_desc) == nullptr) {
    vm_exit_during_initialization(err_msg("Error in mapping Java heap at the given filesystem directory"));
  }
  MemTracker::record_virtual_memory_commit((address)aligned_base, size, CALLER_PC);
  return aligned_base;
}

int os::get_fileno(FILE* fp) {
  return NOT_AIX(::)fileno(fp);
}

struct tm* os::gmtime_pd(const time_t* clock, struct tm*  res) {
  return gmtime_r(clock, res);
}

void os::Posix::print_load_average(outputStream* st) {
  st->print("load average: ");
  double loadavg[3];
  int res = os::loadavg(loadavg, 3);
  if (res != -1) {
    st->print("%0.02f %0.02f %0.02f", loadavg[0], loadavg[1], loadavg[2]);
  } else {
    st->print(" Unavailable");
  }
  st->cr();
}

// boot/uptime information;
// unfortunately it does not work on macOS and Linux because the utx chain has no entry
// for reboot at least on my test machines
void os::Posix::print_uptime_info(outputStream* st) {
  int bootsec = -1;
  time_t currsec = time(nullptr);
  struct utmpx* ent;
  setutxent();
  while ((ent = getutxent())) {
    if (!strcmp("system boot", ent->ut_line)) {
      bootsec = (int)ent->ut_tv.tv_sec;
      break;
    }
  }

  if (bootsec != -1) {
    os::print_dhm(st, "OS uptime:", currsec-bootsec);
  }
}

static void print_rlimit(outputStream* st, const char* msg,
                         int resource, bool output_k = false) {
  struct rlimit rlim;

  st->print(" %s ", msg);
  int res = getrlimit(resource, &rlim);
  if (res == -1) {
    st->print("could not obtain value");
  } else {
    // soft limit
    if (rlim.rlim_cur == RLIM_INFINITY) { st->print("infinity"); }
    else {
      if (output_k) { st->print(UINT64_FORMAT "k", uint64_t(rlim.rlim_cur) / K); }
      else { st->print(UINT64_FORMAT, uint64_t(rlim.rlim_cur)); }
    }
    // hard limit
    st->print("/");
    if (rlim.rlim_max == RLIM_INFINITY) { st->print("infinity"); }
    else {
      if (output_k) { st->print(UINT64_FORMAT "k", uint64_t(rlim.rlim_max) / K); }
      else { st->print(UINT64_FORMAT, uint64_t(rlim.rlim_max)); }
    }
  }
}

void os::Posix::print_rlimit_info(outputStream* st) {
  st->print("rlimit (soft/hard):");
  print_rlimit(st, "STACK", RLIMIT_STACK, true);
  print_rlimit(st, ", CORE", RLIMIT_CORE, true);

#if defined(AIX)
  st->print(", NPROC ");
  st->print("%ld", sysconf(_SC_CHILD_MAX));

  print_rlimit(st, ", THREADS", RLIMIT_THREADS);
#else
  print_rlimit(st, ", NPROC", RLIMIT_NPROC);
#endif

  print_rlimit(st, ", NOFILE", RLIMIT_NOFILE);
  print_rlimit(st, ", AS", RLIMIT_AS, true);
  print_rlimit(st, ", CPU", RLIMIT_CPU);
  print_rlimit(st, ", DATA", RLIMIT_DATA, true);

  // maximum size of files that the process may create
  print_rlimit(st, ", FSIZE", RLIMIT_FSIZE, true);

#if defined(LINUX) || defined(__APPLE__)
  // maximum number of bytes of memory that may be locked into RAM
  // (rounded down to the nearest  multiple of system pagesize)
  print_rlimit(st, ", MEMLOCK", RLIMIT_MEMLOCK, true);
#endif

  // MacOS; The maximum size (in bytes) to which a process's resident set size may grow.
#if defined(__APPLE__)
  print_rlimit(st, ", RSS", RLIMIT_RSS, true);
#endif

  st->cr();
}

void os::Posix::print_uname_info(outputStream* st) {
  // kernel
  st->print("uname: ");
  struct utsname name;
  uname(&name);
  st->print("%s ", name.sysname);
#ifdef ASSERT
  st->print("%s ", name.nodename);
#endif
  st->print("%s ", name.release);
  st->print("%s ", name.version);
  st->print("%s", name.machine);
  st->cr();
}

void os::Posix::print_umask(outputStream* st, mode_t umsk) {
  st->print((umsk & S_IRUSR) ? "r" : "-");
  st->print((umsk & S_IWUSR) ? "w" : "-");
  st->print((umsk & S_IXUSR) ? "x" : "-");
  st->print((umsk & S_IRGRP) ? "r" : "-");
  st->print((umsk & S_IWGRP) ? "w" : "-");
  st->print((umsk & S_IXGRP) ? "x" : "-");
  st->print((umsk & S_IROTH) ? "r" : "-");
  st->print((umsk & S_IWOTH) ? "w" : "-");
  st->print((umsk & S_IXOTH) ? "x" : "-");
}

void os::print_user_info(outputStream* st) {
  unsigned id = (unsigned) ::getuid();
  st->print("uid  : %u ", id);
  id = (unsigned) ::geteuid();
  st->print("euid : %u ", id);
  id = (unsigned) ::getgid();
  st->print("gid  : %u ", id);
  id = (unsigned) ::getegid();
  st->print_cr("egid : %u", id);
  st->cr();

  mode_t umsk = ::umask(0);
  ::umask(umsk);
  st->print("umask: %04o (", (unsigned) umsk);
  os::Posix::print_umask(st, umsk);
  st->print_cr(")");
  st->cr();
}

// Print all active locale categories, one line each
void os::print_active_locale(outputStream* st) {
  st->print_cr("Active Locale:");
  // Posix is quiet about how exactly LC_ALL is implemented.
  // Just print it out too, in case LC_ALL is held separately
  // from the individual categories.
  #define LOCALE_CAT_DO(f) \
    f(LC_ALL) \
    f(LC_COLLATE) \
    f(LC_CTYPE) \
    f(LC_MESSAGES) \
    f(LC_MONETARY) \
    f(LC_NUMERIC) \
    f(LC_TIME)
  #define XX(cat) { cat, #cat },
  const struct { int c; const char* name; } categories[] = {
      LOCALE_CAT_DO(XX)
      { -1, nullptr }
  };
  #undef XX
  #undef LOCALE_CAT_DO
  for (int i = 0; categories[i].c != -1; i ++) {
    const char* locale = setlocale(categories[i].c, nullptr);
    st->print_cr("%s=%s", categories[i].name,
                 ((locale != nullptr) ? locale : "<unknown>"));
  }
}

bool os::get_host_name(char* buf, size_t buflen) {
  struct utsname name;
  int retcode = uname(&name);
  if (retcode != -1) {
    jio_snprintf(buf, buflen, "%s", name.nodename);
    return true;
  }
  const char* errmsg = os::strerror(errno);
  log_warning(os)("Failed to get host name, error message: %s", errmsg);
  return false;
}

#ifndef _LP64
// Helper, on 32bit, for os::commit_memory_limit
static bool is_allocatable(size_t s) {
  if (s < 2 * G) {
    return true;
  }
  // Use raw anonymous mmap here; no need to go through any
  // of our reservation layers. We will unmap right away.
  void* p = ::mmap(nullptr, s, PROT_NONE,
                   MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    return false;
  } else {
    ::munmap(p, s);
    return true;
  }
}
#endif // !_LP64

size_t os::commit_memory_limit() {
  // On POSIX systems, the amount of memory that can be commmitted is limited
  // by the size of the reservable memory.
  size_t reserve_limit = reserve_memory_limit();

#ifdef _LP64
  return reserve_limit;
#else
  // Arbitrary max reserve limit for 32 bit Unices found by testing.
  const size_t max_reserve_limit = 3800 * M;

  // Bound the reserve limit with the arbitrary max.
  size_t actual_limit = MIN2(reserve_limit, max_reserve_limit);

  // bound by actually allocatable memory. The algorithm uses two bounds, an
  // upper and a lower limit. The upper limit is the current highest amount of
  // memory that could not be allocated, the lower limit is the current highest
  // amount of memory that could be allocated.
  // The algorithm iteratively refines the result by halving the difference
  // between these limits, updating either the upper limit (if that value could
  // not be allocated) or the lower limit (if the that value could be allocated)
  // until the difference between these limits is "small".

  // the minimum amount of memory we care about allocating.
  const size_t min_allocation_size = M;

  size_t upper_limit = actual_limit;

  // first check a few trivial cases
  if (is_allocatable(upper_limit) || (upper_limit <= min_allocation_size)) {
    // The actual limit is allocatable, no need to do anything.
  } else if (!is_allocatable(min_allocation_size)) {
    // we found that not even min_allocation_size is allocatable. Return it
    // anyway. There is no point to search for a better value any more.
    actual_limit = min_allocation_size;
  } else {
    // perform the binary search.
    size_t lower_limit = min_allocation_size;
    while ((upper_limit - lower_limit) > min_allocation_size) {
      size_t temp_limit = ((upper_limit - lower_limit) / 2) + lower_limit;
      temp_limit = align_down(temp_limit, min_allocation_size);
      if (is_allocatable(temp_limit)) {
        lower_limit = temp_limit;
      } else {
        upper_limit = temp_limit;
      }
    }
    actual_limit = lower_limit;
  }

  return actual_limit;
#endif
}

size_t os::reserve_memory_limit() {
  struct rlimit rlim;
  int getrlimit_res = getrlimit(RLIMIT_AS, &rlim);

  // If there was an error calling getrlimit, conservatively assume no limit.
  if (getrlimit_res != 0) {
    return SIZE_MAX;
  }

  // If the current limit is not infinity, there is a limit.
  if (rlim.rlim_cur != RLIM_INFINITY) {
    return (size_t)rlim.rlim_cur;
  }

  // No limit
  return SIZE_MAX;
}

void* os::get_default_process_handle() {
#ifdef __APPLE__
  // MacOS X needs to use RTLD_FIRST instead of RTLD_LAZY
  // to avoid finding unexpected symbols on second (or later)
  // loads of a library.
  return (void*)::dlopen(nullptr, RTLD_FIRST);
#else
  return (void*)::dlopen(nullptr, RTLD_LAZY);
#endif
}

void* os::dll_lookup(void* handle, const char* name) {
  ::dlerror(); // Clear any previous error
  void* ret = ::dlsym(handle, name);
  if (ret == nullptr) {
    const char* tmp = ::dlerror();
    // It is possible that we found a null symbol, hence no error.
    if (tmp != nullptr) {
      log_debug(os)("Symbol %s not found in dll: %s", name, tmp);
    }
  }
  return ret;
}

void os::dll_unload(void *lib) {
  // os::Linux::dll_path returns a pointer to a string that is owned by the dynamic loader. Upon
  // calling dlclose the dynamic loader may free the memory containing the string, thus we need to
  // copy the string to be able to reference it after dlclose.
  const char* l_path = nullptr;

#ifdef LINUX
  char* l_pathdup = nullptr;
  l_path = os::Linux::dll_path(lib);
  if (l_path != nullptr) {
    l_path = l_pathdup = os::strdup(l_path);
  }
#endif  // LINUX

  JFR_ONLY(NativeLibraryUnloadEvent unload_event(l_path);)

  if (l_path == nullptr) {
    l_path = "<not available>";
  }

  char ebuf[1024];
  bool res = os::pd_dll_unload(lib, ebuf, sizeof(ebuf));

  if (res) {
    Events::log_dll_message(nullptr, "Unloaded shared library \"%s\" [" INTPTR_FORMAT "]",
                            l_path, p2i(lib));
    log_info(os)("Unloaded shared library \"%s\" [" INTPTR_FORMAT "]", l_path, p2i(lib));
    JFR_ONLY(unload_event.set_result(true);)
  } else {
    Events::log_dll_message(nullptr, "Attempt to unload shared library \"%s\" [" INTPTR_FORMAT "] failed, %s",
                            l_path, p2i(lib), ebuf);
    log_info(os)("Attempt to unload shared library \"%s\" [" INTPTR_FORMAT "] failed, %s",
                  l_path, p2i(lib), ebuf);
    JFR_ONLY(unload_event.set_error_msg(ebuf);)
  }
  LINUX_ONLY(os::free(l_pathdup));
}

void* os::lookup_function(const char* name) {
  // This returns the global symbol in the main executable and its dependencies,
  // as well as shared objects dynamically loaded with RTLD_GLOBAL flag.
  return dlsym(RTLD_DEFAULT, name);
}

jlong os::lseek(int fd, jlong offset, int whence) {
  return (jlong) ::lseek(fd, offset, whence);
}

int os::ftruncate(int fd, jlong length) {
   return ::ftruncate(fd, length);
}

const char* os::get_current_directory(char *buf, size_t buflen) {
  return getcwd(buf, buflen);
}

FILE* os::fdopen(int fd, const char* mode) {
  return ::fdopen(fd, mode);
}

ssize_t os::pd_write(int fd, const void *buf, size_t nBytes) {
  ssize_t res;
  RESTARTABLE(::write(fd, buf, nBytes), res);
  return res;
}

ssize_t os::read_at(int fd, void *buf, unsigned int nBytes, jlong offset) {
  return ::pread(fd, buf, nBytes, offset);
}

void os::flockfile(FILE* fp) {
  ::flockfile(fp);
}

void os::funlockfile(FILE* fp) {
  ::funlockfile(fp);
}

DIR* os::opendir(const char* dirname) {
  assert(dirname != nullptr, "just checking");
  return ::opendir(dirname);
}

struct dirent* os::readdir(DIR* dirp) {
  assert(dirp != nullptr, "just checking");
  return ::readdir(dirp);
}

int os::closedir(DIR *dirp) {
  assert(dirp != nullptr, "just checking");
  return ::closedir(dirp);
}

int os::socket_close(int fd) {
  return ::close(fd);
}

ssize_t os::recv(int fd, char* buf, size_t nBytes, uint flags) {
  RESTARTABLE_RETURN_SSIZE_T(::recv(fd, buf, nBytes, flags));
}

ssize_t os::send(int fd, char* buf, size_t nBytes, uint flags) {
  RESTARTABLE_RETURN_SSIZE_T(::send(fd, buf, nBytes, flags));
}

ssize_t os::raw_send(int fd, char* buf, size_t nBytes, uint flags) {
  return os::send(fd, buf, nBytes, flags);
}

ssize_t os::connect(int fd, struct sockaddr* him, socklen_t len) {
  RESTARTABLE_RETURN_SSIZE_T(::connect(fd, him, len));
}

void os::exit(int num) {
  permit_forbidden_function::exit(num);
}

void os::_exit(int num) {
  permit_forbidden_function::_exit(num);
}

void os::naked_yield() {
  sched_yield();
}

// Sleep forever; naked call to OS-specific sleep; use with CAUTION
void os::infinite_sleep() {
  while (true) {    // sleep forever ...
    ::sleep(100);   // ... 100 seconds at a time
  }
}

void os::naked_short_nanosleep(jlong ns) {
  struct timespec req;
  assert(ns > -1 && ns < NANOUNITS, "Un-interruptable sleep, short time use only");
  req.tv_sec = 0;
  req.tv_nsec = ns;
  ::nanosleep(&req, nullptr);
  return;
}

void os::naked_short_sleep(jlong ms) {
  assert(ms < MILLIUNITS, "Un-interruptable sleep, short time use only");
  os::naked_short_nanosleep(millis_to_nanos(ms));
  return;
}

char* os::Posix::describe_pthread_attr(char* buf, size_t buflen, const pthread_attr_t* attr) {
  size_t stack_size = 0;
  size_t guard_size = 0;
  int detachstate = 0;
  pthread_attr_getstacksize(attr, &stack_size);
  pthread_attr_getguardsize(attr, &guard_size);
  // Work around glibc stack guard issue, see os::create_thread() in os_linux.cpp.
  LINUX_ONLY(if (os::Linux::adjustStackSizeForGuardPages()) stack_size -= guard_size;)
  pthread_attr_getdetachstate(attr, &detachstate);
  jio_snprintf(buf, buflen, "stacksize: %zuk, guardsize: %zuk, %s",
    stack_size / K, guard_size / K,
    (detachstate == PTHREAD_CREATE_DETACHED ? "detached" : "joinable"));
  return buf;
}

char* os::realpath(const char* filename, char* outbuf, size_t outbuflen) {

  if (filename == nullptr || outbuf == nullptr || outbuflen < 1) {
    assert(false, "os::realpath: invalid arguments.");
    errno = EINVAL;
    return nullptr;
  }

  char* result = nullptr;

  // This assumes platform realpath() is implemented according to POSIX.1-2008.
  // POSIX.1-2008 allows to specify null for the output buffer, in which case
  // output buffer is dynamically allocated and must be ::free()'d by the caller.
  char* p = permit_forbidden_function::realpath(filename, nullptr);
  if (p != nullptr) {
    if (strlen(p) < outbuflen) {
      strcpy(outbuf, p);
      result = outbuf;
    } else {
      errno = ENAMETOOLONG;
    }
    permit_forbidden_function::free(p); // *not* os::free
  } else {
    // Fallback for platforms struggling with modern Posix standards (AIX 5.3, 6.1). If realpath
    // returns EINVAL, this may indicate that realpath is not POSIX.1-2008 compatible and
    // that it complains about the null we handed down as user buffer.
    // In this case, use the user provided buffer but at least check whether realpath caused
    // a memory overwrite.
    if (errno == EINVAL) {
      outbuf[outbuflen - 1] = '\0';
      p = permit_forbidden_function::realpath(filename, outbuf);
      if (p != nullptr) {
        guarantee(outbuf[outbuflen - 1] == '\0', "realpath buffer overwrite detected.");
        result = p;
      }
    }
  }
  return result;
}

int os::stat(const char *path, struct stat *sbuf) {
  return ::stat(path, sbuf);
}

char * os::native_path(char *path) {
  return path;
}

bool os::same_files(const char* file1, const char* file2) {
  if (file1 == nullptr && file2 == nullptr) {
    return true;
  }

  if (file1 == nullptr || file2 == nullptr) {
    return false;
  }

  if (strcmp(file1, file2) == 0) {
    return true;
  }

  bool is_same = false;
  struct stat st1;
  struct stat st2;

  if (os::stat(file1, &st1) < 0) {
    return false;
  }

  if (os::stat(file2, &st2) < 0) {
    return false;
  }

  if (st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino) {
    // same files
    is_same = true;
  }
  return is_same;
}

static char saved_jvm_path[MAXPATHLEN] = {0};

// Find the full path to the current module, libjvm.so
void os::jvm_path(char *buf, jint buflen) {
  // Error checking.
  if (buflen < MAXPATHLEN) {
    assert(false, "must use a large-enough buffer");
    buf[0] = '\0';
    return;
  }
  // Lazy resolve the path to current module.
  if (saved_jvm_path[0] != 0) {
    strcpy(buf, saved_jvm_path);
    return;
  }

  const char* fname;
#ifdef AIX
  Dl_info dlinfo;
  int ret = dladdr(CAST_FROM_FN_PTR(void *, os::jvm_path), &dlinfo);
  assert(ret != 0, "cannot locate libjvm");
  if (ret == 0) {
    return;
  }
  fname = dlinfo.dli_fname;
#else
  char dli_fname[MAXPATHLEN];
  dli_fname[0] = '\0';
  bool ret = dll_address_to_library_name(
                                         CAST_FROM_FN_PTR(address, os::jvm_path),
                                         dli_fname, sizeof(dli_fname), nullptr);
  assert(ret, "cannot locate libjvm");
  if (!ret) {
    return;
  }
  fname = dli_fname;
#endif // AIX
  char* rp = nullptr;
  if (fname[0] != '\0') {
    rp = os::realpath(fname, buf, buflen);
  }
  if (rp == nullptr) {
    return;
  }

  // If executing unit tests we require JAVA_HOME to point to the real JDK.
  if (Arguments::executing_unit_tests()) {
    // Look for JAVA_HOME in the environment.
    char* java_home_var = ::getenv("JAVA_HOME");
    if (java_home_var != nullptr && java_home_var[0] != 0) {

      // Check the current module name "libjvm.so".
      const char* p = strrchr(buf, '/');
      if (p == nullptr) {
        return;
      }
      assert(strstr(p, "/libjvm") == p, "invalid library name");

      stringStream ss(buf, buflen);
      rp = os::realpath(java_home_var, buf, buflen);
      if (rp == nullptr) {
        return;
      }

      assert((int)strlen(buf) < buflen, "Ran out of buffer room");
      ss.print("%s/lib", buf);

      // If the path exists within JAVA_HOME, add the VM variant directory and JVM
      // library name to complete the path to JVM being overridden.  Otherwise fallback
      // to the path to the current library.
      if (0 == access(buf, F_OK)) {
        // Use current module name "libjvm.so"
        ss.print("/%s/libjvm%s", Abstract_VM_Version::vm_variant(), JNI_LIB_SUFFIX);
        assert(strcmp(buf + strlen(buf) - strlen(JNI_LIB_SUFFIX), JNI_LIB_SUFFIX) == 0,
               "buf has been truncated");
      } else {
        // Go back to path of .so
        rp = os::realpath(fname, buf, buflen);
        if (rp == nullptr) {
          return;
        }
      }
    }
  }

  strncpy(saved_jvm_path, buf, MAXPATHLEN);
  saved_jvm_path[MAXPATHLEN - 1] = '\0';
}

// Called when creating the thread.  The minimum stack sizes have already been calculated
size_t os::Posix::get_initial_stack_size(ThreadType thr_type, size_t req_stack_size) {
  size_t stack_size;
  if (req_stack_size == 0) {
    stack_size = default_stack_size(thr_type);
  } else {
    stack_size = req_stack_size;
  }

  switch (thr_type) {
  case os::java_thread:
    // Java threads use ThreadStackSize which default value can be
    // changed with the flag -Xss
    if (req_stack_size == 0 && JavaThread::stack_size_at_create() > 0) {
      // no requested size and we have a more specific default value
      stack_size = JavaThread::stack_size_at_create();
    }
    stack_size = MAX2(stack_size,
                      _java_thread_min_stack_allowed);
    break;
  case os::compiler_thread:
    if (req_stack_size == 0 && CompilerThreadStackSize > 0) {
      // no requested size and we have a more specific default value
      stack_size = (size_t)(CompilerThreadStackSize * K);
    }
    stack_size = MAX2(stack_size,
                      _compiler_thread_min_stack_allowed);
    break;
  case os::vm_thread:
  case os::gc_thread:
  case os::watcher_thread:
  default:  // presume the unknown thr_type is a VM internal
    if (req_stack_size == 0 && VMThreadStackSize > 0) {
      // no requested size and we have a more specific default value
      stack_size = (size_t)(VMThreadStackSize * K);
    }

    stack_size = MAX2(stack_size,
                      _vm_internal_thread_min_stack_allowed);
    break;
  }

  // pthread_attr_setstacksize() may require that the size be rounded up to the OS page size.
  // Be careful not to round up to 0. Align down in that case.
  if (stack_size <= SIZE_MAX - vm_page_size()) {
    stack_size = align_up(stack_size, vm_page_size());
  } else {
    stack_size = align_down(stack_size, vm_page_size());
  }

  return stack_size;
}

#ifndef ZERO
#ifndef ARM
static bool get_frame_at_stack_banging_point(JavaThread* thread, address pc, const void* ucVoid, frame* fr) {
  if (Interpreter::contains(pc)) {
    // interpreter performs stack banging after the fixed frame header has
    // been generated while the compilers perform it before. To maintain
    // semantic consistency between interpreted and compiled frames, the
    // method returns the Java sender of the current frame.
    *fr = os::fetch_frame_from_context(ucVoid);
    if (!fr->is_first_java_frame()) {
      // get_frame_at_stack_banging_point() is only called when we
      // have well defined stacks so java_sender() calls do not need
      // to assert safe_for_sender() first.
      *fr = fr->java_sender();
    }
  } else {
    // more complex code with compiled code
    assert(!Interpreter::contains(pc), "Interpreted methods should have been handled above");
    CodeBlob* cb = CodeCache::find_blob(pc);
    if (cb == nullptr || !cb->is_nmethod() || cb->is_frame_complete_at(pc)) {
      // Not sure where the pc points to, fallback to default
      // stack overflow handling
      return false;
    } else {
      // in compiled code, the stack banging is performed just after the return pc
      // has been pushed on the stack
      *fr = os::fetch_compiled_frame_from_context(ucVoid);
      if (!fr->is_java_frame()) {
        assert(!fr->is_first_frame(), "Safety check");
        // See java_sender() comment above.
        *fr = fr->java_sender();
      }
    }
  }
  assert(fr->is_java_frame(), "Safety check");
  return true;
}
#endif // ARM

// This return true if the signal handler should just continue, ie. return after calling this
bool os::Posix::handle_stack_overflow(JavaThread* thread, address addr, address pc,
                                      const void* ucVoid, address* stub) {
  // stack overflow
  StackOverflow* overflow_state = thread->stack_overflow_state();
  if (overflow_state->in_stack_yellow_reserved_zone(addr)) {
    if (thread->thread_state() == _thread_in_Java) {
#ifndef ARM
      // arm32 doesn't have this
      // vthreads don't support this
      if (!thread->is_vthread_mounted() && overflow_state->in_stack_reserved_zone(addr)) {
        frame fr;
        if (get_frame_at_stack_banging_point(thread, pc, ucVoid, &fr)) {
          assert(fr.is_java_frame(), "Must be a Java frame");
          frame activation =
            SharedRuntime::look_for_reserved_stack_annotated_method(thread, fr);
          if (activation.sp() != nullptr) {
            overflow_state->disable_stack_reserved_zone();
            if (activation.is_interpreted_frame()) {
              overflow_state->set_reserved_stack_activation((address)(activation.fp()
                // Some platforms use frame pointers for interpreter frames, others use initial sp.
#if !defined(PPC64) && !defined(S390)
                + frame::interpreter_frame_initial_sp_offset
#endif
                ));
            } else {
              overflow_state->set_reserved_stack_activation((address)activation.unextended_sp());
            }
            return true; // just continue
          }
        }
      }
#endif // ARM
      // Throw a stack overflow exception.  Guard pages will be re-enabled
      // while unwinding the stack.
      overflow_state->disable_stack_yellow_reserved_zone();
      *stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::STACK_OVERFLOW);
    } else {
      // Thread was in the vm or native code.  Return and try to finish.
      overflow_state->disable_stack_yellow_reserved_zone();
      return true; // just continue
    }
  } else if (overflow_state->in_stack_red_zone(addr)) {
    // Fatal red zone violation. Disable the guard pages and keep
    // on handling the signal.
    overflow_state->disable_stack_red_zone();
    tty->print_raw_cr("An irrecoverable stack overflow has occurred.");

    // This is a likely cause, but hard to verify. Let's just print
    // it as a hint.
    tty->print_raw_cr("Please check if any of your loaded .so files has "
                      "enabled executable stack (see man page execstack(8))");

  } else {
#ifdef LINUX
    // This only works with os::Linux::manually_expand_stack()

    // Accessing stack address below sp may cause SEGV if current
    // thread has MAP_GROWSDOWN stack. This should only happen when
    // current thread was created by user code with MAP_GROWSDOWN flag
    // and then attached to VM. See notes in os_linux.cpp.
    if (thread->osthread()->expanding_stack() == 0) {
       thread->osthread()->set_expanding_stack();
       if (os::Linux::manually_expand_stack(thread, addr)) {
         thread->osthread()->clear_expanding_stack();
         return true; // just continue
       }
       thread->osthread()->clear_expanding_stack();
    } else {
       fatal("recursive segv. expanding stack.");
    }
#else
    tty->print_raw_cr("SIGSEGV happened inside stack but outside yellow and red zone.");
#endif // LINUX
  }
  return false;
}
#endif // ZERO

bool os::Posix::is_root(uid_t uid){
    return ROOT_UID == uid;
}

bool os::Posix::matches_effective_uid_or_root(uid_t uid) {
    return is_root(uid) || geteuid() == uid;
}

bool os::Posix::matches_effective_uid_and_gid_or_root(uid_t uid, gid_t gid) {
    return is_root(uid) || (geteuid() == uid && getegid() == gid);
}

// Shared clock/time and other supporting routines for pthread_mutex/cond
// initialization. This is enabled on Solaris but only some of the clock/time
// functionality is actually used there.

// Shared condattr object for use with relative timed-waits. Will be associated
// with CLOCK_MONOTONIC if available to avoid issues with time-of-day changes,
// but otherwise whatever default is used by the platform - generally the
// time-of-day clock.
static pthread_condattr_t _condAttr[1];

// Shared mutexattr to explicitly set the type to PTHREAD_MUTEX_NORMAL as not
// all systems (e.g. FreeBSD) map the default to "normal".
static pthread_mutexattr_t _mutexAttr[1];

// common basic initialization that is always supported
static void pthread_init_common(void) {
  int status;
  if ((status = pthread_condattr_init(_condAttr)) != 0) {
    fatal("pthread_condattr_init: %s", os::strerror(status));
  }
  if ((status = pthread_mutexattr_init(_mutexAttr)) != 0) {
    fatal("pthread_mutexattr_init: %s", os::strerror(status));
  }
  if ((status = pthread_mutexattr_settype(_mutexAttr, PTHREAD_MUTEX_NORMAL)) != 0) {
    fatal("pthread_mutexattr_settype: %s", os::strerror(status));
  }
  PlatformMutex::init();
}

static int (*_pthread_condattr_setclock)(pthread_condattr_t *, clockid_t) = nullptr;

static bool _use_clock_monotonic_condattr = false;

// Determine what POSIX API's are present and do appropriate
// configuration.
void os::Posix::init(void) {
#if defined(_ALLBSD_SOURCE)
  clock_tics_per_sec = CLK_TCK;
#else
  clock_tics_per_sec = checked_cast<int>(sysconf(_SC_CLK_TCK));
#endif
  // NOTE: no logging available when this is called. Put logging
  // statements in init_2().

  // Check for pthread_condattr_setclock support.

  // libpthread is already loaded.
  int (*condattr_setclock_func)(pthread_condattr_t*, clockid_t) =
    (int (*)(pthread_condattr_t*, clockid_t))dlsym(RTLD_DEFAULT,
                                                   "pthread_condattr_setclock");
  if (condattr_setclock_func != nullptr) {
    _pthread_condattr_setclock = condattr_setclock_func;
  }

  // Now do general initialization.

  pthread_init_common();

  int status;
  if (_pthread_condattr_setclock != nullptr) {
    if ((status = _pthread_condattr_setclock(_condAttr, CLOCK_MONOTONIC)) != 0) {
      if (status == EINVAL) {
        _use_clock_monotonic_condattr = false;
        warning("Unable to use monotonic clock with relative timed-waits" \
                " - changes to the time-of-day clock may have adverse affects");
      } else {
        fatal("pthread_condattr_setclock: %s", os::strerror(status));
      }
    } else {
      _use_clock_monotonic_condattr = true;
    }
  }

  initial_time_count = javaTimeNanos();
}

void os::Posix::init_2(void) {
  log_info(os)("Use of CLOCK_MONOTONIC is supported");
  log_info(os)("Use of pthread_condattr_setclock is%s supported",
               (_pthread_condattr_setclock != nullptr ? "" : " not"));
  log_info(os)("Relative timed-wait using pthread_cond_timedwait is associated with %s",
               _use_clock_monotonic_condattr ? "CLOCK_MONOTONIC" : "the default clock");
}

int os::Posix::clock_tics_per_second() {
  return clock_tics_per_sec;
}

#ifdef ASSERT
bool os::Posix::ucontext_is_interpreter(const ucontext_t* uc) {
  assert(uc != nullptr, "invariant");
  address pc = os::Posix::ucontext_get_pc(uc);
  assert(pc != nullptr, "invariant");
  return Interpreter::contains(pc);
}
#endif

// Utility to convert the given timeout to an absolute timespec
// (based on the appropriate clock) to use with pthread_cond_timewait,
// and sem_timedwait().
// The clock queried here must be the clock used to manage the
// timeout of the condition variable or semaphore.
//
// The passed in timeout value is either a relative time in nanoseconds
// or an absolute time in milliseconds. A relative timeout will be
// associated with CLOCK_MONOTONIC if available, unless the real-time clock
// is explicitly requested; otherwise, or if absolute,
// the default time-of-day clock will be used.

// Given time is a 64-bit value and the time_t used in the timespec is
// sometimes a signed-32-bit value we have to watch for overflow if times
// way in the future are given. Further on Solaris versions
// prior to 10 there is a restriction (see cond_timedwait) that the specified
// number of seconds, in abstime, is less than current_time + 100000000.
// As it will be over 20 years before "now + 100000000" will overflow we can
// ignore overflow and just impose a hard-limit on seconds using the value
// of "now + 100000000". This places a limit on the timeout of about 3.17
// years from "now".
//
#define MAX_SECS 100000000

// Calculate a new absolute time that is "timeout" nanoseconds from "now".
// "unit" indicates the unit of "now_part_sec" (may be nanos or micros depending
// on which clock API is being used).
static void calc_rel_time(timespec* abstime, jlong timeout, jlong now_sec,
                          jlong now_part_sec, jlong unit) {
  time_t max_secs = now_sec + MAX_SECS;

  jlong seconds = timeout / NANOUNITS;
  timeout %= NANOUNITS; // remaining nanos

  if (seconds >= MAX_SECS) {
    // More seconds than we can add, so pin to max_secs.
    abstime->tv_sec = max_secs;
    abstime->tv_nsec = 0;
  } else {
    abstime->tv_sec = now_sec  + seconds;
    long nanos = (now_part_sec * (NANOUNITS / unit)) + timeout;
    if (nanos >= NANOUNITS) { // overflow
      abstime->tv_sec += 1;
      nanos -= NANOUNITS;
    }
    abstime->tv_nsec = nanos;
  }
}

// Unpack the given deadline in milliseconds since the epoch, into the given timespec.
// The current time in seconds is also passed in to enforce an upper bound as discussed above.
static void unpack_abs_time(timespec* abstime, jlong deadline, jlong now_sec) {
  time_t max_secs = now_sec + MAX_SECS;

  jlong seconds = deadline / MILLIUNITS;
  jlong millis = deadline % MILLIUNITS;

  if (seconds >= max_secs) {
    // Absolute seconds exceeds allowed max, so pin to max_secs.
    abstime->tv_sec = max_secs;
    abstime->tv_nsec = 0;
  } else {
    abstime->tv_sec = seconds;
    abstime->tv_nsec = millis_to_nanos(millis);
  }
}

static jlong millis_to_nanos_bounded(jlong millis) {
  // We have to watch for overflow when converting millis to nanos,
  // but if millis is that large then we will end up limiting to
  // MAX_SECS anyway, so just do that here.
  if (millis / MILLIUNITS > MAX_SECS) {
    millis = jlong(MAX_SECS) * MILLIUNITS;
  }
  return millis_to_nanos(millis);
}

static void to_abstime(timespec* abstime, jlong timeout,
                       bool isAbsolute, bool isRealtime) {
  DEBUG_ONLY(time_t max_secs = MAX_SECS;)

  if (timeout < 0) {
    timeout = 0;
  }

  clockid_t clock = CLOCK_MONOTONIC;
  if (isAbsolute || (!_use_clock_monotonic_condattr || isRealtime)) {
    clock = CLOCK_REALTIME;
  }

  struct timespec now;
  int status = clock_gettime(clock, &now);
  assert(status == 0, "clock_gettime error: %s", os::strerror(errno));

  if (!isAbsolute) {
    calc_rel_time(abstime, timeout, now.tv_sec, now.tv_nsec, NANOUNITS);
  } else {
    unpack_abs_time(abstime, timeout, now.tv_sec);
  }
  DEBUG_ONLY(max_secs += now.tv_sec;)

  assert(abstime->tv_sec >= 0, "tv_sec < 0");
  assert(abstime->tv_sec <= max_secs, "tv_sec > max_secs");
  assert(abstime->tv_nsec >= 0, "tv_nsec < 0");
  assert(abstime->tv_nsec < NANOUNITS, "tv_nsec >= NANOUNITS");
}

// Create an absolute time 'millis' milliseconds in the future, using the
// real-time (time-of-day) clock. Used by PosixSemaphore.
void os::Posix::to_RTC_abstime(timespec* abstime, int64_t millis) {
  to_abstime(abstime, millis_to_nanos_bounded(millis),
             false /* not absolute */,
             true  /* use real-time clock */);
}

// Common (partly) shared time functions

jlong os::javaTimeMillis() {
  struct timespec ts;
  int status = clock_gettime(CLOCK_REALTIME, &ts);
  assert(status == 0, "clock_gettime error: %s", os::strerror(errno));
  return jlong(ts.tv_sec) * MILLIUNITS +
    jlong(ts.tv_nsec) / NANOUNITS_PER_MILLIUNIT;
}

void os::javaTimeSystemUTC(jlong &seconds, jlong &nanos) {
  struct timespec ts;
  int status = clock_gettime(CLOCK_REALTIME, &ts);
  assert(status == 0, "clock_gettime error: %s", os::strerror(errno));
  seconds = jlong(ts.tv_sec);
  nanos = jlong(ts.tv_nsec);
}

// macOS and AIX have platform specific implementations for javaTimeNanos()
// using native clock/timer access APIs. These have historically worked well
// for those platforms, but it may be possible for them to switch to the
// generic clock_gettime mechanism in the future.
#if !defined(__APPLE__) && !defined(AIX)

jlong os::javaTimeNanos() {
  struct timespec tp;
  int status = clock_gettime(CLOCK_MONOTONIC, &tp);
  assert(status == 0, "clock_gettime error: %s", os::strerror(errno));
  jlong result = jlong(tp.tv_sec) * NANOSECS_PER_SEC + jlong(tp.tv_nsec);
  return result;
}

void os::javaTimeNanos_info(jvmtiTimerInfo *info_ptr) {
  // CLOCK_MONOTONIC - amount of time since some arbitrary point in the past
  info_ptr->max_value = all_bits_jlong;
  info_ptr->may_skip_backward = false;      // not subject to resetting or drifting
  info_ptr->may_skip_forward = false;       // not subject to resetting or drifting
  info_ptr->kind = JVMTI_TIMER_ELAPSED;     // elapsed not CPU time
}
#endif // ! APPLE && !AIX

// Time since start-up in seconds to a fine granularity.
double os::elapsedTime() {
  return ((double)os::elapsed_counter()) / (double)os::elapsed_frequency(); // nanosecond resolution
}

jlong os::elapsed_counter() {
  return os::javaTimeNanos() - initial_time_count;
}

jlong os::elapsed_frequency() {
  return NANOSECS_PER_SEC; // nanosecond resolution
}

double os::elapsed_process_cpu_time() {
  struct rusage usage;
  int retval = getrusage(RUSAGE_SELF, &usage);
  if (retval == 0) {
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / (1000.0 * 1000.0);
  } else {
    return -1;
  }
}

// Return the real, user, and system times in seconds from an
// arbitrary fixed point in the past.
bool os::getTimesSecs(double* process_real_time,
                      double* process_user_time,
                      double* process_system_time) {
  struct tms ticks;
  clock_t real_ticks = times(&ticks);

  if (real_ticks == (clock_t) (-1)) {
    return false;
  } else {
    double ticks_per_second = (double) clock_tics_per_sec;
    *process_user_time = ((double) ticks.tms_utime) / ticks_per_second;
    *process_system_time = ((double) ticks.tms_stime) / ticks_per_second;
    *process_real_time = ((double) real_ticks) / ticks_per_second;

    return true;
  }
}

char * os::local_time_string(char *buf, size_t buflen) {
  struct tm t;
  time_t long_time;
  time(&long_time);
  localtime_r(&long_time, &t);
  jio_snprintf(buf, buflen, "%d-%02d-%02d %02d:%02d:%02d",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
               t.tm_hour, t.tm_min, t.tm_sec);
  return buf;
}

struct tm* os::localtime_pd(const time_t* clock, struct tm*  res) {
  return localtime_r(clock, res);
}

// PlatformEvent
//
// Assumption:
//    Only one parker can exist on an event, which is why we allocate
//    them per-thread. Multiple unparkers can coexist.
//
// _event serves as a restricted-range semaphore.
//   -1 : thread is blocked, i.e. there is a waiter
//    0 : neutral: thread is running or ready,
//        could have been signaled after a wait started
//    1 : signaled - thread is running or ready
//
//    Having three states allows for some detection of bad usage - see
//    comments on unpark().

PlatformEvent::PlatformEvent() {
  int status = pthread_cond_init(_cond, _condAttr);
  assert_status(status == 0, status, "cond_init");
  status = pthread_mutex_init(_mutex, _mutexAttr);
  assert_status(status == 0, status, "mutex_init");
  _event   = 0;
  _nParked = 0;
}

void PlatformEvent::park() {       // AKA "down()"
  // Transitions for _event:
  //   -1 => -1 : illegal
  //    1 =>  0 : pass - return immediately
  //    0 => -1 : block; then set _event to 0 before returning

  // Invariant: Only the thread associated with the PlatformEvent
  // may call park().
  assert(_nParked == 0, "invariant");

  int v;

  // atomically decrement _event
  for (;;) {
    v = _event;
    if (Atomic::cmpxchg(&_event, v, v - 1) == v) break;
  }
  guarantee(v >= 0, "invariant");

  if (v == 0) { // Do this the hard way by blocking ...
    int status = pthread_mutex_lock(_mutex);
    assert_status(status == 0, status, "mutex_lock");
    guarantee(_nParked == 0, "invariant");
    ++_nParked;
    while (_event < 0) {
      // OS-level "spurious wakeups" are ignored
      status = pthread_cond_wait(_cond, _mutex);
      assert_status(status == 0 MACOS_ONLY(|| status == ETIMEDOUT),
                    status, "cond_wait");
    }
    --_nParked;

    _event = 0;
    status = pthread_mutex_unlock(_mutex);
    assert_status(status == 0, status, "mutex_unlock");
    // Paranoia to ensure our locked and lock-free paths interact
    // correctly with each other.
    OrderAccess::fence();
  }
  guarantee(_event >= 0, "invariant");
}

int PlatformEvent::park(jlong millis) {
  return park_nanos(millis_to_nanos_bounded(millis));
}

int PlatformEvent::park_nanos(jlong nanos) {
  assert(nanos > 0, "nanos are positive");

  // Transitions for _event:
  //   -1 => -1 : illegal
  //    1 =>  0 : pass - return immediately
  //    0 => -1 : block; then set _event to 0 before returning

  // Invariant: Only the thread associated with the Event/PlatformEvent
  // may call park().
  assert(_nParked == 0, "invariant");

  int v;
  // atomically decrement _event
  for (;;) {
    v = _event;
    if (Atomic::cmpxchg(&_event, v, v - 1) == v) break;
  }
  guarantee(v >= 0, "invariant");

  if (v == 0) { // Do this the hard way by blocking ...
    struct timespec abst;
    to_abstime(&abst, nanos, false, false);

    int ret = OS_TIMEOUT;
    int status = pthread_mutex_lock(_mutex);
    assert_status(status == 0, status, "mutex_lock");
    guarantee(_nParked == 0, "invariant");
    ++_nParked;

    while (_event < 0) {
      status = pthread_cond_timedwait(_cond, _mutex, &abst);
      assert_status(status == 0 || status == ETIMEDOUT,
                    status, "cond_timedwait");
      // OS-level "spurious wakeups" are ignored
      if (status == ETIMEDOUT) break;
    }
    --_nParked;

    if (_event >= 0) {
      ret = OS_OK;
    }

    _event = 0;
    status = pthread_mutex_unlock(_mutex);
    assert_status(status == 0, status, "mutex_unlock");
    // Paranoia to ensure our locked and lock-free paths interact
    // correctly with each other.
    OrderAccess::fence();
    return ret;
  }
  return OS_OK;
}

void PlatformEvent::unpark() {
  // Transitions for _event:
  //    0 => 1 : just return
  //    1 => 1 : just return
  //   -1 => either 0 or 1; must signal target thread
  //         That is, we can safely transition _event from -1 to either
  //         0 or 1.
  // See also: "Semaphores in Plan 9" by Mullender & Cox
  //
  // Note: Forcing a transition from "-1" to "1" on an unpark() means
  // that it will take two back-to-back park() calls for the owning
  // thread to block. This has the benefit of forcing a spurious return
  // from the first park() call after an unpark() call which will help
  // shake out uses of park() and unpark() without checking state conditions
  // properly. This spurious return doesn't manifest itself in any user code
  // but only in the correctly written condition checking loops of ObjectMonitor,
  // Mutex/Monitor, and JavaThread::sleep

  if (Atomic::xchg(&_event, 1) >= 0) return;

  int status = pthread_mutex_lock(_mutex);
  assert_status(status == 0, status, "mutex_lock");
  int anyWaiters = _nParked;
  assert(anyWaiters == 0 || anyWaiters == 1, "invariant");
  status = pthread_mutex_unlock(_mutex);
  assert_status(status == 0, status, "mutex_unlock");

  // Note that we signal() *after* dropping the lock for "immortal" Events.
  // This is safe and avoids a common class of futile wakeups.  In rare
  // circumstances this can cause a thread to return prematurely from
  // cond_{timed}wait() but the spurious wakeup is benign and the victim
  // will simply re-test the condition and re-park itself.
  // This provides particular benefit if the underlying platform does not
  // provide wait morphing.

  if (anyWaiters != 0) {
    status = pthread_cond_signal(_cond);
    assert_status(status == 0, status, "cond_signal");
  }
}

// JSR166 support

 PlatformParker::PlatformParker() : _counter(0), _cur_index(-1) {
  int status = pthread_cond_init(&_cond[REL_INDEX], _condAttr);
  assert_status(status == 0, status, "cond_init rel");
  status = pthread_cond_init(&_cond[ABS_INDEX], nullptr);
  assert_status(status == 0, status, "cond_init abs");
  status = pthread_mutex_init(_mutex, _mutexAttr);
  assert_status(status == 0, status, "mutex_init");
}

PlatformParker::~PlatformParker() {
  int status = pthread_cond_destroy(&_cond[REL_INDEX]);
  assert_status(status == 0, status, "cond_destroy rel");
  status = pthread_cond_destroy(&_cond[ABS_INDEX]);
  assert_status(status == 0, status, "cond_destroy abs");
  status = pthread_mutex_destroy(_mutex);
  assert_status(status == 0, status, "mutex_destroy");
}

// Parker::park decrements count if > 0, else does a condvar wait.  Unpark
// sets count to 1 and signals condvar.  Only one thread ever waits
// on the condvar. Contention seen when trying to park implies that someone
// is unparking you, so don't wait. And spurious returns are fine, so there
// is no need to track notifications.

void Parker::park(bool isAbsolute, jlong time) {

  // Optional fast-path check:
  // Return immediately if a permit is available.
  // We depend on Atomic::xchg() having full barrier semantics
  // since we are doing a lock-free update to _counter.
  if (Atomic::xchg(&_counter, 0) > 0) return;

  JavaThread *jt = JavaThread::current();

  // Optional optimization -- avoid state transitions if there's
  // an interrupt pending.
  if (jt->is_interrupted(false)) {
    return;
  }

  // Next, demultiplex/decode time arguments
  struct timespec absTime;
  if (time < 0 || (isAbsolute && time == 0)) { // don't wait at all
    return;
  }
  if (time > 0) {
    to_abstime(&absTime, time, isAbsolute, false);
  }

  // Enter safepoint region
  // Beware of deadlocks such as 6317397.
  // The per-thread Parker:: mutex is a classic leaf-lock.
  // In particular a thread must never block on the Threads_lock while
  // holding the Parker:: mutex.  If safepoints are pending both the
  // the ThreadBlockInVM() CTOR and DTOR may grab Threads_lock.
  ThreadBlockInVM tbivm(jt);

  // Can't access interrupt state now that we are _thread_blocked. If we've
  // been interrupted since we checked above then _counter will be > 0.

  // Don't wait if cannot get lock since interference arises from
  // unparking.
  if (pthread_mutex_trylock(_mutex) != 0) {
    return;
  }

  int status;
  if (_counter > 0)  { // no wait needed
    _counter = 0;
    status = pthread_mutex_unlock(_mutex);
    assert_status(status == 0, status, "invariant");
    // Paranoia to ensure our locked and lock-free paths interact
    // correctly with each other and Java-level accesses.
    OrderAccess::fence();
    return;
  }

  OSThreadWaitState osts(jt->osthread(), false /* not Object.wait() */);

  assert(_cur_index == -1, "invariant");
  if (time == 0) {
    _cur_index = REL_INDEX; // arbitrary choice when not timed
    status = pthread_cond_wait(&_cond[_cur_index], _mutex);
    assert_status(status == 0 MACOS_ONLY(|| status == ETIMEDOUT),
                  status, "cond_wait");
  }
  else {
    _cur_index = isAbsolute ? ABS_INDEX : REL_INDEX;
    status = pthread_cond_timedwait(&_cond[_cur_index], _mutex, &absTime);
    assert_status(status == 0 || status == ETIMEDOUT,
                  status, "cond_timedwait");
  }
  _cur_index = -1;

  _counter = 0;
  status = pthread_mutex_unlock(_mutex);
  assert_status(status == 0, status, "invariant");
  // Paranoia to ensure our locked and lock-free paths interact
  // correctly with each other and Java-level accesses.
  OrderAccess::fence();
}

void Parker::unpark() {
  int status = pthread_mutex_lock(_mutex);
  assert_status(status == 0, status, "invariant");
  const int s = _counter;
  _counter = 1;
  // must capture correct index before unlocking
  int index = _cur_index;
  status = pthread_mutex_unlock(_mutex);
  assert_status(status == 0, status, "invariant");

  // Note that we signal() *after* dropping the lock for "immortal" Events.
  // This is safe and avoids a common class of futile wakeups.  In rare
  // circumstances this can cause a thread to return prematurely from
  // cond_{timed}wait() but the spurious wakeup is benign and the victim
  // will simply re-test the condition and re-park itself.
  // This provides particular benefit if the underlying platform does not
  // provide wait morphing.

  if (s < 1 && index != -1) {
    // thread is definitely parked
    status = pthread_cond_signal(&_cond[index]);
    assert_status(status == 0, status, "invariant");
  }
}

// Platform Mutex/Monitor implementation

#if PLATFORM_MONITOR_IMPL_INDIRECT

PlatformMutex::Mutex::Mutex() : _next(nullptr) {
  int status = pthread_mutex_init(&_mutex, _mutexAttr);
  assert_status(status == 0, status, "mutex_init");
}

PlatformMutex::Mutex::~Mutex() {
  int status = pthread_mutex_destroy(&_mutex);
  assert_status(status == 0, status, "mutex_destroy");
}

pthread_mutex_t PlatformMutex::_freelist_lock;
PlatformMutex::Mutex* PlatformMutex::_mutex_freelist = nullptr;

void PlatformMutex::init() {
  int status = pthread_mutex_init(&_freelist_lock, _mutexAttr);
  assert_status(status == 0, status, "freelist lock init");
}

struct PlatformMutex::WithFreeListLocked : public StackObj {
  WithFreeListLocked() {
    int status = pthread_mutex_lock(&_freelist_lock);
    assert_status(status == 0, status, "freelist lock");
  }

  ~WithFreeListLocked() {
    int status = pthread_mutex_unlock(&_freelist_lock);
    assert_status(status == 0, status, "freelist unlock");
  }
};

PlatformMutex::PlatformMutex() {
  {
    WithFreeListLocked wfl;
    _impl = _mutex_freelist;
    if (_impl != nullptr) {
      _mutex_freelist = _impl->_next;
      _impl->_next = nullptr;
      return;
    }
  }
  _impl = new Mutex();
}

PlatformMutex::~PlatformMutex() {
  WithFreeListLocked wfl;
  assert(_impl->_next == nullptr, "invariant");
  _impl->_next = _mutex_freelist;
  _mutex_freelist = _impl;
}

PlatformMonitor::Cond::Cond() : _next(nullptr) {
  int status = pthread_cond_init(&_cond, _condAttr);
  assert_status(status == 0, status, "cond_init");
}

PlatformMonitor::Cond::~Cond() {
  int status = pthread_cond_destroy(&_cond);
  assert_status(status == 0, status, "cond_destroy");
}

PlatformMonitor::Cond* PlatformMonitor::_cond_freelist = nullptr;

PlatformMonitor::PlatformMonitor() {
  {
    WithFreeListLocked wfl;
    _impl = _cond_freelist;
    if (_impl != nullptr) {
      _cond_freelist = _impl->_next;
      _impl->_next = nullptr;
      return;
    }
  }
  _impl = new Cond();
}

PlatformMonitor::~PlatformMonitor() {
  WithFreeListLocked wfl;
  assert(_impl->_next == nullptr, "invariant");
  _impl->_next = _cond_freelist;
  _cond_freelist = _impl;
}

#else

PlatformMutex::PlatformMutex() {
  int status = pthread_mutex_init(&_mutex, _mutexAttr);
  assert_status(status == 0, status, "mutex_init");
}

PlatformMutex::~PlatformMutex() {
  int status = pthread_mutex_destroy(&_mutex);
  assert_status(status == 0, status, "mutex_destroy");
}

PlatformMonitor::PlatformMonitor() {
  int status = pthread_cond_init(&_cond, _condAttr);
  assert_status(status == 0, status, "cond_init");
}

PlatformMonitor::~PlatformMonitor() {
  int status = pthread_cond_destroy(&_cond);
  assert_status(status == 0, status, "cond_destroy");
}

#endif // PLATFORM_MONITOR_IMPL_INDIRECT

// Must already be locked
int PlatformMonitor::wait(uint64_t millis) {
  if (millis > 0) {
    struct timespec abst;
    // We have to watch for overflow when converting millis to nanos,
    // but if millis is that large then we will end up limiting to
    // MAX_SECS anyway, so just do that here. This also handles values
    // larger than int64_t max.
    if (millis / MILLIUNITS > MAX_SECS) {
      millis = uint64_t(MAX_SECS) * MILLIUNITS;
    }
    to_abstime(&abst, millis_to_nanos(int64_t(millis)), false, false);

    int ret = OS_TIMEOUT;
    int status = pthread_cond_timedwait(cond(), mutex(), &abst);
    assert_status(status == 0 || status == ETIMEDOUT,
                  status, "cond_timedwait");
    if (status == 0) {
      ret = OS_OK;
    }
    return ret;
  } else {
    int status = pthread_cond_wait(cond(), mutex());
    assert_status(status == 0 MACOS_ONLY(|| status == ETIMEDOUT),
                  status, "cond_wait");
    return OS_OK;
  }
}

// Darwin has no "environ" in a dynamic library.
#ifdef __APPLE__
  #define environ (*_NSGetEnviron())
#else
  extern char** environ;
#endif

char** os::get_environ() { return environ; }

// Run the specified command in a separate process. Return its exit value,
// or -1 on failure (e.g. can't fork a new process).
// Notes: -Unlike system(), this function can be called from signal handler. It
//         doesn't block SIGINT et al.
//        -this function is unsafe to use in non-error situations, mainly
//         because the child process will inherit all parent descriptors.
int os::fork_and_exec(const char* cmd) {
  const char* argv[4] = {"sh", "-c", cmd, nullptr};
  pid_t pid = -1;
  char** env = os::get_environ();
  // Note: cast is needed because posix_spawn() requires - for compatibility with ancient
  // C-code - a non-const argv/envp pointer array. But it is fine to hand in literal
  // strings and just cast the constness away. See also ProcessImpl_md.c.
  int rc = ::posix_spawn(&pid, "/bin/sh", nullptr, nullptr, (char**) argv, env);
  if (rc == 0) {
    int status;
    // Wait for the child process to exit.  This returns immediately if
    // the child has already exited. */
    while (::waitpid(pid, &status, 0) < 0) {
      switch (errno) {
      case ECHILD: return 0;
      case EINTR: break;
      default: return -1;
      }
    }
    if (WIFEXITED(status)) {
      // The child exited normally; get its exit code.
      return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      // The child exited because of a signal
      // The best value to return is 0x80 + signal number,
      // because that is what all Unix shells do, and because
      // it allows callers to distinguish between process exit and
      // process death by signal.
      return 0x80 + WTERMSIG(status);
    } else {
      // Unknown exit code; pass it through
      return status;
    }
  } else {
    // Don't log, we are inside error handling
    return -1;
  }
}

bool os::message_box(const char* title, const char* message) {
  int i;
  fdStream err(defaultStream::error_fd());
  for (i = 0; i < 78; i++) err.print_raw("=");
  err.cr();
  err.print_raw_cr(title);
  for (i = 0; i < 78; i++) err.print_raw("-");
  err.cr();
  err.print_raw_cr(message);
  for (i = 0; i < 78; i++) err.print_raw("=");
  err.cr();

  char buf[16];
  // Prevent process from exiting upon "read error" without consuming all CPU
  while (::read(0, buf, sizeof(buf)) <= 0) { ::sleep(100); }

  return buf[0] == 'y' || buf[0] == 'Y';
}

////////////////////////////////////////////////////////////////////////////////
// runtime exit support

// Note: os::shutdown() might be called very early during initialization, or
// called from signal handler. Before adding something to os::shutdown(), make
// sure it is async-safe and can handle partially initialized VM.
void os::shutdown() {

  // allow PerfMemory to attempt cleanup of any persistent resources
  perfMemory_exit();

  // needs to remove object in file system
  AttachListener::abort();

  // flush buffered output, finish log files
  ostream_abort();

  // Check for abort hook
  abort_hook_t abort_hook = Arguments::abort_hook();
  if (abort_hook != nullptr) {
    abort_hook();
  }

}

// Note: os::abort() might be called very early during initialization, or
// called from signal handler. Before adding something to os::abort(), make
// sure it is async-safe and can handle partially initialized VM.
// Also note we can abort while other threads continue to run, so we can
// easily trigger secondary faults in those threads. To reduce the likelihood
// of that we use _exit rather than exit, so that no atexit hooks get run.
// But note that os::shutdown() could also trigger secondary faults.
void os::abort(bool dump_core, const void* siginfo, const void* context) {
  os::shutdown();
  if (dump_core) {
    LINUX_ONLY(if (DumpPrivateMappingsInCore) ClassLoader::close_jrt_image();)
    ::abort(); // dump core
  }
  os::_exit(1);
}

// Die immediately, no exit hook, no abort hook, no cleanup.
// Dump a core file, if possible, for debugging.
void os::die() {
  if (TestUnresponsiveErrorHandler && !CreateCoredumpOnCrash) {
    // For TimeoutInErrorHandlingTest.java, we just kill the VM
    // and don't take the time to generate a core file.
    ::raise(SIGKILL);
    // ::raise is not noreturn, even though with SIGKILL it definitely won't
    // return.  Hence "fall through" to ::abort, which is declared noreturn.
  }
  ::abort();
}

const char* os::file_separator() { return "/"; }
const char* os::line_separator() { return "\n"; }
const char* os::path_separator() { return ":"; }

// Map file into memory; uses mmap().
// Notes:
// - if caller specifies addr, MAP_FIXED is used. That means existing
//   mappings will be replaced.
// - The file descriptor must be valid (to create anonymous mappings, use
//   os::reserve_memory()).
// Returns address to mapped memory, nullptr on error
char* os::pd_map_memory(int fd, const char* unused,
                        size_t file_offset, char *addr, size_t bytes,
                        bool read_only, bool allow_exec) {

  assert(fd != -1, "Specify a valid file descriptor");

  int prot;
  int flags = MAP_PRIVATE;

  if (read_only) {
    prot = PROT_READ;
  } else {
    prot = PROT_READ | PROT_WRITE;
  }

  if (allow_exec) {
    prot |= PROT_EXEC;
  }

  if (addr != nullptr) {
    flags |= MAP_FIXED;
  }

  char* mapped_address = (char*)mmap(addr, (size_t)bytes, prot, flags,
                                     fd, file_offset);
  if (mapped_address == MAP_FAILED) {
    return nullptr;
  }

  // If we did specify an address, and the mapping succeeded, it should
  // have returned that address since we specify MAP_FIXED
  assert(addr == nullptr || addr == mapped_address,
         "mmap+MAP_FIXED returned " PTR_FORMAT ", expected " PTR_FORMAT,
         p2i(mapped_address), p2i(addr));

  return mapped_address;
}

// Unmap a block of memory. Uses munmap.
bool os::pd_unmap_memory(char* addr, size_t bytes) {
  return munmap(addr, bytes) == 0;
}

#ifdef CAN_SHOW_REGISTERS_ON_ASSERT
static ucontext_t _saved_assert_context;
static bool _has_saved_context = false;
#endif // CAN_SHOW_REGISTERS_ON_ASSERT

void os::save_assert_context(const void* ucVoid) {
#ifdef CAN_SHOW_REGISTERS_ON_ASSERT
  assert(ucVoid != nullptr, "invariant");
  assert(!_has_saved_context, "invariant");
  memcpy(&_saved_assert_context, ucVoid, sizeof(ucontext_t));
  // on Linux ppc64, ucontext_t contains pointers into itself which have to be patched up
  //  after copying the context (see comment in sys/ucontext.h):
#if defined(PPC64)
  *((void**)&_saved_assert_context.uc_mcontext.regs) = &(_saved_assert_context.uc_mcontext.gp_regs);
#elif defined(AMD64)
  // In the copied version, fpregs should point to the copied contents.
  // Sanity check: fpregs should point into the context.
  if ((address)((const ucontext_t*)ucVoid)->uc_mcontext.fpregs > (address)ucVoid) {
    size_t fpregs_offset = pointer_delta(((const ucontext_t*)ucVoid)->uc_mcontext.fpregs, ucVoid, 1);
    if (fpregs_offset < sizeof(ucontext_t)) {
      // Preserve the offset.
      *((void**)&_saved_assert_context.uc_mcontext.fpregs) = (void*)((address)(void*)&_saved_assert_context + fpregs_offset);
    }
  }
#endif
  _has_saved_context = true;
#endif // CAN_SHOW_REGISTERS_ON_ASSERT
}

const void* os::get_saved_assert_context(const void** sigInfo) {
#ifdef CAN_SHOW_REGISTERS_ON_ASSERT
  assert(sigInfo != nullptr, "invariant");
  *sigInfo = nullptr;
  return _has_saved_context ? &_saved_assert_context : nullptr;
#endif
  *sigInfo = nullptr;
  return nullptr;
}
