# Architecture

`sosig` is a small C17 static site generator. It reads project configuration, Markdown content, and Mustache templates. It writes the generated files to a site-owned directory. This document describes the code structure. See [README.md](README.md) for usage and template behavior.

## Layout

- [src/app/](src/app/): Process entry point, command-line interface, and command boundaries.
- [src/build/](src/build/): Build orchestration, rendering passes, templates, manifest population, and output writing.
- [src/domain/](src/domain/): Site configuration, content metadata, frontmatter, permalinks, and the output manifest.
- [src/formats/](src/formats/): Boundaries for HTML, Markdown, and TOML data.
- [src/runtime/](src/runtime/): Filesystem and thread-pool services that interact with the host.
- [src/core/](src/core/): Small reusable primitives, containers, diagnostics, text, and path logic.
- [tests/](tests/): Golden test suite. Each fixture site under [tests/fixtures/](tests/fixtures/) has a matching expected output directory under [tests/expected/](tests/expected/). [tests/fixtures/site-file-permalink/](tests/fixtures/site-file-permalink/) covers Markdown and templates with the default permalink. [tests/fixtures/site-index-permalink/](tests/fixtures/site-index-permalink/) covers a permalink that publishes a directory index for each entry.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): Project entry point and supported build configurations.
- [cmake/](cmake/): Build profiles, quality tools, packaging, the golden test driver, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): Bundled dependencies (`copt`, `tomlc17`, `md4c`, `mustache4c`, `acutest`).

First-party dependencies point inward:

- `app` can depend on all inner directories.
- `build` can depend on `domain`, `formats`, `runtime`, and `core`.
- `domain` can depend on `formats`, `runtime`, and `core`.
- `formats` and `runtime` can depend on `core`.
- `core` cannot depend on another first-party directory.

A module can skip layers. Modules in the same directory can depend on each other. First-party include directives start at the `src/` root. For example, a module includes `"core/error.h"`. This convention makes each dependency visible at the call site.

## Entry point and commands

`main` ([src/app/main.c](src/app/main.c)) only calls `cli_dispatch` ([src/app/cli_dispatch.h](src/app/cli_dispatch.h)). This separation lets unit tests link the code that maps an action to an exit code. `cli_dispatch` uses `cli_parse` ([src/app/cli.h](src/app/cli.h)) to parse the command line. `cli_parse` selects a subcommand and one of four actions: version, help, run, or error. For the run action, `cli_dispatch` calls one of these functions:

- `cmd_build_run` ([src/app/cmd_build.h](src/app/cmd_build.h)): generates the site.
- `cmd_config_run` ([src/app/cmd_config.h](src/app/cmd_config.h)): loads `sosig.toml` and prints the resolved config.

Every command returns `enum ExitCode` ([src/app/exit_code.h](src/app/exit_code.h)): `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

## Build pipeline

The build has two layers:

- `cmd_build_run` is the outer boundary. It calls `cmd_build_execute`. On failure, it prints the collected diagnostic to `stderr` exactly once.
- `cmd_build_execute` owns the `BuildState`, calls six phases in sequence, and writes the verbose log. [entry_renderer](src/build/entry_renderer.h) parses entries and renders Markdown. [page_renderer](src/build/page_renderer.h) renders content pages. [manifest_builder](src/build/manifest_builder.h) plans output paths. [site_writer](src/build/site_writer.h) writes the generated site.

No phase writes a failure diagnostic directly to `stderr`. Each phase returns errors to its caller. Most phases return one message through a fixed `(char* err, size_t err_len)` pair. The boundary copies this message to the diagnostic buffer. A render phase can report many failures. It appends these failures to a growable `StringBuffer`.

Verbose status is separate from diagnostics. The main thread writes phase status lines. Render workers use `flockfile` and `funlockfile` to write synchronized progress dots.

The phase order follows the data dependencies. A content template can read `site.updated` and iterate through `content_entries`. These values are not available until the tool parses and sorts all entries. The build uses one parallel pass to parse entries and another to render pages. The collect-and-sort phase runs between the two passes.

1. **Load inputs** (`load_build_inputs`): The tool uses `site_config_load` to read and validate `sosig.toml`. It creates the output directory. Then `fs_list_files_with_suffix(content_dir, ".md", ...)` finds the content sources.
2. **Parse and render entries** (`render_content_entries` -> `entry_renderer_render_entries`): `pool_run` dispatches one job for each source file. Each job reads its file, separates the frontmatter from the body, and parses the frontmatter. For a non-draft, the job converts the Markdown body to HTML. It also creates the URL and output path. For a draft, the job frees the entry before these steps and leaves its result slot empty. Each job writes its entry and error to a separate result slot, which prevents shared mutable render state. After all workers finish, `pool_run` joins them. Then `render_job_run` adds each message to the growable error buffer. It adds one line for each failed entry. The optional synchronized progress dot is the only worker-side I/O.
3. **Collect** (`collect_content_entries`): The tool copies non-draft entries into one compact array. `content_entry_sort` sorts them from newest to oldest. `content_entry_latest_date` calculates the last-updated timestamp once. Each later render reads the sorted array and this timestamp.
4. **Manifest** (`populate_output_manifest` -> `manifest_builder_populate`): The tool records all output paths for content, aggregates, and feeds. It does this before it renders a page. It rejects three types of conflict:
   1. Two producers can claim the same path. `manifest_add` rejects the second claim.
   2. One output can use `a/b` while another uses `a/b/c`. The first path must be a file, but the second path requires a directory at `a/b`. `manifest_find_prefix_collision` detects this conflict after the tool records all paths.
   3. An output can name an existing build input. `manifest_builder_populate` compares the device and inode of each file. It checks the config, all discovered sources, and all directly configured templates. The sources include drafts. The templates include per-entry overrides. The check excludes partials because the renderer discovers their names only during rendering.
5. **Render pages** (`render_content_pages` -> `page_renderer_render_pages`): A second `pool_run` pass renders each content template into its result slot. The render context contains the sorted entries and `site.updated`.
6. **Write** (`write_generated_site` -> `site_writer`): The tool writes the content pages and frees their rendered buffers. Then it renders and writes each aggregate and feed template. This phase is not transactional. It does not remove outputs that are absent from the current manifest. A write or late template failure can leave new, old, and stale files together.

## Modules

### Domain (`src/domain`)

- [site_config](src/domain/site_config.h): Defines `SiteConfig` and its load and print operations. An arena owns the copied values that the load operation validates and normalizes. The load operation also applies optional defaults. Later phases use the resolved config without changing it. `base_url` needs an `http://` or `https://` scheme and a non-empty host. The scheme comparison is case-insensitive. The module removes trailing `/` characters from `base_url` and each configured directory. It also validates template names and permalink expansion.
- [content_entry](src/domain/content_entry.h): Defines the `ContentEntry` metadata struct. Its fields include source data, template data, and generated paths. A per-entry arena owns its strings. `content_entry_sort` sorts entries from newest to oldest and breaks date ties by output path. `content_entry_latest_date` returns the newest date for `site.updated`. It returns the Unix epoch for an empty site.
- [frontmatter](src/domain/frontmatter.h): `frontmatter_split` splits a Markdown file at its `+++` fences, and `frontmatter_parse` parses the TOML frontmatter into a content entry.
- [permalink](src/domain/permalink.h): `permalink_expand` creates an entry URL and replaces the `{slug}` and `{section}` tokens. It removes redundant `/` characters and appends `index.html` when the expanded path ends with `/`. The last expanded byte controls this decision, not the last pattern byte. The URL without its leading `/` is the relative output path, so the public URL names the output file. `site_config_load` tests sample values for path safety, distinct paths, and both output-path limits. Real entries repeat these checks with the same functions because they can be longer than the samples.
- [manifest](src/domain/manifest.h): An insertion-ordered map from `output_path` to `source_label`. The tool creates it before it renders page templates or writes output files. A hash index lets `manifest_add` reject an exact duplicate in O(1) average time. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path. For example, it finds the conflict between `a/b` and `a/b/c`.

### Build (`src/build`)

- [template](src/build/template.h): `template_render_file` uses mustache4c to render a template with a `TemplateContext`. Template names must be safe relative paths. Each render has three limits. The partial-expansion limit guarantees termination. The output-byte limit bounds memory use. The distinct-partial limit matches the cache capacity.
- [entry_renderer](src/build/entry_renderer.h): The first parallel pass. `entry_renderer_render_entries` parses each source into a per-path result slot. For a non-draft entry, it converts the Markdown body and creates the URL and output path. A draft leaves the slot empty. The module owns this per-job pipeline.
- [page_renderer](src/build/page_renderer.h): The second parallel pass. `page_renderer_render_pages` renders each parsed entry through its content template. Each render reads the complete sorted entry set and `site.updated`.
- [render_job](src/build/render_job.h): Defines `RenderJob`, `RenderJobSet`, and `render_job_run`. Both render passes fill `RenderJob` slots. Each slot contains a parsed entry, rendered HTML, and a diagnostic. `RenderJobSet` pairs a slot array with its length. `render_job_run` runs one worker-pool pass and collects its diagnostics. This module connects both render passes to the writer.
- [manifest_builder](src/build/manifest_builder.h): Builds the [manifest](src/domain/manifest.h) before the tool writes an output file. `manifest_builder_populate` records all content, aggregate, and feed output paths. It rejects duplicate paths, nested paths below an output file, and outputs that overwrite build inputs. Build inputs include the config, sources, and directly configured templates. The input check compares device and inode values. It detects alternate spellings, hard links, symlinks, and case-insensitive paths to the same file. The check excludes partials because the renderer resolves them later.
- [site_writer](src/build/site_writer.h): Writes the generated site from the rendered results. It writes each content page first. Then it renders and writes the aggregate and feed templates.

### Formats (`src/formats`)

- [markdown](src/formats/markdown.h): `markdown_to_html` wraps md4c with the project's extension set.
- [html](src/formats/html.h): HTML escaping of untrusted text for generated markup.
- [toml](src/formats/toml.h): TOML text validation plus date/datetime conversion and formatting.

### Runtime (`src/runtime`)

- [pool](src/runtime/pool.h): Runs indexed jobs through a bounded pthread pool. It completes all jobs, even if one job fails. It joins all workers before it returns. It also gets the default worker count from the number of online CPUs.
- [fs](src/runtime/fs.h): Finds and sorts matching regular files in a directory tree. It reads `NUL`-free text files and rejects files that change size during a read. It also creates directories, writes files, and gets device and inode values. The directory walk follows symlinked directories but skips cycles. It also skips dangling links and entries removed during the scan.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [arena](src/core/arena.h): Bump allocator and primary ownership tool.
- [error](src/core/error.h): Uniform diagnostic reporting for fallible actions.
- [grow](src/core/grow.h): Doubling-capacity arithmetic shared by the growable containers.
- [parse](src/core/parse.h): Parsing of terminated text into scalar values.
- [path](src/core/path.h): Path construction and validation.
- [path_list](src/core/path_list.h): Growable list of path strings.
- [string_buffer](src/core/string_buffer.h): Growable string buffer for accumulating output.
- [text](src/core/text.h): Text normalization and validation.

## Cross-cutting conventions

- **Ownership**: Arenas own groups of allocations. Some producer functions return a separate buffer. These functions are `markdown_to_html`, `template_render_file`, and `string_buffer_steal`. They transfer buffer ownership to the caller. The caller must free the buffer.
- **Text representation**: An owned string is a `NUL`-free C string. Only a borrowed slice carries a separate length. Examples include `FrontmatterSplit` and callback data from md4c and mustache4c. This representation makes `strlen` accurate for owned strings.
- **Text input checks**: `fs_read_file` rejects a file that contains a `NUL`. `toml_datum_is_text` rejects a TOML string that contains a decoded `NUL`. These checks prevent silent truncation of generated output. `fs_read_file` reports this policy failure separately from system errors.
- **Return values**: A producer returns a pointer or `NULL`. An action returns `0` or `-1`. A predicate returns `bool`.
- **Diagnostic buffers**: A fallible action can take a final `(char* err, size_t err_len)` pair. `error_report` fills this buffer and marks truncated text with `...`. Filesystem actions use a `(char* reason, size_t reason_len)` pair instead. This pair contains a reason fragment. The caller adds the operation and file information.
- **Diagnostic text**: First-party diagnostics use lowercase prose and name the failed operation first. They use single quotes for literal names and values. External messages keep their original capitalization and punctuation. An unbounded value follows the cause, so truncation removes the value instead of the reason.
- **Path safety**: Template names must be safe relative paths. Partial names use a more restrictive identifier grammar. Permalink expansion creates a public URL that starts with `/`. The tool removes this `/` to get the relative output path. It checks the path before it joins the relative path to `output_dir`. These text checks prevent `.` and `..` traversal. They cannot prevent a symlink inside a configured root from redirecting access.
- **Path limits**: The tool checks each output path against two limits. `OUTPUT_PATH_RELATIVE_LEN_MAX` limits the complete relative path. `FILENAME_LEN_MAX` limits each path segment.
