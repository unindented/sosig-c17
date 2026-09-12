#include "build/manifest_builder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/error.h"
#include "core/grow.h"
#include "core/path.h"
#include "core/path_list.h"
#include "domain/content_entry.h"
#include "domain/manifest.h"
#include "domain/site_config.h"
#include "runtime/fs.h"

/** Slots allocated when the input identity set first grows. */
enum { INPUT_IDENTITIES_CAPACITY_MIN = 16 };

// `grow_capacity` documents `capacity_min >= 1` as a precondition but cannot enforce it: passing
// `0` returns success with a capacity of `0`, and `claim_input_identity`'s append would then run
// out of bounds.
_Static_assert(INPUT_IDENTITIES_CAPACITY_MIN >= 1,
               "grow_capacity requires a minimum capacity of at least 1");

/**
 * Size in bytes of the diagnostic label naming one configured template list entry, including the
 * `NUL` terminator. The `<config key>[<index>]` label distinguishes two colliding template outputs
 * apart. A bare template name cannot: the same name in `aggregate_templates` and in
 * `feed_templates`, and one name listed twice in a single list, are different mistakes with
 * different fixes. Both would otherwise report as a collision between a value and itself. The name
 * is left out of the label because it is already the tail of the output path these messages trail.
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
 * byte-equal. Byte-equal spellings are reachable too, when `output_dir` and `content_dir` name the
 * same directory, so neither comparison includes the other, and identity is what covers both.
 * Comparing A `(device, inode)` comparison also detects cases that text cannot. These include
 * symlinks, hard links, and case-insensitive path aliases.
 *
 * Lookup is a linear scan. At the largest tested build (3,000 sources, 3,002 outputs) that is nine
 * million integer comparisons, which measures as noise beside the reads and renders around it, so a
 * sorted index to make it logarithmic would buy nothing measurable.
 */
struct InputIdentities {
  /** Identities owned by this set. */
  struct FsIdentity* items;

  /** Number of recorded identities. */
  size_t count;

  /** Allocated slots in `items`. */
  size_t capacity;
};

/**
 * @brief Records the identity of every file this build reads.
 *
 * Claims the configuration file, each discovered content source, and every directly named template.
 * This includes the content template, aggregate and feed templates, and entry overrides. It claims
 * drafts too. A draft's source file is ordinary user content and just as overwritable as a
 * published one.
 *
 * This claims the configuration file for the same reason as the rest. It is a file this build
 * reads, and `output_dir = "."` with a template named after it is enough to aim an output path at
 * it. If unclaimed, that output would overwrite the project's configuration. The build would still
 * report success.
 *
 * This skips a path with no identity rather than refusing it. A configured template that does not
 * exist cannot be overwritten. The render reports its absence with a template-specific diagnostic.
 *
 * Partials are the one input class not claimed here. `template.c` resolves them lazily during the
 * render, from names that only appear inside a template's bytes, so no pass that runs before the
 * first write can enumerate them. An output path aimed inside `templates_dir/partials/` is
 * therefore still able to overwrite a partial.
 *
 * @param inputs              Identity set that receives one entry per readable input file. Must not
 *                            be `NULL`.
 * @param site_config         Configuration supplying `templates_dir` and the template lists. Must
 *                            not be `NULL`.
 * @param config_path         Path the configuration was loaded from. Must not be `NULL`.
 * @param source_paths        Every discovered content source path, drafts included. Must not be
 *                            `NULL`.
 * @param content_entries     Non-draft entries whose `template` overrides are claimed. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param scratch             Arena that owns the joined template paths during the scan. Must not be
 *                            `NULL`.
 * @return `0` when every readable input was claimed, or `-1` on allocation failure.
 */
static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct SiteConfig* site_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              const struct ContentEntry* const* content_entries,
                              size_t content_entry_count,
                              struct Arena* scratch) __attribute__((nonnull(1, 2, 3, 4, 7)));

/**
 * @brief Claims one path's identity, ignoring a path that has none.
 *
 * @param inputs    Identity set to append to. Must not be `NULL`.
 * @param file_path Path whose identity is claimed. Must not be `NULL`.
 * @return `0` when the identity was recorded or the path had none, or `-1` on allocation failure.
 */
static int claim_input_identity(struct InputIdentities* inputs, const char* file_path)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Releases the identity set and leaves it initialized for reuse.
 *
 * @param inputs Identity set to release. Must not be `NULL`.
 */
static void free_input_identities(struct InputIdentities* inputs) __attribute__((nonnull(1)));

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
                                    struct Arena* scratch,
                                    char* err,
                                    size_t err_len) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Adds one output path to the manifest, reporting a duplicate, an input overwrite, or an
 *        allocation failure.
 *
 * This is the one function every intended output path passes through, so the input-overwrite check
 * lives here rather than in each producer.
 *
 * @param manifest     Manifest to append to. Must not be `NULL`.
 * @param output_path  Filesystem output path to record. Must not be `NULL`.
 * @param source_label Diagnostic label for the source producing `output_path`. Must not be `NULL`.
 * @param inputs       Identities of this build's input files, which `output_path` must not name.
 *                     Must not be `NULL`.
 * @param err          Destination buffer for a failure diagnostic.
 * @param err_len      Size of `err` in bytes.
 * @return `0` when the path was recorded, or `-1` when it duplicates an earlier output, names a
 *         build input, or could not be recorded.
 */
static int register_output_path(struct Manifest* manifest,
                                const char* output_path,
                                const char* source_label,
                                const struct InputIdentities* inputs,
                                char* err,
                                size_t err_len) __attribute__((nonnull(1, 2, 3, 4)));

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

  int rc = 0;
  if (claim_build_inputs(&inputs, site_config, config_path, source_paths, content_entries,
                         content_entry_count, &scratch) != 0) {
    rc = error_report(err, err_len, "out of memory recording build input files");
  }

  for (size_t i = 0; rc == 0 && i < content_entry_count; i++) {
    const struct ContentEntry* entry = content_entries[i];
    rc = register_output_path(manifest, entry->output_path, entry->source_path, &inputs, err,
                              err_len);
  }
  for (size_t i = 0; rc == 0 && i < site_config->aggregate_template_count; i++) {
    char source_label[TEMPLATE_SOURCE_LABEL_SIZE];
    (void)snprintf(source_label, sizeof(source_label), "aggregate_templates[%zu]", i);
    rc = register_template_output(manifest, site_config->output_dir,
                                  site_config->aggregate_templates[i], source_label, &inputs,
                                  &scratch, err, err_len);
  }
  for (size_t i = 0; rc == 0 && i < site_config->feed_template_count; i++) {
    char source_label[TEMPLATE_SOURCE_LABEL_SIZE];
    (void)snprintf(source_label, sizeof(source_label), "feed_templates[%zu]", i);
    rc = register_template_output(manifest, site_config->output_dir, site_config->feed_templates[i],
                                  source_label, &inputs, &scratch, err, err_len);
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

static int claim_build_inputs(struct InputIdentities* inputs,
                              const struct SiteConfig* site_config,
                              const char* config_path,
                              const struct PathList* source_paths,
                              const struct ContentEntry* const* content_entries,
                              size_t content_entry_count,
                              struct Arena* scratch) {
  if (claim_input_identity(inputs, config_path) != 0) {
    return -1;
  }

  for (size_t i = 0; i < source_paths->count; i++) {
    if (claim_input_identity(inputs, source_paths->items[i]) != 0) {
      return -1;
    }
  }

  // Every template name is relative to `templates_dir`, so the code joins each the same way
  // `write_rendered_template` will join it when it reads the file. A join that runs out of memory
  // is an allocation failure like any other. A name too long to join is not reachable here, because
  // `register_template_output` checks the configured names against the output-path limits.
  const char* const* template_lists[] = {
      &site_config->content_template,
      site_config->aggregate_templates,
      site_config->feed_templates,
  };
  const size_t template_counts[] = {
      1,
      site_config->aggregate_template_count,
      site_config->feed_template_count,
  };
  for (size_t list = 0; list < sizeof(template_lists) / sizeof(template_lists[0]); list++) {
    for (size_t i = 0; i < template_counts[list]; i++) {
      const char* template_path =
          path_join(site_config->templates_dir, template_lists[list][i], scratch);
      if (template_path == NULL || claim_input_identity(inputs, template_path) != 0) {
        return -1;
      }
    }
  }

  for (size_t i = 0; i < content_entry_count; i++) {
    if (content_entries[i]->template == NULL) {
      continue;
    }
    const char* override_path =
        path_join(site_config->templates_dir, content_entries[i]->template, scratch);
    if (override_path == NULL || claim_input_identity(inputs, override_path) != 0) {
      return -1;
    }
  }
  return 0;
}

static int claim_input_identity(struct InputIdentities* inputs, const char* file_path) {
  struct FsIdentity identity;
  if (fs_identify(file_path, &identity) != 0) {
    return 0;
  }
  if (inputs->count == inputs->capacity) {
    size_t capacity_next = 0;
    size_t capacity_bytes_next = 0;
    if (grow_capacity(inputs->capacity, INPUT_IDENTITIES_CAPACITY_MIN, sizeof(*inputs->items),
                      &capacity_next, &capacity_bytes_next) != 0) {
      return -1;
    }
    // Reallocating into a temporary keeps `inputs->items` valid when `realloc` returns `NULL`.
    struct FsIdentity* items = realloc(inputs->items, capacity_bytes_next);
    if (items == NULL) {
      return -1;
    }
    inputs->items = items;
    inputs->capacity = capacity_next;
  }
  inputs->items[inputs->count++] = identity;
  return 0;
}

static void free_input_identities(struct InputIdentities* inputs) {
  free(inputs->items);
  *inputs = (struct InputIdentities){0};
}

static int register_template_output(struct Manifest* manifest,
                                    const char* output_dir,
                                    const char* template_name,
                                    const char* source_label,
                                    const struct InputIdentities* inputs,
                                    struct Arena* scratch,
                                    char* err,
                                    size_t err_len) {
  // This is checked against the same limits as a content entry's output path. Both are generated
  // output paths joined onto `output_dir`, and both land in this manifest. Applying them to one
  // producer and not the other would leave a template output path unbounded.
  struct PathOutputMetrics metrics;
  switch (path_check_output_limits(template_name, &metrics)) {
    case PATH_OUTPUT_OK:
      break;
    case PATH_OUTPUT_TOO_LONG:
      // Both limit messages lead with the limit and the measured length and trail the offending
      // value. The trigger here is a path longer than `OUTPUT_PATH_RELATIVE_LEN_MAX`, which is
      // larger than `ERROR_MESSAGE_SIZE`, so a leading value would push the limit clause off the
      // end at every input that could reach this branch. The reason would be unreachable, not
      // merely at risk. The segment case names the segment rather than the whole template name,
      // because the segment is the substring that failed and naming both would put two unbounded
      // values in one message.
      return error_report(err, err_len,
                          "output path for a configured template exceeds max output path length "
                          "(%zu bytes) at %zu bytes: '%s'",
                          (size_t)OUTPUT_PATH_RELATIVE_LEN_MAX, metrics.len, template_name);
    case PATH_OUTPUT_SEGMENT_TOO_LONG:
      return error_report(err, err_len,
                          "output path segment for a configured template exceeds max filename "
                          "length (%zu bytes) at %zu bytes: '%.*s'",
                          (size_t)FILENAME_LEN_MAX, metrics.segment_len, (int)metrics.segment_len,
                          metrics.segment);
  }

  const char* output_path = path_join(output_dir, template_name, scratch);
  if (output_path == NULL) {
    return error_report(err, err_len, "out of memory building output path for '%s'", template_name);
  }
  return register_output_path(manifest, output_path, source_label, inputs, err, err_len);
}

static int register_output_path(struct Manifest* manifest,
                                const char* output_path,
                                const char* source_label,
                                const struct InputIdentities* inputs,
                                char* err,
                                size_t err_len) {
  // This is checked before the manifest, because it is the failure that destroys work rather than
  // merely abandoning it. An output path that names one of this build's own input files would
  // overwrite it with rendered output, losing a content source or a template. Nothing later in the
  // build would notice or report it. `fs_identify` failing means the path names nothing yet, which
  // is the ordinary case for an output about to be created.
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
      // `ERROR_MESSAGE_SIZE`, and leading with it would truncate both labels away.
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

static bool has_input_identity(const struct InputIdentities* inputs,
                               const struct FsIdentity* identity) {
  for (size_t i = 0; i < inputs->count; i++) {
    if (inputs->items[i].device == identity->device && inputs->items[i].inode == identity->inode) {
      return true;
    }
  }
  return false;
}
