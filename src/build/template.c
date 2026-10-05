#include "build/template.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "core/path.h"
#include "core/sosig_version.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "formats/html.h"
#include "mustache.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

// Bounds on one template render. There are three independent failure modes, so three limits, each
// enforced at the one place that can observe it. Each limit names what it bounds: the expansion
// count terminates partial recursion, the output length bounds the output buffer, and the distinct
// partial count bounds the compiled-partial cache. All three bound a single render, and renders run
// `worker_count`-wide, so several per-render costs can be live at once. That scaling is close to
// linear. On an 8-entry runaway build at three names per expansion, peak resident memory rose from
// 63 MB with one worker to 397 MB with eight. Read the per-render figures below as costs that
// `worker_count` concurrent runaway renders multiply.
//
// None of the three bounds the render's total memory. Templates are trusted input, like the config.
// `node_alloc` puts a node in the per-render arena for every name resolution, and a section nested
// inside another resolves its names once per iteration of every enclosing section without touching
// the expansion counter. A template's own nested iteration therefore uses memory proportional to
// the iterations it asks for. Over 20 entries, `{{#tags}}{{/tags}}` inside `{{#content_entries}}`
// nested five deep renders a one-byte page at 322 MB peak resident memory, and six deep at 6.4 GB.
//
// Termination: mustache4c expands partials iteratively through its own stack and exposes no nesting
// depth, but it calls the partial resolver once per expansion, cache hits included. Expansions are
// what can be counted, and counting them stops a partial that includes itself. A legitimate render
// costs one expansion per partial reference per entry, so an aggregate over 10,000 entries
// referencing ten partials each costs 100,000. The bound sits three times above that.
//
// The margin is three rather than ten because of what an expansion costs. mustache4c's own stack is
// 24 bytes per expansion: three `uintptr_t` pushes, at `mustache.c:1119`, heap-backed rather than
// thread stack. That is the smaller term. The larger one is this file's nodes, which the arena
// keeps until the render ends, and nothing bounds names-per-expansion. A template referencing nine
// names inside a self-including partial resolves nine nodes per expansion, so a runaway render's
// memory is `expansions × names`, not `expansions`. The bound has to be set against the product.
//
// These figures are peak resident memory of a whole `sosig build` measured at this bound, one
// entry, one worker, release build: 10 MB for a bare `{{>loop}}`, 53 MB at three names, 111 MB at
// nine, scaling linearly in names from there. The nodes cannot be shared or interned to avoid this.
// See `node_alloc`, which states the vendored contract requiring pointer-unique nodes. Lowering the
// count further would start to reject the legitimate case above. A section iterating inside the
// recursive partial multiplies each expansion's cost by its iteration count, which no limit here
// bounds.
//
// Output: escaping expands one byte into as much as a six-byte entity, so the check bounds
// accumulated output after each append rather than from the incoming chunk length. This bounds the
// output buffer, not the render's other allocations. It is the only one of the three a legitimate
// site can reach, so the bound targets that case rather than a runaway one. An aggregate embedding
// every entry's full body costs `entry_count * body_size`, which for 10,000 entries at ~6.7 KB is
// 64 MB, so the bound sits four times above the largest site this tool is meant for. The growable
// buffer doubles, so a render that reaches the bound allocates near three times it before failing.
// Resident memory stays lower, because the doubled capacity is never written. One that tripped the
// bound peaked at 261 MB. Reaching it costs that many bytes, so `template_render_file_limited`
// takes the bound as a parameter, and the test that asserts its diagnostic passes a smaller one.
//
// Capacity: the compiled-partial cache is a fixed array, so its length bounds the distinct partial
// count and with it the cache. This one limits real templates, not runaway ones. A cycle cannot
// inflate the distinct-name count, because the cache returns hits.
enum { RENDER_EXPANSION_COUNT_MAX = 300000 };
enum { RENDER_OUTPUT_LEN_MAX = 256 * 1024 * 1024 };
enum { RENDER_PARTIAL_COUNT_MAX = 64 };

_Static_assert((size_t)RENDER_PARTIAL_COUNT_MAX <= (size_t)RENDER_EXPANSION_COUNT_MAX,
               "a distinct partial costs at least one expansion, so its limit must be reachable");

/**
 * Largest template or partial file read, in bytes.
 *
 * A render reads its template and compiles up to `RENDER_PARTIAL_COUNT_MAX` distinct partials, and
 * the compiled form keeps a copy of every literal run, so 4 MiB each bounds one render's compiled
 * sources at 260 MiB, the same order as `RENDER_OUTPUT_LEN_MAX`. A hand-written template with
 * inline styles, scripts and SVG is tens of kilobytes, so this sits two orders of magnitude above
 * one. The read checks it before loading the file, so an oversize template is never resident.
 */
enum { TEMPLATE_FILE_LEN_MAX = 4 * 1024 * 1024 };

/**
 * Size of the stack buffer `node_get_partial_path` formats `partials/<name>.html` into. This is a
 * buffer bound on one path rather than a limit on the render's behavior.
 *
 * 256 leaves 241 bytes for the name, after `partials/` (9), `.html` (5) and the terminator. That is
 * far above any real partial name and deliberately below the 250 that `FILENAME_LEN_MAX` would
 * allow for `<name>.html`. This buffer, not the filesystem layer, is the first limit an absurd name
 * meets. The diagnostic the user sees names the partial instead of reporting an opaque read
 * failure.
 */
enum { PARTIAL_PATH_SIZE = 256 };

_Static_assert(PARTIAL_PATH_SIZE > sizeof("partials/") - 1 + sizeof(".html") - 1,
               "partial path buffer must hold the 'partials/' prefix and '.html' suffix");

/** Kind of a node in the immutable data tree walked by `mustache_process`. */
enum NodeKind {
  /** Top-level lookup context: `site.*`, `content_entries`, and current entry fields. */
  NODE_ROOT,

  /** Site metadata subtree resolving `site.*` names. */
  NODE_SITE,

  /** One content entry, resolving bare field names. */
  NODE_ENTRY,

  /** The `content_entries` list, iterated by index. */
  NODE_ENTRY_LIST,

  /** A content entry's tag list, iterated by index. */
  NODE_TAG_LIST,

  /** A terminal string value. */
  NODE_SCALAR,
};

/** A node in the data tree. Nodes are arena-owned and live for one render. */
struct Node {
  /** Selects which of the fields below carry meaning for this node. */
  enum NodeKind kind;

  /** Backing content entry for `NODE_ENTRY` and `NODE_TAG_LIST`. */
  const struct ContentEntry* entry;

  /**
   * Borrowed text for `NODE_SCALAR`. Never `NULL`. Never empty either when it came from
   * `node_scalar`, which rejects both so an unset optional field reads as absent. A tag may still
   * be empty, because `node_get_child_by_index` builds tag nodes directly to keep an empty tag from
   * ending the list. Either way `node_dump` may call `strlen` on it unguarded.
   */
  const char* scalar;
};

/** State threaded through the Mustache parser and data-provider callbacks. */
struct ProviderData {
  /** Template directory root used to locate partials. */
  const char* templates_dir;

  /** Name of the template being rendered, used in diagnostics. */
  const char* template_name;

  /**
   * Relative path of the template or partial currently being compiled, which `record_parse_error`
   * names as the file a syntax error is inside. Distinct from `template_name`, because a partial
   * compiles while mustache4c renders the outer template. This is a path rather than a bare name,
   * so the `(in '<file>')` clause denotes a file in both cases.
   */
  const char* template_name_compiling;

  /** Borrowed values visible to template variables. */
  const struct TemplateContext* context;

  /** Arena owning the nodes and partial bookkeeping of this render. */
  struct Arena* arena;

  /** Root node returned to `mustache_process`. */
  struct Node* node_root;

  /** Compiled partials, released after processing. */
  MUSTACHE_TEMPLATE* partials[RENDER_PARTIAL_COUNT_MAX];

  /** Partial names parallel to `partials`, used as a compile cache. */
  const char* partial_names[RENDER_PARTIAL_COUNT_MAX];

  /** Number of populated entries in `partials`/`partial_names`. */
  size_t partial_count;

  /** Partial expansions resolved so far, bounded by `RENDER_EXPANSION_COUNT_MAX`. */
  size_t expansion_count;

  /** Destination for the render's first failure diagnostic. May be `NULL` when `err_len` is 0. */
  char* err;

  /** Size of `err` in bytes. */
  size_t err_len;

  /**
   * `render_fail` sets this on the first failure in any callback, and
   * `template_render_file_limited` checks it once processing returns. That failure is an exceeded
   * limit, an unsafe partial name, an unreadable or uncompilable partial, or an allocation failure.
   */
  bool has_failed;
};

/**
 * Destination for one render's output, passed to mustache4c as its renderer data. Pairs the buffer
 * and its bound with the provider state so an output callback that passes the bound can record the
 * diagnostic itself.
 */
struct RenderOutput {
  /** Buffer accumulating the rendered bytes. */
  struct StringBuffer* buffer;

  /** Largest accumulated length `buffer` may reach, in bytes. */
  size_t output_len_max;

  /** Provider state for this render, used to report a failed or oversize append. */
  struct ProviderData* provider_data;
};

/**
 * @brief Appends output text verbatim to the render buffer.
 *
 * This is installed in `renderer` as mustache4c's unescaped-output callback.
 *
 * @param output        Rendered bytes to append.
 * @param output_len    Number of bytes in `output`.
 * @param renderer_data Pointer to the destination `struct RenderOutput`.
 * @return `0` on success, or `-1` on allocation failure or an exceeded output limit.
 */
static int out_verbatim(const char* output, size_t output_len, void* renderer_data);

/**
 * @brief Appends output text HTML-escaped to the render buffer.
 *
 * This is installed in `renderer` as mustache4c's escaped-output callback.
 *
 * @param output        Rendered bytes to append.
 * @param output_len    Number of bytes in `output`.
 * @param renderer_data Pointer to the destination `struct RenderOutput`.
 * @return `0` on success, or `-1` on allocation failure or an exceeded output limit.
 */
static int out_escaped(const char* output, size_t output_len, void* renderer_data);

/**
 * @brief Fails the render when its accumulated output has passed its `output_len_max`.
 *
 * This runs after each append rather than before. Escaping expands a byte into as much as a
 * six-byte entity, so the incoming chunk length alone does not bound the growth. The overshoot is
 * one append, and an append is not necessarily small. `node_dump` hands `out_fn` a whole scalar in
 * a single call, and mustache4c hands `out_verbatim` each literal run of a template or partial
 * whole, so the largest append is the longest literal run or scalar. For `{{{body}}}` that scalar
 * is an entry's entire rendered HTML. md4c is not on this path at all. It fills `markdown.c`'s own
 * buffer and never reaches these callbacks. A non-zero return aborts `mustache_process`.
 *
 * @param render_output Render destination whose buffer length is checked. Must not be `NULL`.
 * @return `0` when the render may continue, or `-1` once the limit is passed.
 */
static int check_output_size(const struct RenderOutput* render_output) __attribute__((nonnull(1)));

/**
 * @brief Writes a node's terminal text through `out_fn`.
 *
 * This is installed in `provider` as the value-rendering callback. Only scalar nodes produce text.
 * Other node kinds write nothing.
 *
 * @param node_ptr          Node to dump, as a `struct Node*`.
 * @param out_fn            Output callback supplied by the renderer.
 * @param renderer_data     Renderer state forwarded to `out_fn`.
 * @param provider_data_ptr Unused provider state.
 * @return The result of `out_fn`, or `0` for a non-scalar node.
 */
static int node_dump(void* node_ptr,
                     int (*out_fn)(const char*, size_t, void*),
                     void* renderer_data,
                     void* provider_data_ptr);

/**
 * @brief Returns the top-level lookup context node.
 *
 * This is installed in `provider` as the root-resolution callback.
 *
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The root node for name and index resolution.
 */
static void* node_get_root(void* provider_data_ptr);

/**
 * @brief Resolves a named child of a node.
 *
 * This is installed in `provider` as the by-name lookup callback. At the root a name selects the
 * `site.*` subtree, the `content_entries` list, the `generator` string that names this tool and its
 * version, or otherwise a field of the entry being rendered, such as `title`.
 *
 * @param node_ptr          Parent node, as a `struct Node*`.
 * @param name              Requested child name. Not `NUL`-terminated.
 * @param name_len          Length of `name` in bytes.
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The child node, or `NULL` when the name is not present.
 */
static void* node_get_child_by_name(void* node_ptr,
                                    const char* name,
                                    size_t name_len,
                                    void* provider_data_ptr);

/**
 * @brief Resolves an indexed child of a node.
 *
 * This is installed in `provider` as the by-index lookup callback. Iterates content-entry and tag
 * lists by index. A non-list value is iterable once at index 0.
 *
 * @param node_ptr          Parent node, as a `struct Node*`.
 * @param index             Zero-based child index.
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The child node, or `NULL` when the index is out of range.
 */
static void* node_get_child_by_index(void* node_ptr, unsigned index, void* provider_data_ptr);

/**
 * @brief Loads, compiles, and caches a `partials/<name>.html` template.
 *
 * This is installed in `provider` as the partial-resolution callback, which mustache4c calls once
 * per expansion. Flags the render as failed when expansions pass the bound, the name is unsafe, the
 * cache is full, or the partial cannot be loaded or compiled.
 *
 * @param name              Partial name. Not `NUL`-terminated.
 * @param name_len          Length of `name` in bytes.
 * @param provider_data_ptr Pointer to the render's `struct ProviderData`.
 * @return The compiled partial template, or `NULL` on failure.
 */
static MUSTACHE_TEMPLATE* node_get_partial(const char* name,
                                           size_t name_len,
                                           void* provider_data_ptr);

/**
 * @brief Returns the already-compiled partial registered under `name`, if any.
 *
 * This matches on the length-delimited name, so the lookup runs before copying the name into the
 * arena. A partial referenced inside a loop resolves once per iteration, and copying first would
 * leak an arena allocation per occurrence.
 *
 * @param provider_data Render state holding the compile cache. Must not be `NULL`.
 * @param name          Partial name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The cached compiled partial, or `NULL` when the name is not cached.
 */
static MUSTACHE_TEMPLATE* node_get_partial_cached(const struct ProviderData* provider_data,
                                                  const char* name,
                                                  size_t name_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Loads and compiles `partials/<name>.html`, reporting the reason it could not.
 *
 * Releases the read buffer on both the success and failure paths. Does not register the result in
 * the compile cache. The caller owns that bookkeeping.
 *
 * @param provider_data Render state supplying `templates_dir` and the arena, and receiving a
 *                      diagnostic on failure. Must not be `NULL`.
 * @param name          Terminated partial name, already validated as a safe identifier. Must not be
 *                      `NULL`.
 * @return The compiled partial template, or `NULL` on a path, read, or compile failure.
 */
static MUSTACHE_TEMPLATE* node_get_partial_compile(struct ProviderData* provider_data,
                                                   const char* name) __attribute__((nonnull(1, 2)));

/**
 * @brief Builds the `partials/<name>.html` template path rooted under the render's `templates_dir`.
 *
 * Reports its own failure, because the two causes are distinct to the user. A name too long for
 * `PARTIAL_PATH_SIZE` names that limit, while a failed join is an allocation failure.
 *
 * @param provider_data     Render state supplying `templates_dir` and the arena, and receiving a
 *                          diagnostic on failure. Must not be `NULL`.
 * @param name              Terminated partial name, already validated as a safe identifier. Must
 *                          not be `NULL`.
 * @param relative_path_out Receives the `partials/<name>.html` form, arena-owned, on success only.
 *                          Handed back rather than rebuilt by the caller because that is the form
 *                          `record_parse_error` names. Must not be `NULL`.
 * @return The terminated joined path owned by the render arena, or `NULL` on an oversize name or
 *         allocation failure.
 */
static char* node_get_partial_path(struct ProviderData* provider_data,
                                   const char* name,
                                   const char** relative_path_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Allocates an arena-owned node of the given kind for the current render.
 *
 * Zeroes every field other than `kind`, so a caller sets only the fields its kind carries. Flags
 * the render as failed on allocation failure.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param kind          Node kind to assign.
 * @return The new node, or `NULL` on allocation failure.
 */
static struct Node* node_alloc(struct ProviderData* provider_data, enum NodeKind kind)
    __attribute__((nonnull(1)));

/**
 * @brief Returns a scalar node wrapping `value`, or `NULL` when `value` is absent or empty.
 *
 * @param provider_data Render provider state owning the node arena. Must not be `NULL`.
 * @param value         Terminated string to wrap, borrowed for the rest of the render, or `NULL`.
 * @return A scalar node, or `NULL` when `value` is `NULL` or empty, or on allocation failure.
 */
static struct Node* node_scalar(struct ProviderData* provider_data, const char* value)
    __attribute__((nonnull(1)));

/**
 * @brief Resolves a `site.*` name against the render's site configuration and timestamp.
 *
 * @param provider_data Render provider state holding the site configuration and `site.updated`.
 *                      Must not be `NULL`.
 * @param name          Requested field name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The resolved node, or `NULL` when the name is unmatched or its value is unset.
 */
static struct Node* resolve_site(struct ProviderData* provider_data,
                                 const char* name,
                                 size_t name_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Resolves a bare field name against a content entry.
 *
 * @param provider_data Render provider state. Must not be `NULL`.
 * @param entry         Content entry to resolve against, or `NULL`.
 * @param name          Requested field name. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len      Length of `name` in bytes.
 * @return The resolved node, or `NULL` when the name is unmatched, the field is absent, or `entry`
 *         is `NULL`.
 */
static struct Node* resolve_entry(struct ProviderData* provider_data,
                                  const struct ContentEntry* entry,
                                  const char* name,
                                  size_t name_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether a length-delimited name equals a terminated literal.
 *
 * @param name     Name bytes. Not `NUL`-terminated. Must not be `NULL`.
 * @param name_len Length of `name` in bytes.
 * @param expected Terminated literal to compare against. Must not be `NULL`.
 * @return `true` when the two are byte-for-byte equal, `false` otherwise.
 */
static bool is_name_equal(const char* name, size_t name_len, const char* expected)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Records the render's first failure diagnostic and marks the render as failed.
 *
 * Only the first message is kept: it names the root cause, and later callbacks typically fail as a
 * consequence of it. Writes nothing when the caller asked for no diagnostic.
 *
 * @param provider_data Render provider state holding the diagnostic buffer and failure flag. Must
 *                      not be `NULL`.
 * @param fmt           `printf`-style format string. Must not be `NULL`.
 * @param ...           Arguments for `fmt`.
 */
static void render_fail(struct ProviderData* provider_data, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Records a template syntax error, naming the reason and where it is.
 *
 * Installed in `parser` as mustache4c's parse-error callback. Without it the library substitutes a
 * no-op and `mustache_compile` reports only that it failed, so the line and column are lost and the
 * user has to find a malformed tag by eye.
 *
 * mustache4c can report several errors for one template, such as the secondary
 * `MUSTACHE_ERR_SECTIONOPENERHERE` note locating the opener of an unclosed section. `render_fail`
 * keeps the first message, which is the primary error.
 *
 * @param err_code    mustache4c `MUSTACHE_ERR_*` code. Unused, since `msg` already names the cause.
 * @param msg         Library message for `err_code`. Must not be `NULL`.
 * @param line        One-based line of the offending tag.
 * @param column      One-based column of the offending tag.
 * @param parser_data Pointer to the render's `struct ProviderData`. Must not be `NULL`.
 */
static void record_parse_error(int err_code,
                               const char* msg,
                               unsigned line,
                               unsigned column,
                               void* parser_data) __attribute__((nonnull(2, 5)));

/**
 * Renderer dispatch table. These tables are `const` so they stay in read-only data. Renders run
 * concurrently in pool jobs and `src/` deliberately holds zero writable globals. `site_config.c`'s
 * default template arrays have this same shape, and land in read-only data only because their
 * elements are `const` too.
 */
static const MUSTACHE_RENDERER renderer = {out_verbatim, out_escaped};

/** Data-provider dispatch table. */
static const MUSTACHE_DATAPROVIDER provider = {node_dump, node_get_root, node_get_child_by_name,
                                               node_get_child_by_index, node_get_partial};

/** Parser dispatch table. */
static const MUSTACHE_PARSER parser = {record_parse_error};

char* template_render_file(const char* templates_dir,
                           const char* template_name,
                           const struct TemplateContext* context,
                           size_t* html_len_out,
                           char* err,
                           size_t err_len) {
  return template_render_file_limited(templates_dir, template_name, context,
                                      (size_t)RENDER_OUTPUT_LEN_MAX, html_len_out, err, err_len);
}

char* template_render_file_limited(const char* templates_dir,
                                   const char* template_name,
                                   const struct TemplateContext* context,
                                   size_t output_len_max,
                                   size_t* html_len_out,
                                   char* err,
                                   size_t err_len) {
  if (!path_is_safe_relative(template_name)) {
    (void)error_report(err, err_len, "template must be a safe relative template name: '%s'",
                       template_name);
    return NULL;
  }

  // Every allocation this render makes goes into `scratch`, which this call creates and destroys.
  // Renders run concurrently in pool jobs, so the per-call arena keeps two renders out of each
  // other's memory. Nothing in this file may allocate into an arena reached through `context`.
  struct Arena scratch;
  arena_init(&scratch);
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  struct Node node_root = {.kind = NODE_ROOT};
  // Because this literal names only the non-zero values, initialization cannot accidentally omit a
  // new `ProviderData` field. `partial_count`, `expansion_count`, and `has_failed` start at zero.
  // The code assigns `partials` and `partial_names` when a partial compiles. It assigns
  // `template_name_compiling` before both `mustache_compile` calls. Only those calls can reach
  // `record_parse_error`.
  struct ProviderData provider_data = {.templates_dir = templates_dir,
                                       .template_name = template_name,
                                       .context = context,
                                       .arena = &scratch,
                                       .node_root = &node_root,
                                       .err = err,
                                       .err_len = err_len};
  struct RenderOutput render_output = {
      .buffer = &buffer, .output_len_max = output_len_max, .provider_data = &provider_data};

  char* template_data = NULL;
  size_t template_len = 0;
  MUSTACHE_TEMPLATE* templ = NULL;
  char* rendered_html = NULL;
  size_t rendered_len = 0;

  char* template_path = path_join(templates_dir, template_name, &scratch);
  if (template_path == NULL) {
    (void)error_report(err, err_len, "out of memory building path for template '%s'",
                       template_name);
    goto cleanup;
  }
  char reason[FS_REASON_SIZE];
  if (fs_read_file(template_path, TEMPLATE_FILE_LEN_MAX, &template_data, &template_len, reason,
                   sizeof(reason)) != 0) {
    (void)error_report(err, err_len, "failed to read template: %s ('%s')", reason, template_path);
    goto cleanup;
  }

  provider_data.template_name_compiling = template_name;
  templ = mustache_compile(template_data, template_len, &parser, &provider_data, 0);
  if (templ == NULL) {
    // `record_parse_error` has already named the reason and its line for a syntax error. Only fall
    // back to a generic message when the compile failed without reporting one.
    if (!provider_data.has_failed) {
      (void)error_report(err, err_len, "failed to compile template '%s'", template_name);
    }
    goto cleanup;
  }
  // The render needs both failure signals. `mustache_process` reports only a callback that returned
  // non-zero, and a `NULL` from `node_get_partial` or `node_get_child_by_name` means *absent* to
  // mustache4c rather than *failed*. It skips the partial and renders on. Without `has_failed` an
  // unreadable partial or a failed node allocation would yield a successful, silently incomplete
  // page. This line is also the boundary where mustache4c's zero/non-zero convention becomes this
  // project's `NULL`-on-failure producer contract.
  if (mustache_process(templ, &renderer, &render_output, &provider, &provider_data) != 0 ||
      provider_data.has_failed) {
    // A failing provider callback has already recorded the specific cause. Only fall back to a
    // generic message when the renderer itself failed.
    if (!provider_data.has_failed) {
      (void)error_report(err, err_len, "failed to render template '%s'", template_name);
    }
    goto cleanup;
  }
  // A template that rendered nothing still yields an allocated, terminated buffer to steal.
  rendered_len = buffer.len;
  rendered_html = string_buffer_steal(&buffer);
  if (rendered_html == NULL) {
    (void)error_report(err, err_len, "out of memory rendering template '%s'", template_name);
  } else if (html_len_out != NULL) {
    *html_len_out = rendered_len;
  }

cleanup:
  for (size_t i = 0; i < provider_data.partial_count; i++) {
    mustache_release(provider_data.partials[i]);
  }
  if (templ != NULL) {
    mustache_release(templ);
  }
  free(template_data);
  string_buffer_free(&buffer);
  arena_free(&scratch);
  return rendered_html;
}

static int out_verbatim(const char* output, size_t output_len, void* renderer_data) {
  struct RenderOutput* render_output = renderer_data;
  if (string_buffer_append_len(render_output->buffer, output, output_len) != 0) {
    render_fail(render_output->provider_data, "out of memory rendering template '%s'",
                render_output->provider_data->template_name);
    return -1;
  }
  return check_output_size(render_output);
}

static int out_escaped(const char* output, size_t output_len, void* renderer_data) {
  struct RenderOutput* render_output = renderer_data;
  if (html_escape_append_len(render_output->buffer, output, output_len) != 0) {
    render_fail(render_output->provider_data, "out of memory rendering template '%s'",
                render_output->provider_data->template_name);
    return -1;
  }
  return check_output_size(render_output);
}

static int check_output_size(const struct RenderOutput* render_output) {
  if (render_output->buffer->len <= render_output->output_len_max) {
    return 0;
  }
  render_fail(render_output->provider_data,
              "render exceeds max rendered output (%zu bytes) at %zu bytes (in '%s')",
              render_output->output_len_max, render_output->buffer->len,
              render_output->provider_data->template_name);
  return -1;
}

static int node_dump(void* node_ptr,
                     int (*out_fn)(const char*, size_t, void*),
                     void* renderer_data,
                     void* provider_data_ptr) {
  (void)provider_data_ptr;
  const struct Node* node = node_ptr;
  if (node->kind != NODE_SCALAR) {
    return 0;
  }
  return out_fn(node->scalar, strlen(node->scalar), renderer_data);
}

static void* node_get_root(void* provider_data_ptr) {
  return ((struct ProviderData*)provider_data_ptr)->node_root;
}

static void* node_get_child_by_name(void* node_ptr,
                                    const char* name,
                                    size_t name_len,
                                    void* provider_data_ptr) {
  struct ProviderData* provider_data = provider_data_ptr;
  const struct Node* node = node_ptr;
  switch (node->kind) {
    case NODE_ROOT:
      if (is_name_equal(name, name_len, "site")) {
        return node_alloc(provider_data, NODE_SITE);
      }
      if (is_name_equal(name, name_len, "content_entries")) {
        return node_alloc(provider_data, NODE_ENTRY_LIST);
      }
      if (is_name_equal(name, name_len, "generator")) {
        return node_scalar(provider_data, sosig_generator_string());
      }
      return resolve_entry(provider_data, provider_data->context->content_entry_current, name,
                           name_len);
    case NODE_SITE:
      return resolve_site(provider_data, name, name_len);
    case NODE_ENTRY:
      return resolve_entry(provider_data, node->entry, name, name_len);
    case NODE_ENTRY_LIST:
    case NODE_TAG_LIST:
    case NODE_SCALAR:
      return NULL;
  }
  // Unreachable: the switch covers every `enum NodeKind`, and omits `default:` so that `-Wswitch`
  // fails the build when a kind is added without a case here.
  abort();
}

static void* node_get_child_by_index(void* node_ptr, unsigned index, void* provider_data_ptr) {
  struct ProviderData* provider_data = provider_data_ptr;
  struct Node* node = node_ptr;
  if (node->kind == NODE_ENTRY_LIST) {
    if (index >= provider_data->context->content_entry_count) {
      return NULL;
    }
    struct Node* child = node_alloc(provider_data, NODE_ENTRY);
    if (child != NULL) {
      child->entry = provider_data->context->content_entries[index];
    }
    return child;
  }
  if (node->kind == NODE_TAG_LIST) {
    const struct ContentEntry* entry = node->entry;
    if (index >= entry->tag_count) {
      return NULL;
    }
    // This is deliberately not `node_scalar`. That maps `""` to `NULL` so an unset optional field
    // is falsey as a section, but here `NULL` is how this callback says end of list, so an empty
    // tag would silently drop every tag after it. A tag list carries an explicit count, so every
    // index below it yields a node whatever the tag holds.
    struct Node* child = node_alloc(provider_data, NODE_SCALAR);
    if (child != NULL) {
      child->scalar = entry->tags[index];
    }
    return child;
  }
  // `vendor/mustache4c/mustache.h` requires this. Per the Mustache specification a single value is
  // iterable too, so this callback returns the node itself for index 0 and `NULL` for every other
  // index. Section truthiness uses the same call. mustache4c asks for child 0 to decide whether
  // `{{#x}}` enters and whether `{{^x}}` fires.
  return index == 0 ? node : NULL;
}

static MUSTACHE_TEMPLATE* node_get_partial(const char* name,
                                           size_t name_len,
                                           void* provider_data_ptr) {
  struct ProviderData* provider_data = provider_data_ptr;

  // The counter increments before the cache lookup, so the bound holds for a cycle whose partial is
  // already compiled. This is the render's only termination guard.
  provider_data->expansion_count++;
  if (provider_data->expansion_count > (size_t)RENDER_EXPANSION_COUNT_MAX) {
    render_fail(provider_data,
                "render exceeds max partial expansions (%d); check for a partial that includes "
                "itself (in '%s')",
                RENDER_EXPANSION_COUNT_MAX, provider_data->template_name);
    return NULL;
  }

  // The cache lookup precedes the arena copy below, so a repeated reference costs no allocation.
  MUSTACHE_TEMPLATE* cached = node_get_partial_cached(provider_data, name, name_len);
  if (cached != NULL) {
    return cached;
  }

  char* name_z = arena_strndup(provider_data->arena, name, name_len);
  if (name_z == NULL) {
    render_fail(provider_data, "out of memory resolving partial '%.*s'", (int)name_len, name);
    return NULL;
  }
  if (!text_is_safe_identifier(name_z)) {
    // This is the directory-traversal guard, so it cannot be dropped as redundant input validation.
    // The partial name becomes a filesystem path, so this check validates it here rather than
    // trusting it to mustache4c. `mustache_validate_tagname` happens to reject `..` and to accept
    // `/`, and neither rule is part of `mustache.h`'s contract, so `{{>a/b}}` does reach this
    // callback and only this check keeps a partial inside `partials/`. The name trails the reason
    // because `mustache.h` bounds a tag name nowhere, so a long `{{>...}}` would otherwise truncate
    // the reason away.
    render_fail(provider_data, "partial name must contain only letters, digits, '_' and '-': '%s'",
                name_z);
    return NULL;
  }
  if (provider_data->partial_count >= RENDER_PARTIAL_COUNT_MAX) {
    render_fail(provider_data,
                "render exceeds max distinct partials (%d) while resolving partial '%s' (in '%s')",
                RENDER_PARTIAL_COUNT_MAX, name_z, provider_data->template_name);
    return NULL;
  }

  MUSTACHE_TEMPLATE* templ = node_get_partial_compile(provider_data, name_z);
  if (templ == NULL) {
    return NULL;
  }
  provider_data->partials[provider_data->partial_count] = templ;
  provider_data->partial_names[provider_data->partial_count] = name_z;
  provider_data->partial_count++;
  return templ;
}

static MUSTACHE_TEMPLATE* node_get_partial_cached(const struct ProviderData* provider_data,
                                                  const char* name,
                                                  size_t name_len) {
  for (size_t i = 0; i < provider_data->partial_count; i++) {
    if (is_name_equal(name, name_len, provider_data->partial_names[i])) {
      return provider_data->partials[i];
    }
  }
  return NULL;
}

static MUSTACHE_TEMPLATE* node_get_partial_compile(struct ProviderData* provider_data,
                                                   const char* name) {
  const char* relative_path = NULL;
  char* partial_path = node_get_partial_path(provider_data, name, &relative_path);
  if (partial_path == NULL) {
    return NULL;
  }
  char* partial_data = NULL;
  size_t partial_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(partial_path, TEMPLATE_FILE_LEN_MAX, &partial_data, &partial_len, reason,
                   sizeof(reason)) != 0) {
    render_fail(provider_data, "failed to read partial: %s ('%s')", reason, partial_path);
    return NULL;
  }

  // This uses the relative path, not the bare `name`. `record_parse_error` composes this into
  // `(in '<file>')`, which must denote a file.
  provider_data->template_name_compiling = relative_path;
  MUSTACHE_TEMPLATE* templ = mustache_compile(partial_data, partial_len, &parser, provider_data, 0);
  // Freeing this immediately is safe because `mustache_compile` copies every literal run and tag
  // name into the instruction buffer it returns. `mustache.h` never states that lifetime, so this
  // rests on reading `mustache.c`. A vendor that started borrowing `templ_data` would turn this
  // into a use-after-free for the rest of the render. The `free` also precedes the `templ == NULL`
  // check, so the failing path does not leak it.
  free(partial_data);
  if (templ == NULL) {
    // `record_parse_error` names a syntax error itself, and `render_fail` keeps the first message,
    // so this is the fallback for a compile that failed without reporting a reason.
    render_fail(provider_data, "failed to compile partial '%s'", name);
    return NULL;
  }
  return templ;
}

static char* node_get_partial_path(struct ProviderData* provider_data,
                                   const char* name,
                                   const char** relative_path_out) {
  char formatted[PARTIAL_PATH_SIZE];
  const int n = snprintf(formatted, sizeof(formatted), "partials/%s.html", name);
  if (n < 0 || (size_t)n >= sizeof(formatted)) {
    render_fail(provider_data,
                "partial path exceeds max partial path length (%zu bytes) at %d bytes: '%s'",
                sizeof(formatted) - 1, n, name);
    return NULL;
  }
  // This is copied into the arena because the caller holds it for the whole compile, while
  // `formatted` dies with this call.
  const char* relative_path = arena_strdup(provider_data->arena, formatted);
  char* partial_path = relative_path == NULL ? NULL
                                             : path_join(provider_data->templates_dir,
                                                         relative_path, provider_data->arena);
  if (partial_path == NULL) {
    render_fail(provider_data, "out of memory building path for partial '%s'", name);
    return NULL;
  }
  *relative_path_out = relative_path;
  return partial_path;
}

static struct Node* node_alloc(struct ProviderData* provider_data, enum NodeKind kind) {
  // This allocates a fresh node per resolution, never a shared singleton, even for the kinds that
  // carry no per-instance state. `vendor/mustache4c/mustache.h` requires each node of the hierarchy
  // to be uniquely identified by its pointer. Today's `mustache.c` only pushes and pops them, so
  // sharing would work, but that is an implementation detail a vendor bump can change without a
  // word. The allocation reads as an unnoticed inefficiency, so do not replace it with a shared
  // node.
  struct Node* node = arena_calloc(provider_data->arena, 1, sizeof(*node));
  if (node == NULL) {
    render_fail(provider_data, "out of memory building template render context");
    return NULL;
  }
  node->kind = kind;
  return node;
}

static struct Node* node_scalar(struct ProviderData* provider_data, const char* value) {
  // An empty value is absent, not present-and-empty, so `{{#field}}` skips it and `{{^field}}`
  // fires. mustache4c decides a section's truthiness by asking for child 0. A non-list node answers
  // with itself, so returning a node here for `""` would make a section over an unset field always
  // render. Interpolation is unchanged. `{{field}}` writes zero bytes whether the node is absent or
  // holds an empty string. `resolve_entry` already applies this same rule to an empty tag list.
  if (value == NULL || *value == '\0') {
    return NULL;
  }
  struct Node* node = node_alloc(provider_data, NODE_SCALAR);
  if (node != NULL) {
    node->scalar = value;
  }
  return node;
}

static struct Node* resolve_site(struct ProviderData* provider_data,
                                 const char* name,
                                 size_t name_len) {
  const struct SiteConfig* site_config = provider_data->context->site_config;
  if (is_name_equal(name, name_len, "title")) {
    return node_scalar(provider_data, site_config->title);
  }
  if (is_name_equal(name, name_len, "author")) {
    return node_scalar(provider_data, site_config->author);
  }
  if (is_name_equal(name, name_len, "base_url")) {
    return node_scalar(provider_data, site_config->base_url);
  }
  if (is_name_equal(name, name_len, "updated")) {
    // This is derived from the whole entry set rather than read off the configuration.
    return node_scalar(provider_data, provider_data->context->site_updated);
  }
  return NULL;
}

static struct Node* resolve_entry(struct ProviderData* provider_data,
                                  const struct ContentEntry* entry,
                                  const char* name,
                                  size_t name_len) {
  if (entry == NULL) {
    return NULL;
  }
  if (is_name_equal(name, name_len, "title")) {
    return node_scalar(provider_data, entry->title);
  }
  if (is_name_equal(name, name_len, "date")) {
    return node_scalar(provider_data, entry->date);
  }
  if (is_name_equal(name, name_len, "description")) {
    return node_scalar(provider_data, entry->description);
  }
  if (is_name_equal(name, name_len, "slug")) {
    return node_scalar(provider_data, entry->slug);
  }
  if (is_name_equal(name, name_len, "url")) {
    return node_scalar(provider_data, entry->url_path);
  }
  if (is_name_equal(name, name_len, "body")) {
    return node_scalar(provider_data, entry->body_html);
  }
  if (is_name_equal(name, name_len, "tags")) {
    if (entry->tag_count == 0) {
      return NULL;
    }
    struct Node* tags = node_alloc(provider_data, NODE_TAG_LIST);
    if (tags != NULL) {
      tags->entry = entry;
    }
    return tags;
  }
  return NULL;
}

static bool is_name_equal(const char* name, size_t name_len, const char* expected) {
  // The comparison must check the lengths first, and the `&&` short circuit guarantees it. `memcmp`
  // may read all `name_len` bytes however early the first difference falls, so comparing without
  // already knowing the lengths match reads past the end of `expected` whenever `name_len` is the
  // longer. That is undefined behavior, not merely a wrong answer.
  return strlen(expected) == name_len && memcmp(name, expected, name_len) == 0;
}

static void render_fail(struct ProviderData* provider_data, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (!provider_data->has_failed) {
    error_report_va(provider_data->err, provider_data->err_len, fmt, ap);
  }
  va_end(ap);
  provider_data->has_failed = true;
}

static void record_parse_error(int err_code,
                               const char* msg,
                               unsigned line,
                               unsigned column,
                               void* parser_data) {
  (void)err_code;
  struct ProviderData* provider_data = parser_data;
  render_fail(provider_data, "%s at line %u, column %u (in '%s')", msg, line, column,
              provider_data->template_name_compiling);
}
