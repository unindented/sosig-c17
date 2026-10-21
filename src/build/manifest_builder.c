#include "build/manifest_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "build/output_path.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/manifest.h"
#include "domain/site_config.h"
#include "runtime/os.h"
#include "runtime/walk.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * Identities of the files this build reads, gathered before any output path is registered so an
 * intended output naming one of them can be rejected before anything is written.
 *
 * Path strings cannot reliably identify the same file. An output path is relative to `output_dir`,
 * but a source path is relative to `content_dir`. For example, `output_dir = "."` can produce
 * `./content/x.md` for the source `content/x.md`. That is one file with two spellings that are not
 * byte-equal. A `(device, inode)` comparison also detects cases that text cannot. These include
 * symlinks, hard links, and case-insensitive path aliases.
 *
 * The input roots reject any output below `content_dir` or `templates_dir` first, so this set is
 * what still protects an input outside both trees: the configuration file, and the target of a
 * source or template that is a symlink or hard link to a file elsewhere.
 *
 * The identities are sorted once after they are all claimed, and each lookup is a binary search. A
 * rebuild looks up every output that already exists, which is every output of a site built before.
 * At the 10,000 sources sosig targets, a linear scan over the inputs would make that about 100
 * million comparisons. Measured in a release build, the manifest phase of a 10,000-source rebuild
 * took 95 milliseconds with a linear scan and 45 milliseconds with the binary search, out of 0.9
 * seconds for the whole rebuild.
 */
struct InputIdentities {
  /**
   * Identities in the populate call's scratch, allocated once with a slot for every candidate
   * input. In claim order until `claim_build_inputs` sorts them by device and then inode for
   * `has_input_identity`'s binary search.
   */
  struct OsIdentity* items;

  /** Number of recorded identities. */
  ptrdiff_t count;
};

/** Number of input directory trees no output may enter: `content_dir` and `templates_dir`. */
enum { INPUT_ROOT_COUNT = 2 };

/** One input directory tree that no output may land inside. */
struct InputRoot {
  /** Config key naming the root in diagnostics. */
  struct Str config_key;

  /** Identity of the root directory. Set only when `is_present` is `true`. */
  struct OsIdentity identity;

  /** Whether the root exists. A missing root holds no input to protect. */
  bool is_present;
};

/**
 * Input directory trees that no output may land at or below, gathered before any output path is
 * registered.
 *
 * Protecting the files that already exist is not enough. A new output below `content_dir` is read
 * back as a content source by the next build. A new output below `templates_dir` can be read as a
 * partial by the render of the same build, because partials resolve lazily. Neither overwrites a
 * file, so the identity set cannot see either.
 *
 * Each root is compared by identity against the directories an output passes through, for the same
 * reasons the identity set is. `./content`, `content/`, a symlinked root, and a case-insensitive
 * alias spell one directory in different ways. A root that holds `output_dir` holds every output,
 * so `manifest_builder_check_output_dir` answers that once, against `output_dir` and each of its
 * physical ancestors. The build runs it before it walks any input tree, and
 * `manifest_builder_populate` runs it again so that no caller can skip it. Populate then compares
 * each directory between `output_dir` and the output file, which is where a symlink inside the
 * output tree can still lead into a root.
 */
struct InputRoots {
  /** The `content_dir` and `templates_dir` roots. */
  struct InputRoot items[INPUT_ROOT_COUNT];

  /** Output directory every registered output path is joined onto. */
  struct Str output_dir;

  /** Host the roots and the outputs are on. */
  struct Os* os;
};

/**
 * @brief Records the identity of every file this build reads.
 *
 * Claims the configuration file, each discovered content source, and every file below
 * `templates_dir`. It claims drafts too. A draft's source file is ordinary user content and just as
 * overwritable as a published one.
 *
 * This claims the configuration file for the same reason as the rest. It is a file this build
 * reads, and `output_dir = "."` with a template named after it is enough to aim an output path at
 * it. If unclaimed, that output would overwrite the project's configuration. The build would still
 * report success.
 *
 * The whole template tree is claimed rather than the configured template names, because partials
 * are inputs too. `template.c` resolves them lazily during the render, from names that only appear
 * inside a template's bytes, so no phase that runs before the first write can enumerate the
 * partials a build will read. Claiming every file below `templates_dir` covers them without that
 * list, so an output path aimed inside `templates_dir/partials/` is rejected. It also covers every
 * configured template, the content template, aggregate and feed templates, and entry overrides
 * alike, because each is a safe relative name joined below `templates_dir`.
 *
 * This skips a path with no identity rather than refusing it. A missing configuration file or
 * source cannot be overwritten, and neither can a missing `templates_dir`, which claims nothing.
 * `cmd_build` already rejects a missing `templates_dir` while loading the build inputs, and the
 * render reports a missing template with a template-specific diagnostic, so failing here would only
 * report an absence earlier and with less context.
 *
 * The template tree is listed first, so the set is allocated once with a slot for every candidate.
 *
 * The listed template paths are needed only until their identities are claimed, so they go into
 * `scratch`. They cannot go into `perm`, because the walk gets a copy of `perm` by value for its
 * bookkeeping, and a path list growing in `perm` would overlap it. The identity set goes into
 * `perm` itself after the walk has returned, so the set outlives this call and overlaps only memory
 * the walk is done with.
 *
 * @param inputs       Empty identity set that receives one entry per readable input file. Must not
 *                     be `NULL`.
 * @param site_config  Configuration supplying `templates_dir`. Must not be `NULL`.
 * @param config_path  Path the configuration was loaded from.
 * @param source_paths Every discovered content source path, drafts included. Must not be `NULL`.
 * @param os           Host the inputs are on. Must not be `NULL`.
 * @param perm         Arena that owns the identity set. Must not be `NULL`.
 * @param scratch      Arena for the template paths, which must not overlap `perm`.
 * @param log          Log that receives every error. Must not be `NULL`.
 * @return `0` when every readable input was claimed, or `-1` when the template tree cannot be
 *         walked.
 */
static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct SiteConfig* site_config,
                              struct Str config_path,
                              const struct PathList* source_paths,
                              struct Os* os,
                              struct Arena* perm,
                              struct Arena scratch,
                              struct Log* log) __attribute__((nonnull(1, 2, 4, 5, 6, 8)));

/**
 * @brief Lists every file below `templates_dir`, partials included.
 *
 * @param template_paths Empty path list that receives the files. Must not be `NULL`.
 * @param templates_dir  Template directory root to walk. A path with no identity lists nothing.
 * @param output_dir     Output directory the walk leaves out, so a generated file reached through a
 *                       symlink is not claimed as a template.
 * @param os             Host the tree is on. Must not be `NULL`.
 * @param perm           Arena that owns the listed paths. Must not be `NULL`.
 * @param scratch        Arena for the walk's bookkeeping. Must not overlap `perm`.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` when every file in the tree was listed or `templates_dir` has no identity, or `-1`
 *         when the walk fails.
 */
static int claim_build_inputs_list_templates(struct PathList* template_paths,
                                             struct Str templates_dir,
                                             struct Str output_dir,
                                             struct Os* os,
                                             struct Arena* perm,
                                             struct Arena scratch,
                                             struct Log* log) __attribute__((nonnull(1, 4, 5, 7)));

/**
 * @brief Claims one path's identity, ignoring a path that has none.
 *
 * @param inputs    Identity set to append to, with a free slot. Must not be `NULL`.
 * @param file_path Path whose identity is claimed.
 * @param os        Host the path is on. Must not be `NULL`.
 */
static void claim_build_inputs_identity(struct InputIdentities* inputs,
                                        struct Str file_path,
                                        struct Os* os) __attribute__((nonnull(1, 3)));

/**
 * @brief Records the identity of `content_dir` and `templates_dir`.
 *
 * A root with no identity is recorded as absent rather than refused, for the same reason
 * `claim_build_inputs_identity` skips a missing file: it holds no input an output could land among.
 *
 * @param roots       Root set to fill. Must not be `NULL`.
 * @param site_config Configuration supplying the roots and `output_dir`. Must not be `NULL`.
 * @param os          Host the roots are on. Must not be `NULL`.
 */
static void claim_input_roots(struct InputRoots* roots,
                              const struct SiteConfig* site_config,
                              struct Os* os) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Finds the input root that `output_dir` is at or below, whether or not it exists yet.
 *
 * Starts from the nearest existing ancestor of `output_dir`, which is `output_dir` itself once it
 * exists, so the answer is the same before and after the build creates it. From there the walk
 * appends `..` rather than trimming the text, because the path can be relative, hold `..` segments,
 * or pass through a symlink, and the kernel resolves `..` against the directory actually reached.
 * It stops at a root match, at a path with no identity, or at the filesystem root, whose `..` is
 * itself.
 *
 * @param roots   Input directory trees to look for. Must not be `NULL`.
 * @param scratch Arena for the ancestor paths.
 * @return The config key of the root holding `output_dir`, or an empty string when no root does.
 */
static struct Str find_input_root_holding_output_dir(const struct InputRoots* roots,
                                                     struct Arena scratch)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the nearest existing ancestor of `dir_path`, which is `dir_path` itself when it
 *        exists.
 *
 * Each component is joined onto the nearest path known to exist, so an existing component, `..`
 * included, resolves the way the kernel resolves it. Past the first missing component the
 * components are only counted, because `os_mkdirs` creates each of them as a real directory: a `..`
 * there cancels the missing component before it, and probing resumes once none is left. That keeps
 * `missing/../content/new` from hiding that it lands in `content`.
 *
 * @param dir_path Directory path, absolute or relative to the working directory.
 * @param os       Host the path is on. Must not be `NULL`.
 * @param arena    Arena that owns the returned path and the copies built on the way. Must not be
 *                 `NULL`.
 * @return The nearest existing ancestor, owned by `arena` or static.
 */
static struct Str find_nearest_existing_ancestor(struct Str dir_path,
                                                 struct Os* os,
                                                 struct Arena* arena)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Returns the config key of the input root whose identity is `identity`.
 *
 * @param roots    Root set to search. Must not be `NULL`.
 * @param identity Directory identity to look for. Must not be `NULL`.
 * @return The matching root's config key, or an empty string when `identity` names no root.
 */
static struct Str find_input_root(const struct InputRoots* roots, const struct OsIdentity* identity)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Registers the output path of every content entry, labeled by the entry's source path.
 *
 * @param manifest            Manifest that receives the output paths. Must not be `NULL`.
 * @param content_entries     Rendered content entries whose output paths, each joined onto
 *                            `roots->output_dir`, are recorded. May be `NULL` only when
 *                            `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param inputs              Identities of this build's input files, which no output may name. Must
 *                            not be `NULL`.
 * @param roots               Input directory trees no output may land in. Must not be `NULL`.
 * @param perm                Arena that owns the manifest's entries. Must not be `NULL`.
 * @param log                 Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when any path collides or names an input.
 */
static int register_content_entry_list(struct Manifest* manifest,
                                       const struct ContentEntry* const* content_entries,
                                       ptrdiff_t content_entry_count,
                                       const struct InputIdentities* inputs,
                                       const struct InputRoots* roots,
                                       struct Arena* perm,
                                       struct Log* log) __attribute__((nonnull(1, 4, 5, 6, 7)));

/**
 * @brief Registers the output path of every template in one configured template list.
 *
 * @param manifest       Manifest that receives the output paths. Must not be `NULL`.
 * @param list_key       Config key of the list, which leads each entry's `<config key>[<index>]`
 *                       diagnostic label.
 * @param template_names Array of `template_count` safe relative template names. May be `NULL` only
 *                       when `template_count` is 0.
 * @param template_count Number of entries in `template_names`.
 * @param inputs         Identities of this build's input files, which no output may name. Must not
 *                       be `NULL`.
 * @param roots          Input directory trees no output may land in. Must not be `NULL`.
 * @param perm           Arena that owns the manifest's entries. Must not be `NULL`.
 * @param scratch        Arena for each entry's label and joined output path during registration.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when any path is oversize or collides.
 */
static int register_template_list(struct Manifest* manifest,
                                  struct Str list_key,
                                  const struct Str* template_names,
                                  ptrdiff_t template_count,
                                  const struct InputIdentities* inputs,
                                  const struct InputRoots* roots,
                                  struct Arena* perm,
                                  struct Arena scratch,
                                  struct Log* log) __attribute__((nonnull(1, 5, 6, 7, 9)));

/**
 * @brief Registers one configured template's output path in the build manifest.
 *
 * @param manifest      Manifest that receives the output path. Must not be `NULL`.
 * @param template_name Safe relative template name whose output path is registered, rooted under
 *                      `roots->output_dir`.
 * @param source_label  Diagnostic label for the configured list entry naming `template_name`, as
 *                      `<config key>[<index>]`. A bare template name would report the same name in
 *                      two lists, and one name listed twice in one list, as a collision between a
 *                      value and itself. The name is left out of the label because it is already
 *                      the tail of the output path these messages trail.
 * @param inputs        Identities of this build's input files, which the output must not name. Must
 *                      not be `NULL`.
 * @param roots         Input directory trees the output must not land in. Must not be `NULL`.
 * @param perm          Arena that owns the manifest's entries. Must not be `NULL`.
 * @param scratch       Arena for the joined output path during registration.
 * @param log           Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on an oversize path or a collision.
 */
static int register_template_output(struct Manifest* manifest,
                                    struct Str template_name,
                                    struct Str source_label,
                                    const struct InputIdentities* inputs,
                                    const struct InputRoots* roots,
                                    struct Arena* perm,
                                    struct Arena scratch,
                                    struct Log* log) __attribute__((nonnull(1, 4, 5, 6, 8)));

/**
 * @brief Adds one output path to the manifest, reporting a duplicate, an output inside an input
 *        root, or an input overwrite.
 *
 * This is the one function every intended output path passes through, so the input checks live here
 * rather than in each producer.
 *
 * @param manifest     Manifest to append to. Must not be `NULL`.
 * @param output_path  Filesystem output path to record, joined onto `roots->output_dir`.
 * @param source_label Diagnostic label for the source producing `output_path`: a content entry's
 *                     source path or a configured template list entry.
 * @param inputs       Identities of this build's input files, which `output_path` must not name.
 *                     Must not be `NULL`.
 * @param roots        Input directory trees `output_path` must not land in. Must not be `NULL`.
 * @param perm         Arena that owns the manifest's entries. Must not be `NULL`.
 * @param log          Log that receives every error. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` when it lands in an input root, duplicates an
 *         earlier output, or names a build input.
 */
static int register_output_path(struct Manifest* manifest,
                                struct Str output_path,
                                struct Str source_label,
                                const struct InputIdentities* inputs,
                                const struct InputRoots* roots,
                                struct Arena* perm,
                                struct Log* log) __attribute__((nonnull(1, 4, 5, 6, 7)));

/**
 * @brief Finds the input root that an output path lands at or below.
 *
 * Checks each existing directory below `output_dir` down to the output path itself, so a symlink
 * inside `output_dir` that points into an input root is caught. `output_dir` and its ancestors are
 * not checked here, because the build has already cleared them through
 * `manifest_builder_check_output_dir`. The walk stops at the first missing directory, because the
 * build creates that directory and everything below it fresh.
 *
 * @param roots       Input directory trees to look for. Must not be `NULL`.
 * @param output_path Output path joined onto `roots->output_dir`.
 * @return The config key of the root holding `output_path`, or an empty string when no root does.
 */
static struct Str find_input_root_holding_output_path(const struct InputRoots* roots,
                                                      struct Str output_path)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether `identity` names a file this build reads.
 *
 * @param inputs   Identity set to search, sorted by `claim_build_inputs`. Must not be `NULL`.
 * @param identity Identity to look for. Must not be `NULL`.
 * @return `true` when the identity was claimed as an input, `false` otherwise.
 */
static bool has_input_identity(const struct InputIdentities* inputs,
                               const struct OsIdentity* identity) __attribute__((nonnull(1, 2)));

/**
 * @brief Reports every recorded output path that nests under another, the collision `manifest_add`
 *        cannot see while the paths are still being recorded.
 *
 * @param manifest Manifest holding every output path of the build. Must not be `NULL`.
 * @param log      Log that receives one error per nested path. Must not be `NULL`.
 * @return `0` when no output path nests under another, or `-1` when one does.
 */
static int report_nested_output_paths(const struct Manifest* manifest, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports the identity of the file at `file_path`, when it exists and can be inspected.
 *
 * Each caller here uses only whether the path has an identity, so a path that cannot be inspected
 * is treated like one that does not exist: neither is a file an output could overwrite or land in.
 * A caller that cannot `stat` a path is not going to write it either, and the write reports that
 * failure with its own system message.
 *
 * @param file_path    Path to inspect, followed through symlinks.
 * @param os           Host the path is on. Must not be `NULL`.
 * @param identity_out Receives the identity. Written only on success. Must not be `NULL`.
 * @return `0` when `identity_out` was filled, or `-1` when `file_path` has no identity.
 */
static int identify(struct Str file_path, struct Os* os, struct OsIdentity* identity_out)
    __attribute__((nonnull(2, 3)));

int manifest_builder_check_output_dir(const struct SiteConfig* site_config,
                                      struct Os* os,
                                      struct Arena scratch,
                                      struct Log* log) {
  struct InputRoots roots;
  claim_input_roots(&roots, site_config, os);
  const struct Str root_key = find_input_root_holding_output_dir(&roots, scratch);
  if (root_key.len > 0) {
    // The root is a config key, so it stays in the sentence. The configured path trails as the
    // value that broke the rule.
    return log_error(log, "output directory would write inside '%.*s': '%.*s'", STR_FMT(root_key),
                     STR_FMT(site_config->output_dir));
  }
  return 0;
}

int manifest_builder_populate(struct Manifest* manifest,
                              const struct SiteConfig* site_config,
                              struct Str config_path,
                              const struct PathList* source_paths,
                              const struct ContentEntry* const* content_entries,
                              ptrdiff_t content_entry_count,
                              struct Os* os,
                              struct Arena* perm,
                              struct Arena scratch,
                              struct Log* log) {
  struct InputIdentities inputs = {0};
  struct InputRoots roots;

  // An input failure makes every output path suspect, so it stops the check rather than reporting
  // the same cause once per output.
  claim_input_roots(&roots, site_config, os);
  if (claim_build_inputs(&inputs, site_config, config_path, source_paths, os, &scratch, *perm,
                         log) != 0) {
    return -1;
  }

  // Every output is checked even after one fails, so each collision in the site is reported in one
  // run. A rejected path is not recorded, so it cannot cause a second report against a later one.
  int rc = 0;
  if (register_content_entry_list(manifest, content_entries, content_entry_count, &inputs, &roots,
                                  perm, log) != 0) {
    rc = -1;
  }
  if (register_template_list(manifest, S("aggregate_templates"), site_config->aggregate_templates,
                             site_config->aggregate_template_count, &inputs, &roots, perm, scratch,
                             log) != 0) {
    rc = -1;
  }
  if (register_template_list(manifest, S("feed_templates"), site_config->feed_templates,
                             site_config->feed_template_count, &inputs, &roots, perm, scratch,
                             log) != 0) {
    rc = -1;
  }
  // Every path is now recorded, so the prefix scan sees all of them.
  if (report_nested_output_paths(manifest, log) != 0) {
    rc = -1;
  }
  return rc;
}

static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct SiteConfig* site_config,
                              struct Str config_path,
                              const struct PathList* source_paths,
                              struct Os* os,
                              struct Arena* perm,
                              struct Arena scratch,
                              struct Log* log) {
  struct PathList template_paths = {0};
  if (claim_build_inputs_list_templates(&template_paths, site_config->templates_dir,
                                        site_config->output_dir, os, &scratch, *perm, log) != 0) {
    return -1;
  }
  // One slot for the configuration file, then one per source and per template file. The counts
  // index arrays already in memory, so their sum cannot overflow.
  inputs->items =
      arena_new(perm, 1 + source_paths->count + template_paths.count, struct OsIdentity);
  claim_build_inputs_identity(inputs, config_path, os);
  for (ptrdiff_t i = 0; i < source_paths->count; i++) {
    claim_build_inputs_identity(inputs, source_paths->items[i], os);
  }
  for (ptrdiff_t i = 0; i < template_paths.count; i++) {
    claim_build_inputs_identity(inputs, template_paths.items[i], os);
  }
  qsort(inputs->items, (size_t)inputs->count, sizeof(*inputs->items), os_identity_compare);
  return 0;
}

static int claim_build_inputs_list_templates(struct PathList* template_paths,
                                             struct Str templates_dir,
                                             struct Str output_dir,
                                             struct Os* os,
                                             struct Arena* perm,
                                             struct Arena scratch,
                                             struct Log* log) {
  // A `templates_dir` with no identity holds no file to overwrite, so there is nothing to claim.
  // This is the same skip `claim_build_inputs_identity` applies to a single missing path. A full
  // build never gets here with one, because `cmd_build` rejects a missing `templates_dir` while
  // loading the build inputs, so reporting it is left to that earlier check. An existing root that
  // cannot be walked still fails, because a partial the walk could not see would stay overwritable.
  struct OsIdentity templates_dir_identity;
  if (identify(templates_dir, os, &templates_dir_identity) != 0) {
    return 0;
  }
  // An empty suffix matches every filename, so the walk lists the whole tree.
  static const struct Str all_suffixes[] = {STR_INIT("")};
  if (walk_files_with_suffixes(template_paths, templates_dir, output_dir, all_suffixes, 1, false,
                               os, perm, scratch, S("failed to list template files"), log) != 0) {
    // The failure names the directory or entry that failed, which is more precise than the
    // configured root, so the root is not repeated here.
    return -1;
  }
  return 0;
}

static void claim_build_inputs_identity(struct InputIdentities* inputs,
                                        struct Str file_path,
                                        struct Os* os) {
  struct OsIdentity identity;
  if (identify(file_path, os, &identity) == 0) {
    inputs->items[inputs->count++] = identity;
  }
}

static void claim_input_roots(struct InputRoots* roots,
                              const struct SiteConfig* site_config,
                              struct Os* os) {
  *roots = (struct InputRoots){
      .items = {{.config_key = S("content_dir")}, {.config_key = S("templates_dir")}},
      .output_dir = site_config->output_dir,
      .os = os,
  };
  const struct Str root_dirs[INPUT_ROOT_COUNT] = {site_config->content_dir,
                                                  site_config->templates_dir};
  for (ptrdiff_t i = 0; i < INPUT_ROOT_COUNT; i++) {
    roots->items[i].is_present = identify(root_dirs[i], os, &roots->items[i].identity) == 0;
  }
}

static struct Str find_input_root_holding_output_dir(const struct InputRoots* roots,
                                                     struct Arena scratch) {
  struct Str dir_path = find_nearest_existing_ancestor(roots->output_dir, roots->os, &scratch);
  struct OsIdentity identity;
  if (identify(dir_path, roots->os, &identity) != 0) {
    return (struct Str){0};
  }
  for (;;) {
    const struct Str config_key = find_input_root(roots, &identity);
    if (config_key.len > 0) {
      return config_key;
    }
    dir_path = path_join(dir_path, S(".."), &scratch);
    struct OsIdentity parent_identity;
    if (identify(dir_path, roots->os, &parent_identity) != 0 ||
        os_identity_compare(&parent_identity, &identity) == 0) {
      return (struct Str){0};
    }
    identity = parent_identity;
  }
}

static struct Str find_nearest_existing_ancestor(struct Str dir_path,
                                                 struct Os* os,
                                                 struct Arena* arena) {
  struct Str ancestor_path = str_startswith(dir_path, S("/")) ? S("/") : S(".");
  ptrdiff_t missing_count = 0;
  struct StrCut cut = {.tail = dir_path, .is_found = true};
  while (cut.is_found) {
    cut = str_cut(cut.tail, '/');
    const struct Str component = cut.head;
    if (component.len == 0 || str_eq(component, S("."))) {
      continue;
    }
    if (missing_count > 0) {
      missing_count = str_eq(component, S("..")) ? missing_count - 1 : missing_count + 1;
      continue;
    }
    const struct Str candidate_path = path_join(ancestor_path, component, arena);
    struct OsIdentity identity;
    if (identify(candidate_path, os, &identity) == 0) {
      ancestor_path = candidate_path;
    } else {
      missing_count = 1;
    }
  }
  return ancestor_path;
}

static struct Str find_input_root(const struct InputRoots* roots,
                                  const struct OsIdentity* identity) {
  for (ptrdiff_t i = 0; i < INPUT_ROOT_COUNT; i++) {
    const struct InputRoot* root = &roots->items[i];
    if (root->is_present && os_identity_compare(&root->identity, identity) == 0) {
      return root->config_key;
    }
  }
  return (struct Str){0};
}

static int register_content_entry_list(struct Manifest* manifest,
                                       const struct ContentEntry* const* content_entries,
                                       ptrdiff_t content_entry_count,
                                       const struct InputIdentities* inputs,
                                       const struct InputRoots* roots,
                                       struct Arena* perm,
                                       struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < content_entry_count; i++) {
    const struct ContentEntry* entry = content_entries[i];
    if (register_output_path(manifest, entry->output_path, entry->source_path, inputs, roots, perm,
                             log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static int register_template_list(struct Manifest* manifest,
                                  struct Str list_key,
                                  const struct Str* template_names,
                                  ptrdiff_t template_count,
                                  const struct InputIdentities* inputs,
                                  const struct InputRoots* roots,
                                  struct Arena* perm,
                                  struct Arena scratch,
                                  struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < template_count; i++) {
    struct Arena entry_scratch = scratch;
    struct Buf source_label = {.arena = &entry_scratch};
    buf_printf(&source_label, "%.*s[%td]", STR_FMT(list_key), i);
    if (register_template_output(manifest, template_names[i], buf_str(&source_label), inputs, roots,
                                 perm, entry_scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static int register_template_output(struct Manifest* manifest,
                                    struct Str template_name,
                                    struct Str source_label,
                                    const struct InputIdentities* inputs,
                                    const struct InputRoots* roots,
                                    struct Arena* perm,
                                    struct Arena scratch,
                                    struct Log* log) {
  // This is checked against the same limits as a content entry's output path. Both are generated
  // output paths joined onto `output_dir`, and both land in this manifest. `entry_renderer` checks
  // the paths it generates, so no producer reaches the manifest with an unbounded output path.
  if (output_path_check_limits(template_name, source_label, log) != 0) {
    return -1;
  }

  // `site_writer` writes to this same join, so the path checked here is the path written.
  const struct Str output_path = path_join(roots->output_dir, template_name, &scratch);
  return register_output_path(manifest, output_path, source_label, inputs, roots, perm, log);
}

static int register_output_path(struct Manifest* manifest,
                                struct Str output_path,
                                struct Str source_label,
                                const struct InputIdentities* inputs,
                                const struct InputRoots* roots,
                                struct Arena* perm,
                                struct Log* log) {
  // Both input checks run before the manifest, because they catch the failures that corrupt this
  // build's inputs rather than merely abandoning the build. The root check runs first because it is
  // the broader rule. It rejects every output below an input root, whether or not a file already
  // sits at that path.
  const struct Str root_key = find_input_root_holding_output_path(roots, output_path);
  if (root_key.len > 0) {
    // The root is a config key, so it stays in the sentence. The path trails, as in the overwrite
    // message below.
    return log_error(log, "output path would write inside '%.*s' for '%.*s': '%.*s'",
                     STR_FMT(root_key), STR_FMT(source_label), STR_FMT(output_path));
  }

  // An output path naming one of this build's own input files would replace that file with
  // generated output, and nothing later in the build would notice. `identify` failing means the
  // path names nothing yet, which is the ordinary case for an output about to be created.
  struct OsIdentity identity;
  if (identify(output_path, roots->os, &identity) == 0 && has_input_identity(inputs, &identity)) {
    // The producer is the actionable part and the path trails it, matching the duplicate message
    // below.
    return log_error(log, "output path would overwrite build input for '%.*s': '%.*s'",
                     STR_FMT(source_label), STR_FMT(output_path));
  }

  struct Str source_label_existing = {0};
  if (manifest_add(manifest, output_path, source_label, perm, &source_label_existing) != 0) {
    // The two colliding producers are the actionable part and the path trails them. The path is
    // this claim's spelling, which may differ from the earlier claim's in ASCII case alone, because
    // `manifest_add` folds case to catch outputs a case-insensitive filesystem would merge.
    return log_error(log, "duplicate output path for '%.*s' and '%.*s': '%.*s'",
                     STR_FMT(source_label_existing), STR_FMT(source_label), STR_FMT(output_path));
  }
  return 0;
}

static struct Str find_input_root_holding_output_path(const struct InputRoots* roots,
                                                      struct Str output_path) {
  // Each directory below `output_dir` is a prefix of the path ending before a `/`. Starting past
  // `output_dir` and its separator keeps the walk off `output_dir`'s own text, whose prefixes need
  // not be its ancestors once it holds `..`. The final pass checks the output path itself, so an
  // output that names a root directory outright is caught too.
  for (ptrdiff_t i = roots->output_dir.len + 1;; i++) {
    const bool is_end = i >= output_path.len;
    if (!is_end && output_path.data[i] != '/') {
      continue;
    }
    struct OsIdentity identity;
    if (identify(str_head(output_path, is_end ? output_path.len : i), roots->os, &identity) != 0) {
      return (struct Str){0};
    }
    const struct Str config_key = find_input_root(roots, &identity);
    if (config_key.len > 0 || is_end) {
      return config_key;
    }
  }
}

static bool has_input_identity(const struct InputIdentities* inputs,
                               const struct OsIdentity* identity) {
  return inputs->count > 0 && bsearch(identity, inputs->items, (size_t)inputs->count,
                                      sizeof(*inputs->items), os_identity_compare) != NULL;
}

static int report_nested_output_paths(const struct Manifest* manifest, struct Log* log) {
  // One output path that is a directory prefix of another cannot be both a file and a directory.
  // Every path that nests is reported, the two labels leading as the actionable part.
  int rc = 0;
  struct ManifestPrefixCollision collision = {0};
  while (manifest_find_prefix_collision(manifest, &collision)) {
    rc = log_error(log, "output path for '%.*s' nests under output path for '%.*s': '%.*s'",
                   STR_FMT(collision.descendant_label), STR_FMT(collision.ancestor_label),
                   STR_FMT(collision.descendant_path));
  }
  return rc;
}

static int identify(struct Str file_path, struct Os* os, struct OsIdentity* identity_out) {
  struct OsStat st;
  if (os_stat(os, file_path, &st) != 0) {
    return -1;
  }
  *identity_out = st.identity;
  return 0;
}
