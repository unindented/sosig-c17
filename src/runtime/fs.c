#include "runtime/fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/text.h"
#include "shared/arena.h"

/**
 * One directory on the active recursion path, identified by device and inode so a symlinked
 * directory whose target is one of its own ancestors is detected as a cycle and not followed.
 */
struct DirCrumb {
  dev_t dev;
  ino_t ino;
  const struct DirCrumb* parent;
};

/**
 * @brief Walks a directory tree and appends matching files to `paths` without sorting.
 *
 * @param paths      Path list that receives the matching paths. Must not be `NULL`.
 * @param dir_path   Directory to walk. The tree root on the outermost call. The subdirectory
 *                   being scanned on every recursive one. Must not be `NULL`.
 * @param suffix     Literal filename suffix to match. Must not be `NULL`.
 * @param ancestors  Device/inode chain of the directories enclosing `dir_path`, including
 *                   `dir_path` itself, used to detect and skip symlink cycles.
 * @param reason     Receives the failure reason, naming the directory that failed. May be `NULL`
 *                   only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a directory or allocation failure.
 */
static int fs_list_files_with_suffix_inner(struct PathList* paths,
                                           const char* dir_path,
                                           const char* suffix,
                                           const struct DirCrumb* ancestors,
                                           char* reason,
                                           size_t reason_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Appends one matching regular file to `paths` or descends into a subdirectory.
 *
 * @param paths      Path list that receives matching paths. Must not be `NULL`.
 * @param dir_path   Directory containing `entry_name`. Must not be `NULL`.
 * @param suffix     Literal filename suffix to match. Must not be `NULL`.
 * @param entry_name Bare directory entry name to inspect. Must not be `NULL`.
 * @param ancestors  Device/inode chain of `dir_path` and its ancestors, used to skip a subdirectory
 *                   that would form a symlink cycle.
 * @param scratch    Arena owning the joined path for this entry. Owned by the enclosing directory's
 *                   walk, which outlives any recursion into this entry. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success (including skipped entries and skipped cycles), or `-1` on failure.
 */
static int visit_matching_entry(struct PathList* paths,
                                const char* dir_path,
                                const char* suffix,
                                const char* entry_name,
                                const struct DirCrumb* ancestors,
                                struct Arena* scratch,
                                char* reason,
                                size_t reason_len) __attribute__((nonnull(1, 2, 3, 4, 6)));

/**
 * @brief Reports whether a directory identity already appears in the ancestor chain.
 *
 * @param ancestors Ancestor device/inode chain to search, innermost first. `NULL` is an empty
 *                  chain.
 * @param dev       Device id of the directory to look for.
 * @param ino       Inode number of the directory to look for.
 * @return `true` when `dev`/`ino` is already in `ancestors` (a directory cycle), `false` otherwise.
 */
static bool has_dir_crumb_id(const struct DirCrumb* ancestors, dev_t dev, ino_t ino);

/**
 * @brief Reports whether `text` ends with the literal `suffix`.
 *
 * @param text   Terminated string to test. Must not be `NULL`.
 * @param suffix Terminated suffix to look for. Must not be `NULL`.
 * @return `true` when `text` ends with `suffix`, `false` otherwise.
 */
static bool has_suffix(const char* text, const char* suffix) __attribute__((nonnull(1, 2)));

/**
 * @brief Compares two `PathList` item pointers for `qsort`.
 *
 * @param a Pointer to the first `char*` element. Must not be `NULL`.
 * @param b Pointer to the second `char*` element. Must not be `NULL`.
 * @return A negative, zero, or positive value per `strcmp` of the two paths.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Reads exactly `data_len` bytes into `data`, verifying the file did not change underneath.
 *
 * `stat` and `fread` are two observations of a file another process may be writing, so a read that
 * returns anything other than `data_len` bytes is a changed file rather than a partial success.
 * Both directions are rejected: too few bytes means the file shrank, and one readable byte past
 * `data_len` means it grew. An embedded `NUL` is rejected here too, so every caller may treat the
 * result as a C string.
 *
 * @param data       Buffer of at least `data_len` bytes that receives the file contents. Must not
 *                   be `NULL`.
 * @param data_len   Number of bytes to read, taken from the `stat` that preceded the open.
 * @param fp         Stream positioned at the start of the file. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` when exactly `data_len` stable, `NUL`-free bytes were read, or `-1` otherwise.
 */
static int fs_read_file_bytes(char* data,
                              size_t data_len,
                              FILE* fp,
                              char* reason,
                              size_t reason_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Creates the parent directory of `file_path` if it has one.
 *
 * @param file_path  Path whose parent directory should exist. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success or when the path has no directory component, or `-1` on failure.
 */
static int ensure_parent_dir(const char* file_path, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/**
 * @brief Creates one directory, accepting a path that already names a directory.
 *
 * Creates only the named directory. `fs_mkdir_p` walks the path and calls this per component.
 *
 * @param dir_path   Directory path to create. Must not be `NULL`.
 * @param reason     Receives the failure reason, naming `dir_path` since it is one component of a
 *                   path the caller named whole. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success or when `dir_path` already names a directory, or `-1` on failure.
 */
static int ensure_dir(const char* dir_path, char* reason, size_t reason_len)
    __attribute__((nonnull(1)));

/**
 * @brief Reports `error_number` alone as a failure reason.
 *
 * This is for a failure on the path the caller already names, where repeating it would only pad the
 * composed diagnostic.
 *
 * @param reason       Receives the reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len   Size of `reason` in bytes.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return fs_reason_errno(...);`.
 */
static int fs_reason_errno(char* reason, size_t reason_len, int error_number);

/**
 * @brief Reports `error_number` as a failure reason naming the operation and path that failed.
 *
 * This is for a failure on a path the caller cannot name, such as a directory reached partway
 * through a recursive walk or one component of a path created whole.
 *
 * @param reason       Receives the reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len   Size of `reason` in bytes.
 * @param action       Lowercase operation that failed, such as `open directory`. Must not be
 *                     `NULL`.
 * @param path         Path the operation was attempted on. Must not be `NULL`.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return fs_reason_path_errno(...);`.
 */
static int fs_reason_path_errno(char* reason,
                                size_t reason_len,
                                const char* action,
                                const char* path,
                                int error_number) __attribute__((nonnull(3, 4)));

int fs_list_files_with_suffix(struct PathList* paths,
                              const char* root_dir,
                              const char* suffix,
                              char* reason,
                              size_t reason_len) {
  struct stat root_st;
  // Seed the ancestor chain with the root's own identity. A symlink anywhere in the tree pointing
  // back at `root_dir` is the shortest cycle there is. This `stat` does not validate the root.
  // `opendir` in the walk below rejects a non-directory.
  if (stat(root_dir, &root_st) != 0) {
    return fs_reason_path_errno(reason, reason_len, "inspect directory", root_dir, errno);
  }
  const struct DirCrumb root_crumb = {.dev = root_st.st_dev, .ino = root_st.st_ino, .parent = NULL};
  if (fs_list_files_with_suffix_inner(paths, root_dir, suffix, &root_crumb, reason, reason_len) !=
      0) {
    return -1;
  }
  // Skip the empty case rather than let `qsort` see it. `path_list_init` leaves `items` `NULL`, and
  // passing a null pointer to `qsort` is undefined even with a count of 0.
  if (paths->count > 0) {
    qsort(paths->items, paths->count, sizeof(*paths->items), compare_paths);
  }
  return 0;
}

int fs_read_file(const char* file_path,
                 char** data_out,
                 size_t* data_len_out,
                 char* reason,
                 size_t reason_len) {
  struct stat st;
  if (stat(file_path, &st) != 0) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  if (!S_ISREG(st.st_mode)) {
    return error_report(reason, reason_len, "not a regular file");
  }
  if (st.st_size < 0) {
    return error_report(reason, reason_len, "has a negative size");
  }
  // Reserve one byte for the terminator the allocation below adds, so `size + 1` cannot wrap to 0
  // and hand back a buffer shorter than the read. Both sides widen to `uintmax_t` because the
  // comparison only binds where `off_t` is wider than `size_t`, as on a 32-bit target.
  if ((uintmax_t)st.st_size > (uintmax_t)SIZE_MAX - 1) {
    return error_report(reason, reason_len, "exceeds max readable size (%zu bytes) at %ju bytes",
                        SIZE_MAX - 1, (uintmax_t)st.st_size);
  }

  const size_t size = (size_t)st.st_size;
  FILE* fp = fopen(file_path, "rb");
  if (fp == NULL) {
    return fs_reason_errno(reason, reason_len, errno);
  }

  // Use `malloc`, not `calloc`. `fs_read_file_bytes` writes all `size` bytes or fails, so only the
  // terminator needs to be zero, and zero-filling first would mean a second pass over the largest
  // file this program reads.
  char* data = malloc(size + 1);
  int rc = -1;
  if (data == NULL) {
    (void)error_report(reason, reason_len, "out of memory");
  } else {
    data[size] = '\0';
    rc = fs_read_file_bytes(data, size, fp, reason, reason_len);
  }
  // Close before publishing so a close error fails the read and the buffer is still reclaimed.
  if (fclose(fp) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  if (rc == 0) {
    *data_out = data;
    // `fs_read_file_bytes` succeeds only on a full read, so the byte count is the stat size.
    *data_len_out = size;
    data = NULL;
  }

  free(data);
  return rc;
}

int fs_write_file(const char* file_path,
                  const char* data,
                  size_t data_len,
                  char* reason,
                  size_t reason_len) {
  if (ensure_parent_dir(file_path, reason, reason_len) != 0) {
    return -1;
  }
  FILE* fp = fopen(file_path, "wb");
  if (fp == NULL) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  const size_t written = fwrite(data, 1, data_len, fp);
  // Capture `errno` before `ferror`, which is permitted to modify it even when it succeeds.
  const int write_errno = errno;
  int rc = 0;
  if (written != data_len) {
    rc = ferror(fp) != 0
             ? fs_reason_errno(reason, reason_len, write_errno)
             : error_report(reason, reason_len, "wrote only %zu of %zu bytes", written, data_len);
  }
  if (fclose(fp) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  return rc;
}

int fs_mkdir_p(const char* dir_path, char* reason, size_t reason_len) {
  if (dir_path[0] == '\0') {
    return 0;
  }
  char* copy = text_strdup(dir_path);
  if (copy == NULL) {
    return error_report(reason, reason_len, "out of memory");
  }

  int rc = -1;
  // Start at the second byte so a leading `/` does not become `ensure_dir("")`, which would fail
  // with `ENOENT` on a root that always exists. The empty-path early return above keeps `copy + 1`
  // in bounds. For `""` it would point one past the terminator, and dereferencing it reads outside
  // the object.
  for (char* p = copy + 1;; p++) {
    if (*p != '/' && *p != '\0') {
      continue;
    }
    const char saved = *p;
    *p = '\0';
    if (ensure_dir(copy, reason, reason_len) != 0) {
      goto cleanup;
    }
    *p = saved;
    if (saved == '\0') {
      break;
    }
  }
  rc = 0;

cleanup:
  free(copy);
  return rc;
}

int fs_identify(const char* file_path, struct FsIdentity* identity_out) {
  struct stat st;
  if (stat(file_path, &st) != 0) {
    return -1;
  }
  identity_out->device = (uint64_t)st.st_dev;
  identity_out->inode = (uint64_t)st.st_ino;
  return 0;
}

static int fs_list_files_with_suffix_inner(struct PathList* paths,
                                           const char* dir_path,
                                           const char* suffix,
                                           const struct DirCrumb* ancestors,
                                           char* reason,
                                           size_t reason_len) {
  DIR* dp = opendir(dir_path);
  if (dp == NULL) {
    return fs_reason_path_errno(reason, reason_len, "open directory", dir_path, errno);
  }

  // This uses one arena for the whole directory rather than one per entry. Every joined path here
  // is a few hundred bytes, while an arena's first chunk is 8 KB, so a per-entry arena would mean
  // an 8 KB `malloc`/`free` pair for every file in the tree. The lifetime ends with this directory,
  // which is short enough to bound the memory and long enough for a subdirectory's path to stay
  // valid while the recursion below uses it as a root.
  struct Arena scratch;
  arena_init(&scratch);

  int rc = 0;
  while (rc == 0) {
    // `readdir` returns `NULL` both at end-of-directory and on error, so reset `errno` first to
    // tell them apart. A `NULL` entry with `errno` unchanged is the end. Otherwise the walk
    // failed.
    errno = 0;
    struct dirent* entry = readdir(dp);
    if (entry == NULL) {
      if (errno != 0) {
        rc = fs_reason_path_errno(reason, reason_len, "read directory", dir_path, errno);
      }
      break;
    }
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      rc = visit_matching_entry(paths, dir_path, suffix, entry->d_name, ancestors, &scratch, reason,
                                reason_len);
    }
  }
  arena_free(&scratch);
  if (closedir(dp) != 0) {
    if (rc == 0) {
      (void)fs_reason_path_errno(reason, reason_len, "close directory", dir_path, errno);
    }
    rc = -1;
  }
  return rc;
}

static int visit_matching_entry(struct PathList* paths,
                                const char* dir_path,
                                const char* suffix,
                                const char* entry_name,
                                const struct DirCrumb* ancestors,
                                struct Arena* scratch,
                                char* reason,
                                size_t reason_len) {
  char* file_path = path_join(dir_path, entry_name, scratch);
  if (file_path == NULL) {
    return error_report(reason, reason_len, "out of memory");
  }
  struct stat st;
  if (stat(file_path, &st) != 0) {
    if (errno == ENOENT || errno == ELOOP) {
      // This skips an entry that resolves to nothing rather than aborting the whole walk: a
      // dangling symlink or one removed since `readdir` (`ENOENT`), or a symlink that resolves in a
      // cycle (`ELOOP`). No file exists behind either, so the walk loses nothing by stepping past
      // it. `ENOENT` in particular is routine, not exceptional. `readdir` and `stat` cannot be
      // atomic, so any concurrent delete lands here and must not fail the build.
      return 0;
    }
    // Every other reason means a file is there and could not be inspected: `EACCES` from a parent
    // directory that is readable but not searchable, `ENAMETOOLONG`, `EIO`. Skipping those would
    // drop a content file from the build and still report success.
    return fs_reason_path_errno(reason, reason_len, "inspect entry", file_path, errno);
  }
  if (S_ISDIR(st.st_mode)) {
    if (has_dir_crumb_id(ancestors, st.st_dev, st.st_ino)) {
      // This skips a symlinked directory pointing back into its own ancestry. The walk already
      // reaches its contents through the real path, so nothing is lost.
      return 0;
    }
    const struct DirCrumb crumb = {.dev = st.st_dev, .ino = st.st_ino, .parent = ancestors};
    return fs_list_files_with_suffix_inner(paths, file_path, suffix, &crumb, reason, reason_len);
  }
  if (S_ISREG(st.st_mode) && has_suffix(entry_name, suffix)) {
    if (path_list_push(paths, file_path) != 0) {
      return error_report(reason, reason_len, "out of memory");
    }
  }
  return 0;
}

static bool has_dir_crumb_id(const struct DirCrumb* ancestors, dev_t dev, ino_t ino) {
  for (const struct DirCrumb* crumb = ancestors; crumb != NULL; crumb = crumb->parent) {
    if (crumb->dev == dev && crumb->ino == ino) {
      return true;
    }
  }
  return false;
}

static bool has_suffix(const char* text, const char* suffix) {
  const size_t text_len = strlen(text);
  const size_t suffix_len = strlen(suffix);
  // The length test must short-circuit the pointer arithmetic. `text_len - suffix_len` wraps when
  // the suffix is the longer string, and `text +` that value forms a pointer far outside the
  // object, which is undefined regardless of whether it is dereferenced.
  return text_len >= suffix_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

static int compare_paths(const void* a, const void* b) {
  const char* const* pa = a;
  const char* const* pb = b;
  return strcmp(*pa, *pb);
}

static int fs_read_file_bytes(char* data,
                              size_t data_len,
                              FILE* fp,
                              char* reason,
                              size_t reason_len) {
  const size_t nread = fread(data, 1, data_len, fp);
  // Capture `errno` before calling `ferror`, which is permitted to modify it even when it succeeds.
  // Reading it afterwards could name a cause the read never had.
  const int read_errno = errno;
  int rc = -1;
  if (ferror(fp) != 0) {
    (void)fs_reason_errno(reason, reason_len, read_errno);
  } else if (nread != data_len) {
    // No stream error, so the bytes ran out. The file shrank after the caller's `stat`.
    (void)error_report(reason, reason_len, "shrank while being read");
  } else if (feof(fp) != 0) {
    // The read already consumed the whole file, so it cannot have grown.
    rc = 0;
  } else {
    // `fread` stops at `data_len`, so a file that grew between the caller's `stat` and here would
    // read as a silently truncated copy. One more byte tells the two apart. Nothing left to read
    // means the size still matches, while any byte at all means the file changed underneath us.
    char extra = 0;
    const size_t extra_read = fread(&extra, 1, 1, fp);
    const int extra_errno = errno;
    if (ferror(fp) != 0) {
      (void)fs_reason_errno(reason, reason_len, extra_errno);
    } else if (extra_read == 0) {
      rc = 0;
    } else {
      (void)error_report(reason, reason_len, "grew while being read");
    }
  }
  // Text is the only thing this program reads, and the whole codebase recovers lengths with
  // `strlen`. A `NUL` here would silently truncate the rendered output instead.
  if (rc == 0 && memchr(data, '\0', nread) != NULL) {
    (void)error_report(reason, reason_len, "contains an embedded NUL byte");
    rc = -1;
  }
  return rc;
}

static int ensure_parent_dir(const char* file_path, char* reason, size_t reason_len) {
  const char* slash = strrchr(file_path, '/');
  if (slash == NULL) {
    return 0;
  }
  const size_t len = (size_t)(slash - file_path);
  // A path whose only `/` is the leading one yields a zero-length parent, which `fs_mkdir_p` treats
  // as a no-op. The parent is the root, which always exists.
  char* parent = malloc(len + 1);
  if (parent == NULL) {
    return error_report(reason, reason_len, "out of memory");
  }
  memcpy(parent, file_path, len);
  parent[len] = '\0';
  const int rc = fs_mkdir_p(parent, reason, reason_len);
  free(parent);
  return rc;
}

static int ensure_dir(const char* dir_path, char* reason, size_t reason_len) {
  // Create first and treat `EEXIST` as success, rather than testing with `stat` and then creating.
  // Between a test and the create another process can win the race, and `fs_mkdir_p` runs this once
  // per path component. Mode `0775` is a ceiling the process umask trims, not the mode the
  // directory ends up with. The `stat` below runs only once `EEXIST` says something is already
  // there, and rejects it when it is not a directory.
  if (mkdir(dir_path, 0775) == 0) {
    return 0;
  }
  if (errno != EEXIST) {
    return fs_reason_path_errno(reason, reason_len, "create directory", dir_path, errno);
  }
  struct stat st;
  if (stat(dir_path, &st) != 0) {
    return fs_reason_path_errno(reason, reason_len, "inspect directory", dir_path, errno);
  }
  if (!S_ISDIR(st.st_mode)) {
    // This is a bare cause fragment, unlike the two branches above, which name the operation. It
    // needs no verb because "exists and is not a directory" already says a directory was wanted.
    // The errno branches do carry one, because a caller may be writing a file rather than creating
    // a directory, and there the verb is what separates a failed parent directory from a failed
    // write. The path trails the cause either way, so a deep component cannot truncate it.
    return error_report(reason, reason_len, "exists and is not a directory ('%s')", dir_path);
  }
  return 0;
}

static int fs_reason_errno(char* reason, size_t reason_len, int error_number) {
  char message[FS_REASON_SIZE];
  return error_report(reason, reason_len, "%s",
                      error_system_message(message, sizeof(message), error_number));
}

static int fs_reason_path_errno(char* reason,
                                size_t reason_len,
                                const char* action,
                                const char* path,
                                int error_number) {
  char message[FS_REASON_SIZE];
  return error_report(reason, reason_len, "cannot %s: %s ('%s')", action,
                      error_system_message(message, sizeof(message), error_number), path);
}
