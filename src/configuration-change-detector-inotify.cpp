// Copyright (C) 2020  Matthew "strager" Glazar
// See end of file for extended copyright information.

#include <quick-lint-js/have.h>

#if QLJS_HAVE_INOTIFY

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <quick-lint-js/assert.h>
#include <quick-lint-js/configuration-change-detector.h>
#include <quick-lint-js/file-canonical.h>
#include <quick-lint-js/file-handle.h>
#include <quick-lint-js/file.h>
#include <quick-lint-js/narrow-cast.h>
#include <quick-lint-js/unreachable.h>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>
#include <vector>

// @@@ document caveats:
// [_] symlinks
// @@@ add a nuke feature to clear caches and reload all configs. or just have
// the client restart the LSP server...

namespace quick_lint_js {
namespace {
std::vector<posix_fd_file> garbage_inotify_fds;
}

configuration_filesystem_inotify::configuration_filesystem_inotify()
    : inotify_fd_(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) {
  QLJS_ASSERT(this->inotify_fd_.valid());
}

configuration_filesystem_inotify::~configuration_filesystem_inotify() {
  // HACK(strager): On Linux 5.4.86, close() becomes *very* slow (10
  // milliseconds or more) because it summons RCU synchronization demons.
  // (This performance problem only matters in tests.) More details:
  // https://lore.kernel.org/linux-fsdevel/CAC-ggsFLmFpz5Y=-9MMLwxuO2LOS9rhpewDp_-u2hrT9J79ryg@mail.gmail.com/
  //
  // Work around the slowness by deferring close() but manually clearing the
  // inotify.
  for (int watch_descriptor : this->watch_descriptors_) {
    int rc = ::inotify_rm_watch(this->inotify_fd_.get(), watch_descriptor);
    QLJS_ASSERT(rc == 0);
  }
  constexpr std::size_t closes_to_defer = 10;
  if (garbage_inotify_fds.size() > closes_to_defer) {
    garbage_inotify_fds.clear();  // Closes each fd.
  }
  garbage_inotify_fds.push_back(std::move(this->inotify_fd_));
}

canonical_path_result configuration_filesystem_inotify::canonicalize_path(
    const std::string& path) {
  return quick_lint_js::canonicalize_path(path);
}

void configuration_filesystem_inotify::enter_directory(
    const canonical_path& directory) {
  this->watch_directory(directory);
}

read_file_result configuration_filesystem_inotify::read_file(
    const canonical_path& directory, std::string_view file_name) {
  canonical_path config_path = directory;
  config_path.append_component(file_name);
  return quick_lint_js::read_file(config_path.c_str());
}

void configuration_filesystem_inotify::process_changes(
    configuration_change_detector_impl& detector,
    std::vector<configuration_change>* out_changes) {
  this->read_inotify();
  detector.refresh(out_changes);
}

::pollfd configuration_filesystem_inotify::get_notify_poll_fd() {
  return ::pollfd{
      .fd = this->inotify_fd_.get(),
      .events = POLLIN,
      .revents = 0,
  };
}

void configuration_filesystem_inotify::read_inotify() {
  union inotify_event_buffer {
    ::inotify_event event;
    char buffer[sizeof(::inotify_event) + NAME_MAX + 1];
  };

  // TODO(strager): Optimize syscall usage by calling read once with a big
  // buffer.
  for (;;) {
    inotify_event_buffer buffer;
    ssize_t rc = ::read(this->inotify_fd_.get(), &buffer, sizeof(buffer));
    QLJS_ASSERT(rc <= narrow_cast<ssize_t>(sizeof(buffer)));
    if (rc == -1) {
      int error = errno;
      if (error == EAGAIN) {
        // We read all of the queuedevents.
        break;
      }
      QLJS_UNIMPLEMENTED();
    }
    if (rc == 0) {
      QLJS_UNIMPLEMENTED();
    }
  }
}

void configuration_filesystem_inotify::watch_directory(
    const canonical_path& directory) {
  int watch_descriptor = ::inotify_add_watch(
      this->inotify_fd_.get(), directory.c_str(),
      IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
          IN_DELETE_SELF /*@@@*/ | IN_MODIFY | IN_MOVE_SELF /*@@@*/ |
          IN_EXCL_UNLINK | IN_ONLYDIR | IN_MOVED_FROM /*@@@*/ |
          IN_MOVED_TO /*@@@*/ | 0);
  if (watch_descriptor == -1) {
    std::fprintf(stderr, "fatal: inotify_add_watch failed: %s\n",
                 std::strerror(errno));
    QLJS_UNIMPLEMENTED();
  }
  // TODO(strager): Use a more efficient data structure, such as a sorted
  // interval set, for watch descriptors.
  if (std::find(this->watch_descriptors_.begin(),
                this->watch_descriptors_.end(),
                watch_descriptor) == this->watch_descriptors_.end()) {
    this->watch_descriptors_.emplace_back(watch_descriptor);
  }
}
}

#endif

// quick-lint-js finds bugs in JavaScript programs.
// Copyright (C) 2020  Matthew "strager" Glazar
//
// This file is part of quick-lint-js.
//
// quick-lint-js is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// quick-lint-js is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with quick-lint-js.  If not, see <https://www.gnu.org/licenses/>.
