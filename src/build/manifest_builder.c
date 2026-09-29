#include "build/manifest_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build/output_path.h"
#include "core/error.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/manifest.h"
#include "domain/site_config.h"
#include "runtime/fs.h"
#include "shared/arena.h"

/**
 * Size in bytes of the diagnostic label naming one configured template list entry, including the
 * `NUL` terminator. The `<config key>[<index>]` label tells two colliding template outputs apart. A
 * bare template name cannot: the same name in `aggregate_templates` and in `feed_templates`, and
 * one name listed twice in a single list, are different mistakes with different fixes. Both would
 * otherwise report as a collision between a value and itself. The name is left out of the label
 * because it is already the tail of the output path these messages trail.
 */
enum { TEMPLATE_SOURCE_LABEL_SIZE = 64 };

// The longest key plus a decimal `size_t` at its widest, so the labels formatted in
// `manifest_builder_populate` cannot truncate and their `snprintf` results need no check.
_Static_assert(TEMPLATE_SOURCE_LABEL_SIZE >
                   sizeof("aggregate_templates") - 1 + sizeof("[18446744073709551615]") - 1,
               "template source label buffer must hold the longest key and a size_t index");

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
 * Lookup is a linear scan. At the largest tested build (3,000 sources, 3,002 outputs) that is nine
 * million integer comparisons, which measures as noise beside the reads and renders around it, so a
 * sorted index to make it logarithmic would buy nothing measurable.
 */
struct InputIdentities {
  /** Identities owned by this set, allocated once with a slot for every candidate input. */
  struct FsIdentity* items;

  /** Number of recorded identities. */
  size_t count;
};

/** Number of input directory trees no output may enter: `content_dir` and `templates_dir`. */
enum { INPUT_ROOT_COUNT = 2 };

/** One input directory tree that no output may land inside. */
struct InputRoot {
  /** Config key naming the root in diagnostics. */
  const char* config_key;

  /** Identity of the root directory. Set only when `is_present` is `true`. */
  struct FsIdentity identity;

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
 * alias spell one directory in different ways. The directories compared are `output_dir`, each of
 * its physical ancestors, and each directory between `output_dir` and the output file. A root that
 * holds `output_dir` holds every output, so that answer is computed once.
 */
struct InputRoots {
  /** The `content_dir` and `templates_dir` roots. */
  struct InputRoot items[INPUT_ROOT_COUNT];

  /** Output directory every registered output path is joined onto. */
  const char* output_dir;

  /** Config key of the root at or above `output_dir`, or `NULL` when neither root is. */
  const char* output_dir_root_key;
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
 * inside a template's bytes, so no pass that runs before the first write can enumerate the partials
 * a build will read. Claiming every file below `templates_dir` covers them without that list, so an
 * output path aimed inside `templates_dir/partials/` is rejected. It also covers every configured
 * template, the content template, aggregate and feed templates, and entry overrides alike, because
 * each is a safe relative name joined below `templates_dir`.
 *
 * This skips a path with no identity rather than refusing it. A missing configuration file or
 * source cannot be overwritten, and neither can a missing `templates_dir`, which claims nothing.
 * The render reports a missing template with a template-specific diagnostic, so failing here would
 * only report the same absence earlier and with less context.
 *
 * The template tree is listed first, so the set is allocated once with a slot for every candidate.
 *
 * @param inputs       Empty identity set that receives one entry per readable input file. Must
 *                     not be `NULL`.
 * @param site_config  Configuration supplying `templates_dir`. Must not be `NULL`.
 * @param config_path  Path the configuration was loaded from. Must not be `NULL`.
 * @param source_paths Every discovered content source path, drafts included. Must not be `NULL`.
 * @param err          Destination buffer for a failure diagnostic.
 * @param err_len      Size of `err` in bytes.
 * @return `0` when every readable input was claimed, or `-1` when the template tree cannot be
 *         walked or on allocation failure.
 */
static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct SiteConfig* site_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              char* err,
                              size_t err_len) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Lists every file below `templates_dir`, partials included.
 *
 * @param template_paths Initialized, empty path list that receives the files. Must not be `NULL`.
 * @param templates_dir  Template directory root to walk. A path with no identity lists nothing.
 *                       Must not be `NULL`.
 * @param err            Destination buffer for a failure diagnostic.
 * @param err_len        Size of `err` in bytes.
 * @return `0` when every file in the tree was listed or `templates_dir` has no identity, or `-1`
 *         when the walk fails.
 */
static int claim_build_inputs_list_templates(struct PathList* template_paths,
                                             const char* templates_dir,
                                             char* err,
                                             size_t err_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Claims one path's identity, ignoring a path that has none.
 *
 * @param inputs    Identity set to append to, with a free slot. Must not be `NULL`.
 * @param file_path Path whose identity is claimed. Must not be `NULL`.
 */
static void claim_input_identity(struct InputIdentities* inputs, const char* file_path)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Releases the identity set and leaves it initialized for reuse.
 *
 * @param inputs Identity set to release. Must not be `NULL`.
 */
static void free_input_identities(struct InputIdentities* inputs) __attribute__((nonnull(1)));

/**
 * @brief Records the identity of `content_dir` and `templates_dir`, and which of them, if either,
 *        holds `output_dir`.
 *
 * A root with no identity is recorded as absent rather than refused, for the same reason
 * `claim_input_identity` skips a missing file: it holds no input an output could land among.
 *
 * @param roots       Root set to fill. Must not be `NULL`.
 * @param site_config Configuration supplying the three directories. Must not be `NULL`.
 * @param scratch     Arena that owns the ancestor paths built while walking up from `output_dir`.
 *                    Must not be `NULL`.
 * @param err         Destination buffer for a failure diagnostic.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int claim_input_roots(struct InputRoots* roots,
                             const struct SiteConfig* site_config,
                             struct Arena* scratch,
                             char* err,
                             size_t err_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Records which input root, if either, is `output_dir` or one of its physical ancestors.
 *
 * The walk appends `..` rather than trimming the text, because `output_dir` can be relative, hold
 * `..` segments, or pass through a symlink, and the kernel resolves `..` against the directory
 * actually reached. It stops at a root match, at a path with no identity, or at the filesystem
 * root, whose `..` is itself.
 *
 * @param roots   Root set whose `output_dir_root_key` is set. Must not be `NULL`.
 * @param scratch Arena that owns the ancestor paths. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int claim_input_roots_output_dir(struct InputRoots* roots, struct Arena* scratch)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the config key of the input root whose identity is `identity`.
 *
 * @param roots    Root set to search. Must not be `NULL`.
 * @param identity Directory identity to look for. Must not be `NULL`.
 * @return The matching root's config key, or `NULL` when `identity` names neither root.
 */
static const char* find_input_root(const struct InputRoots* roots,
                                   const struct FsIdentity* identity)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Registers one configured template's output path in the build manifest.
 *
 * @param manifest      Manifest that receives the output path. Must not be `NULL`.
 * @param output_dir    Output directory the template name is rooted under. Must not be `NULL`.
 * @param template_name Safe relative template name whose output path is registered. Must not be
 *                      `NULL`.
 * @param source_label  Diagnostic label for the configured list entry naming `template_name`, as
 *                      `<config key>[<index>]`. A bare template name would report the same name in
 *                      two lists, and one name listed twice in one list, as a collision between a
 *                      value and itself. Must not be `NULL`.
 * @param inputs        Identities of this build's input files, which the output must not name. Must
 *                      not be `NULL`.
 * @param roots         Input directory trees the output must not land in. Must not be `NULL`.
 * @param scratch       Arena that owns the joined output path during registration. Must not be
 *                      `NULL`.
 * @param err           Destination buffer for a failure diagnostic.
 * @param err_len       Size of `err` in bytes.
 * @return `0` on success, or `-1` on a collision or allocation failure.
 */
static int register_template_output(struct Manifest* manifest,
                                    const char* output_dir,
                                    const char* template_name,
                                    const char* source_label,
                                    const struct InputIdentities* inputs,
                                    const struct InputRoots* roots,
                                    struct Arena* scratch,
                                    char* err,
                                    size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5, 6, 7)));

/**
 * @brief Adds one output path to the manifest, reporting a duplicate, an output inside an input
 *        root, an input overwrite, or an allocation failure.
 *
 * This is the one function every intended output path passes through, so the input checks live
 * here rather than in each producer.
 *
 * @param manifest     Manifest to append to. Must not be `NULL`.
 * @param output_path  Filesystem output path to record, joined onto `roots->output_dir`. Must not
 *                     be `NULL`.
 * @param source_label Diagnostic label for the source producing `output_path`. Must not be `NULL`.
 * @param inputs       Identities of this build's input files, which `output_path` must not name.
 *                     Must not be `NULL`.
 * @param roots        Input directory trees `output_path` must not land in. Must not be `NULL`.
 * @param scratch      Arena that owns the copy of `output_path` the root check truncates. Must not
 *                     be `NULL`.
 * @param err          Destination buffer for a failure diagnostic.
 * @param err_len      Size of `err` in bytes.
 * @return `0` when the path was recorded, or `-1` when it lands in an input root, duplicates an
 *         earlier output, names a build input, or could not be recorded.
 */
static int register_output_path(struct Manifest* manifest,
                                const char* output_path,
                                const char* source_label,
                                const struct InputIdentities* inputs,
                                const struct InputRoots* roots,
                                struct Arena* scratch,
                                char* err,
                                size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Finds the input root that an output path lands at or below.
 *
 * Checks the answer recorded for `output_dir` first. Then it checks each existing directory from
 * `output_dir` down to the output path itself, so a symlink inside `output_dir` that points into
 * an input root is caught too. The walk stops at the first missing directory, because the build
 * creates that directory and everything below it fresh.
 *
 * @param roots          Input directory trees to look for. Must not be `NULL`.
 * @param output_path    Output path joined onto `roots->output_dir`. Must not be `NULL`.
 * @param scratch        Arena that owns the truncated copy of `output_path`. Must not be `NULL`.
 * @param config_key_out Receives the config key of the root holding `output_path`, or `NULL` when
 *                       neither root does. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int find_input_root_holding(const struct InputRoots* roots,
                                   const char* output_path,
                                   struct Arena* scratch,
                                   const char** config_key_out)
    __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Reports whether `identity` names a file this build reads.
 *
 * @param inputs   Identity set to search. Must not be `NULL`.
 * @param identity Identity to look for. Must not be `NULL`.
 * @return `true` when the identity was claimed as an input, `false` otherwise.
 */
static bool has_input_identity(const struct InputIdentities* inputs,
                               const struct FsIdentity* identity) __attribute__((nonnull(1, 2)));

int manifest_builder_populate(struct Manifest* manifest,
                              const struct SiteConfig* site_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              const struct ContentEntry* const* content_entries,
                              size_t content_entry_count,
                              char* err,
                              size_t err_len) {
  struct Arena scratch;
  arena_init(&scratch);
  struct InputIdentities inputs = {0};
  struct InputRoots roots;

  int rc = claim_build_inputs(&inputs, site_config, config_path, source_paths, err, err_len);
  if (rc == 0) {
    rc = claim_input_roots(&roots, site_config, &scratch, err, err_len);
  }

  for (size_t i = 0; rc == 0 && i < content_entry_count; i++) {
    const struct ContentEntry* entry = content_entries[i];
    rc = register_output_path(manifest, entry->output_path, entry->source_path, &inputs, &roots,
                              &scratch, err, err_len);
  }
  for (size_t i = 0; rc == 0 && i < site_config->aggregate_template_count; i++) {
    char source_label[TEMPLATE_SOURCE_LABEL_SIZE];
    (void)snprintf(source_label, sizeof(source_label), "aggregate_templates[%zu]", i);
    rc = register_template_output(manifest, site_config->output_dir,
                                  site_config->aggregate_templates[i], source_label, &inputs,
                                  &roots, &scratch, err, err_len);
  }
  for (size_t i = 0; rc == 0 && i < site_config->feed_template_count; i++) {
    char source_label[TEMPLATE_SOURCE_LABEL_SIZE];
    (void)snprintf(source_label, sizeof(source_label), "feed_templates[%zu]", i);
    rc = register_template_output(manifest, site_config->output_dir, site_config->feed_templates[i],
                                  source_label, &inputs, &roots, &scratch, err, err_len);
  }

  // Every path is now recorded, so scan for the collision `manifest_add` cannot see incrementally:
  // one output path that is a directory prefix of another. The two labels lead the diagnostic as
  // the actionable part. The offending path trails because it is bounded only by the output-path
  // limit plus the output directory, so leading with it could truncate the labels.
  if (rc == 0) {
    struct ManifestPrefixCollision collision;
    if (manifest_find_prefix_collision(manifest, &collision)) {
      rc = error_report(err, err_len, "output path for '%s' nests under output path for '%s': '%s'",
                        collision.descendant_label, collision.ancestor_label,
                        collision.descendant_path);
    }
  }

  free_input_identities(&inputs);
  arena_free(&scratch);
  return rc;
}

char* manifest_builder_derive_template_output(const char* output_dir,
                                              const char* template_name,
                                              struct Arena* arena) {
  return path_join(output_dir, template_name, arena);
}

static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct SiteConfig* site_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              char* err,
                              size_t err_len) {
  struct PathList template_paths;
  path_list_init(&template_paths);
  int rc =
      claim_build_inputs_list_templates(&template_paths, site_config->templates_dir, err, err_len);
  if (rc == 0) {
    // One slot for the configuration file, then one per source and per template file. `calloc`
    // fails on product overflow rather than wrapping.
    inputs->items = calloc(1 + source_paths->count + template_paths.count, sizeof(*inputs->items));
    if (inputs->items == NULL) {
      rc = error_report(err, err_len, "out of memory recording build input files");
    } else {
      claim_input_identity(inputs, config_path);
      for (size_t i = 0; i < source_paths->count; i++) {
        claim_input_identity(inputs, source_paths->items[i]);
      }
      for (size_t i = 0; i < template_paths.count; i++) {
        claim_input_identity(inputs, template_paths.items[i]);
      }
    }
  }
  path_list_free(&template_paths);
  return rc;
}

static int claim_build_inputs_list_templates(struct PathList* template_paths,
                                             const char* templates_dir,
                                             char* err,
                                             size_t err_len) {
  // A `templates_dir` with no identity holds no file to overwrite, so there is nothing to claim.
  // This is the same skip `claim_input_identity` applies to a single missing path. The walk below
  // would otherwise fail on it, and report a missing template directory ahead of the render's
  // diagnostic naming the template it needed. An existing root that cannot be walked still fails,
  // because a partial the walk could not see would stay overwritable.
  struct FsIdentity templates_dir_identity;
  if (fs_identify(templates_dir, &templates_dir_identity) != 0) {
    return 0;
  }
  // An empty suffix matches every filename, so the walk lists the whole tree.
  char reason[FS_REASON_SIZE];
  if (fs_list_files_with_suffix(template_paths, templates_dir, "", reason, sizeof(reason)) != 0) {
    // The reason names the directory or entry that failed, which is more precise than the
    // configured root, so the root is not repeated here.
    return error_report(err, err_len, "failed to list template files: %s", reason);
  }
  return 0;
}

static void claim_input_identity(struct InputIdentities* inputs, const char* file_path) {
  struct FsIdentity identity;
  if (fs_identify(file_path, &identity) == 0) {
    inputs->items[inputs->count++] = identity;
  }
}

static void free_input_identities(struct InputIdentities* inputs) {
  free(inputs->items);
  *inputs = (struct InputIdentities){0};
}

static int claim_input_roots(struct InputRoots* roots,
                             const struct SiteConfig* site_config,
                             struct Arena* scratch,
                             char* err,
                             size_t err_len) {
  *roots = (struct InputRoots){
      .items = {{.config_key = "content_dir"}, {.config_key = "templates_dir"}},
      .output_dir = site_config->output_dir,
  };
  const char* root_dirs[INPUT_ROOT_COUNT] = {site_config->content_dir, site_config->templates_dir};
  for (size_t i = 0; i < INPUT_ROOT_COUNT; i++) {
    roots->items[i].is_present = fs_identify(root_dirs[i], &roots->items[i].identity) == 0;
  }
  if (claim_input_roots_output_dir(roots, scratch) != 0) {
    return error_report(err, err_len, "out of memory recording build input directories");
  }
  return 0;
}

static int claim_input_roots_output_dir(struct InputRoots* roots, struct Arena* scratch) {
  const char* dir_path = roots->output_dir;
  struct FsIdentity identity;
  if (fs_identify(dir_path, &identity) != 0) {
    return 0;
  }
  for (;;) {
    roots->output_dir_root_key = find_input_root(roots, &identity);
    if (roots->output_dir_root_key != NULL) {
      return 0;
    }
    dir_path = path_join(dir_path, "..", scratch);
    if (dir_path == NULL) {
      return -1;
    }
    struct FsIdentity parent_identity;
    if (fs_identify(dir_path, &parent_identity) != 0 ||
        (parent_identity.device == identity.device && parent_identity.inode == identity.inode)) {
      return 0;
    }
    identity = parent_identity;
  }
}

static const char* find_input_root(const struct InputRoots* roots,
                                   const struct FsIdentity* identity) {
  for (size_t i = 0; i < INPUT_ROOT_COUNT; i++) {
    const struct InputRoot* root = &roots->items[i];
    if (root->is_present && root->identity.device == identity->device &&
        root->identity.inode == identity->inode) {
      return root->config_key;
    }
  }
  return NULL;
}

static int register_template_output(struct Manifest* manifest,
                                    const char* output_dir,
                                    const char* template_name,
                                    const char* source_label,
                                    const struct InputIdentities* inputs,
                                    const struct InputRoots* roots,
                                    struct Arena* scratch,
                                    char* err,
                                    size_t err_len) {
  // This is checked against the same limits as a content entry's output path. Both are generated
  // output paths joined onto `output_dir`, and both land in this manifest. Applying them to one
  // producer and not the other would leave a template output path unbounded.
  if (output_path_check_limits(template_name, source_label, err, err_len) != 0) {
    return -1;
  }

  const char* output_path =
      manifest_builder_derive_template_output(output_dir, template_name, scratch);
  if (output_path == NULL) {
    return error_report(err, err_len, "out of memory building output path for '%s'", template_name);
  }
  return register_output_path(manifest, output_path, source_label, inputs, roots, scratch, err,
                              err_len);
}

static int register_output_path(struct Manifest* manifest,
                                const char* output_path,
                                const char* source_label,
                                const struct InputIdentities* inputs,
                                const struct InputRoots* roots,
                                struct Arena* scratch,
                                char* err,
                                size_t err_len) {
  // Both input checks run before the manifest, because they catch the failures that corrupt this
  // build's inputs rather than merely abandoning the build. The root check runs first because it is
  // the broader rule. It rejects every output below an input root, whether or not a file already
  // sits at that path.
  const char* root_key = NULL;
  if (find_input_root_holding(roots, output_path, scratch, &root_key) != 0) {
    return error_report(err, err_len, "out of memory checking output path for '%s'", source_label);
  }
  if (root_key != NULL) {
    // The root is a config key and bounded, so it stays in the sentence. The path trails for the
    // same reason as in the overwrite message below.
    return error_report(err, err_len, "output path would write inside '%s' for '%s': '%s'",
                        root_key, source_label, output_path);
  }

  // An output path that names one of this build's own input files would overwrite it with rendered
  // output, losing the configuration or the target of a linked source or template. Nothing later
  // in the build would notice or report it. `fs_identify` failing means the path names nothing
  // yet, which is the ordinary case for an output about to be created.
  struct FsIdentity identity;
  if (fs_identify(output_path, &identity) == 0 && has_input_identity(inputs, &identity)) {
    // The producer is the actionable part and the path trails it, matching the duplicate message
    // below. The path is bounded only by `OUTPUT_PATH_RELATIVE_LEN_MAX` plus the output directory,
    // so leading with it would truncate the label away.
    return error_report(err, err_len, "output path would overwrite build input for '%s': '%s'",
                        source_label, output_path);
  }

  const char* source_label_existing = NULL;
  switch (manifest_add(manifest, output_path, source_label, &source_label_existing)) {
    case MANIFEST_ADD_INSERTED:
      return 0;
    case MANIFEST_ADD_DUPLICATE:
      // The two colliding producers are the actionable part and the path trails them. It is bounded
      // only by `OUTPUT_PATH_RELATIVE_LEN_MAX` plus the output directory, which is twice
      // `ERROR_MESSAGE_SIZE`, and leading with it would truncate both labels away. The path is this
      // claim's spelling, which may differ from the earlier claim's in ASCII case alone, because
      // `manifest_add` folds case to catch outputs a case-insensitive filesystem would merge.
      return error_report(err, err_len, "duplicate output path for '%s' and '%s': '%s'",
                          source_label_existing, source_label, output_path);
    case MANIFEST_ADD_ERROR:
      return error_report(err, err_len, "out of memory recording output path '%s'", output_path);
  }
  // Unreachable: the switch covers every `enum ManifestAdd`. This is deliberately not a `default:`
  // case, so `-Wswitch` still fails the build when someone adds an outcome without a case here. A
  // value outside the enum means memory corruption, not a failure this function can report.
  abort();
}

static int find_input_root_holding(const struct InputRoots* roots,
                                   const char* output_path,
                                   struct Arena* scratch,
                                   const char** config_key_out) {
  *config_key_out = roots->output_dir_root_key;
  if (*config_key_out != NULL) {
    return 0;
  }
  // Each directory below `output_dir` is named by cutting a copy of the path at a `/`. Starting
  // past `output_dir` and its separator keeps the walk off `output_dir`'s own text, whose prefixes
  // need not be its ancestors once it holds `..`. The final pass checks the output path itself, so
  // an output that names a root directory outright is caught too.
  char* dir_path = arena_strdup(scratch, output_path);
  if (dir_path == NULL) {
    return -1;
  }
  for (char* cursor = dir_path + strlen(roots->output_dir) + 1;; cursor++) {
    const char c = *cursor;
    if (c != '/' && c != '\0') {
      continue;
    }
    *cursor = '\0';
    struct FsIdentity identity;
    const bool is_present = fs_identify(dir_path, &identity) == 0;
    *cursor = c;
    if (!is_present) {
      return 0;
    }
    *config_key_out = find_input_root(roots, &identity);
    if (*config_key_out != NULL || c == '\0') {
      return 0;
    }
  }
}

static bool has_input_identity(const struct InputIdentities* inputs,
                               const struct FsIdentity* identity) {
  for (size_t i = 0; i < inputs->count; i++) {
    if (inputs->items[i].device == identity->device && inputs->items[i].inode == identity->inode) {
      return true;
    }
  }
  return false;
}
