#ifndef SOSIG_FS_H
#define SOSIG_FS_H

#include <stddef.h>
#include <stdint.h>

struct PathList;

/**
 * Identity of one existing file, as the device and inode pair naming it uniquely.
 *
 * Comparing identities answers "are these two paths the same file?" where comparing path strings
 * cannot. `./content/x.md` and `content/x.md` are the same file spelled two ways, and so are a
 * symlink and its target, a hard link and its twin, and two spellings that differ only in case on a
 * case-insensitive filesystem. These fields are wider than `dev_t` and `ino_t` so this header does
 * not put system types on its callers.
 */
struct FsIdentity {
  /** Device the file resides on. */
  uint64_t device;

  /** Inode number within `device`. */
  uint64_t inode;
};

/**
 * Size in bytes of a filesystem failure reason, including the `NUL` terminator.
 *
 * These actions report a reason fragment, not a whole diagnostic, because the caller owns the
 * operation and the attribution. One failed `fs_write_file` is `failed to write output` from the
 * content pass and `failed to write template output` from the aggregate and feed passes. The
 * fragment is lowercase and unquoted so it composes as `<caller's operation>: <reason>`, with any
 * unbounded path trailing the reason, never leading it. `ERROR_MESSAGE_SIZE` is smaller than a path
 * may be, so a leading path would truncate the cause off the end. This is sized for a system
 * message plus a directory path of ordinary depth, because the caller cannot name the failing
 * component. Only the walk knows how deep it got.
 */
enum { FS_REASON_SIZE = 256 };

/**
 * @brief Recursively lists files under `root_dir` whose name ends with `suffix`.
 *
 * Appends each matching file's path to `paths` and sorts the whole list so builds are reproducible.
 * It follows symlinked directories, except where doing so would revisit an enclosing directory. It
 * skips such a cycle rather than treating it as an error. It likewise skips an entry that resolves
 * to nothing: a dangling symlink, one removed since it was read, or a symlink that resolves in a
 * cycle. An entry that exists but cannot be inspected fails the walk instead, so a file this
 * function could not look at is never silently missing from `paths`.
 *
 * @param paths      Initialized path list that receives the matching paths. Must not be `NULL`.
 * @param root_dir   Directory tree to walk. Must not be `NULL`.
 * @param suffix     Literal filename suffix to match, such as `.md`. Must not be `NULL`.
 * @param reason     Receives the failure reason, always naming the exact path the failure happened
 *                   on: the directory that could not be inspected, opened, read or closed, or the
 *                   entry that could not be inspected. A caller must not append the root it passed,
 *                   because the reason is already more precise than that. May be `NULL` only when
 *                   `reason_len` is 0. Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a directory, entry, or allocation failure.
 */
int fs_list_files_with_suffix(struct PathList* paths,
                              const char* root_dir,
                              const char* suffix,
                              char* reason,
                              size_t reason_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reads a regular file into a freshly allocated, `NUL`-terminated buffer.
 *
 * On success the caller owns `*data_out` and must `free` it. It writes both output parameters only
 * on success. It rejects non-regular files and files that change size during a read. It never
 * reports a partial copy as a successful read.
 *
 * It also rejects a file containing an embedded `NUL` byte. This is the boundary that establishes
 * the codebase's text invariant. Every owned string is a `NUL`-free C string, which makes
 * recovering a length with `strlen` correct downstream. TOML string values are a separate boundary,
 * because the parser can decode an escape into a `NUL` that was never in the file. See
 * `toml_datum_is_text`.
 *
 * @param file_path    Path of the file to read. Must not be `NULL`.
 * @param data_out     Receives the malloc'd buffer holding the file bytes plus a terminator. Must
 *                     not be `NULL`.
 * @param data_len_out Receives the number of bytes read, excluding the terminator. Must not be
 *                     `NULL`.
 * @param reason       Receives the failure reason, which tells the first-party rejections apart
 *                     from the system ones. May be `NULL` only when `reason_len` is 0. Untouched on
 *                     success.
 * @param reason_len   Size of `reason` in bytes.
 * @return `0` on success, or `-1` when the file is missing, not regular, too large, changed size
 *         mid-read or contains an embedded `NUL`, and on a read, allocation or close failure.
 */
int fs_read_file(const char* file_path,
                 char** data_out,
                 size_t* data_len_out,
                 char* reason,
                 size_t reason_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes `data_len` bytes to `file_path`, creating parent directories as needed.
 *
 * Overwrites any existing file.
 *
 * @param file_path  Destination path. Must not be `NULL`.
 * @param data       Source bytes. Must hold at least `data_len` bytes. Must not be `NULL`.
 * @param data_len   Number of bytes to write.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 *                   Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a directory, open, write, or close failure.
 */
int fs_write_file(const char* file_path,
                  const char* data,
                  size_t data_len,
                  char* reason,
                  size_t reason_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Creates `dir_path` and any missing parent directories.
 *
 * It accepts existing directories along the path rather than treating them as errors.
 *
 * @param dir_path   Directory path to create. An empty string is a no-op. Must not be `NULL`.
 * @param reason     Receives the failure reason, naming the component that could not be created.
 *                   May be `NULL` only when `reason_len` is 0. Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` if a component cannot be created.
 */
int fs_mkdir_p(const char* dir_path, char* reason, size_t reason_len) __attribute__((nonnull(1)));

/**
 * @brief Reports the identity of the file at `file_path`, when it exists and can be inspected.
 *
 * Unlike the other actions here this one takes no `reason`, because its caller wants a fact rather
 * than a diagnostic. "no identity" and "not the same file" lead to the same decision. A path that
 * does not exist has no identity and is not a failure worth reporting. It is the ordinary case for
 * an output path about to be created. A path that exists but cannot be inspected also reports `-1`,
 * which is safe here for the same reason. A caller that cannot `stat` a path is not going to write
 * it either. The write reports that failure with its own system message.
 *
 * @param file_path    Path to inspect. Symlinks are followed, so the identity is the target's. Must
 *                     not be `NULL`.
 * @param identity_out Receives the device and inode pair. Written only on success. Must not be
 *                     `NULL`.
 * @return `0` when `identity_out` was filled, or `-1` when `file_path` could not be inspected.
 */
int fs_identify(const char* file_path, struct FsIdentity* identity_out)
    __attribute__((nonnull(1, 2)));

#endif
