# Architecture

`sosig` is a small C17 static site generator. It reads project configuration, Markdown content, and Mustache templates. It writes the generated files to a site-owned directory. This document describes the code structure. See [README.md](README.md) for usage and template behavior.

## Layout

- [src/app/](src/app/): Process entry point, command-line interface, and command boundaries.
- [src/build/](src/build/): Build orchestration, rendering passes, templates, manifest population, and output writing.
- [src/domain/](src/domain/): Site configuration, content metadata, frontmatter, permalinks, and the output manifest.
- [src/formats/](src/formats/): Boundaries for HTML, Markdown, and TOML data.
- [src/runtime/](src/runtime/): Filesystem and thread-pool services that interact with the host.
- [src/core/](src/core/): Small reusable primitives, containers, diagnostics, text, and path logic.
- [tests/](tests/): Golden test suite and the shared unit-test support library ([tests/test_support.h](tests/test_support.h)). Each fixture site under [tests/fixtures/](tests/fixtures/) has a matching expected output directory under [tests/expected/](tests/expected/). [tests/fixtures/site_file_permalink/](tests/fixtures/site_file_permalink/) covers Markdown and templates with the default permalink. [tests/fixtures/site_index_permalink/](tests/fixtures/site_index_permalink/) covers a permalink that publishes a directory index for each entry.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): Project entry point and supported build configurations.
- [cmake/](cmake/): Build profiles, quality tools, packaging, the golden test driver, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): Bundled dependencies (`copt`, `tomlc17`, `md4c`, `mustache4c`, `sharedstuff`, `acutest`).

First-party dependencies point inward:

- `app` can depend on all inner directories.
- `build` can depend on `domain`, `formats`, `runtime`, and `core`.
- `domain` can depend on `formats`, `runtime`, and `core`.
- `formats` and `runtime` can depend on `core`.
- `core` cannot depend on another first-party directory.

A module can skip layers. Modules in the same directory can depend on each other. First-party include directives start at the `src/` root. For example, a module includes `"core/error.h"`. This convention makes each dependency visible at the call site. Vendored support is outside the first-party hierarchy.

## Entry point and commands

`main` ([src/app/main.c](src/app/main.c)) only calls `cli_dispatch` ([src/app/cli_dispatch.h](src/app/cli_dispatch.h)). This separation lets unit tests link the code that maps an action to an exit code. `cli_dispatch` uses `cli_parse` ([src/app/cli.h](src/app/cli.h)) to parse the command line. `cli_parse` selects a subcommand and one of four actions: version, help, run, or error. For the run action, `cli_dispatch` calls one of these functions:

- `cmd_build_run` ([src/app/cmd_build.h](src/app/cmd_build.h)): generates the site.
- `cmd_config_run` ([src/app/cmd_config.h](src/app/cmd_config.h)): loads `sosig.toml` and prints the resolved config.

`cli.c` is the one translation unit that defines `COPT_IMPL`. `cli_parse` never prints or exits. Every command returns `enum ExitCode` ([src/app/exit_code.h](src/app/exit_code.h)): `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

## Build pipeline

The build has two layers:

- `cmd_build_run` is the outer boundary. It calls `cmd_build_execute`. On failure, it prints the collected diagnostic to `stderr` exactly once.
- `cmd_build_execute` owns the `BuildState`, calls seven phases in sequence, and writes the verbose log. [entry_renderer](src/build/entry_renderer.h) parses entries and renders Markdown. [page_renderer](src/build/page_renderer.h) renders and writes content pages. [manifest_builder](src/build/manifest_builder.h) plans output paths. [site_writer](src/build/site_writer.h) writes the aggregate and feed outputs.

No phase writes a failure diagnostic directly to `stderr`. Each phase returns errors to its caller. Most phases return one message through a fixed `(char* err, size_t err_len)` pair. The boundary copies this message to the diagnostic buffer. A parallel phase can report many failures. It appends up to twenty distinct failures and a count of the remainder to a growable `StringBuffer`.

Verbose status is separate from diagnostics. The main thread writes phase status lines. Workers use `flockfile` and `funlockfile` to write a synchronized `\r<label> n/total` progress line that counts completed jobs, such as `parsing content 3/12`.

The phase order follows the data dependencies. A content template can read `site.updated` and iterate through `content_entries`. These values are not available until the tool parses and sorts all entries. The build uses one parallel pass to parse entries and another to render pages. The collect-and-sort phase runs between the two passes.

1. **Load inputs** (`load_build_inputs`): The tool uses `site_config_load` to read and validate `sosig.toml`. It requires each configured read root, `content_dir` and `templates_dir`, to be a usable directory, reporting the cause when one is not. `manifest_builder_check_output_dir` then rejects an `output_dir` at or below `content_dir` or `templates_dir`, before any input tree is walked. Then `fs_list_files_with_suffixes(content_dir, output_dir, {".md"}, ...)` finds the content sources. Every walk of an input tree leaves `output_dir` out by identity, so a symlink into the output tree cannot feed an earlier build's outputs back in as inputs.
2. **Parse and render entries** (`render_content_entries` -> `entry_renderer_render_entries`): `pool_run` dispatches one job for each source file. Each job reads its file, separates the frontmatter from the body, and parses the frontmatter. For a non-draft, the job converts the Markdown body to HTML. It also creates the URL and output path. For a draft, the job frees the entry before these steps and leaves its result slot empty. Each job writes its entry and error to a separate result slot, which prevents shared mutable render state. After all workers finish, `pool_run` joins them. Then `render_job_run` adds each distinct message to the growable error buffer, one line each, up to twenty, followed by a count of the remaining failures. The optional progress line is the only worker output to `stderr`.
3. **Collect** (`collect_content_entries`): The tool copies non-draft entries into one compact array. `content_entry_sort` sorts them from newest to oldest. `content_entry_latest_date` calculates the last-updated timestamp once. Each later render reads the sorted array and this timestamp.
4. **Manifest** (`populate_output_manifest` -> `manifest_builder_populate`): The tool records all output paths for content, aggregates, and feeds. It does this before it renders a page. It rejects four types of conflict:
   1. Two producers can claim the same path. `manifest_add` rejects the second claim. Paths that differ only in ASCII case are the same path, because a case-insensitive filesystem such as the macOS default stores them as one file. The comparison folds only `A-Z` to `a-z`. Other bytes must match exactly.
   2. One output can use `a/b` while another uses `a/b/c`. The first path must be a file, but the second path requires a directory at `a/b`. `manifest_find_prefix_collision` detects this conflict after the tool records all paths. It folds ASCII case in the same way, so `A/B` and `a/b/c` also conflict.
   3. An output can name an existing build input. `manifest_builder_populate` compares the device and inode of each file. It checks the config, all discovered sources, and every file below `templates_dir`. The sources include drafts. The template tree includes partials, per-entry overrides, and templates that no configuration names. A missing `templates_dir` adds no inputs, although load inputs has already rejected one in a full build. A `templates_dir` that exists but cannot be walked fails the build.
   4. An output can land at or below `content_dir` or `templates_dir`, even where no file exists yet. The next build would read a new `.md` file below `content_dir` as a source. The same build could read a new file below `templates_dir` as a partial. `manifest_builder_check_output_dir` has already compared the device and inode of each root with the nearest existing ancestor of `output_dir` and each of that directory's ancestors, during load inputs. That rejects an `output_dir` inside a root before any walk, whether or not `output_dir` exists yet. `manifest_builder_populate` runs the same check again as a backstop, so no caller can skip it, and then compares each root with each existing directory between `output_dir` and the output. That catches a symlink inside `output_dir` that leads into a root. These comparisons detect alternate spellings and symlinked roots. A sibling directory such as `contentx` does not match. A missing root is skipped. The tool runs this check before the input-file check, so an output below a root reports this conflict.
5. **Prepare output** (`prepare_output_dir`): The tool creates `output_dir` with `fs_mkdir_p`. This is the build's first write, so a build that an earlier phase refuses creates nothing, not even an empty `output_dir`. Later writes create their own parent directories.
6. **Render and write pages** (`render_content_pages` -> `page_renderer_render_pages`): A second `pool_run` pass renders each content template. The render context contains the sorted entries and `site.updated`. Each job writes its page to the entry's output path and frees the HTML before it returns, so each worker holds at most one page in memory. Jobs write concurrently. The manifest guarantees that no two jobs write the same file. Jobs can create the same parent directory at the same time, and `fs_write_file` accepts a directory that already exists. A failed write is that entry's diagnostic, like a failed render. The other pages are still written.
7. **Write aggregates and feeds** (`write_generated_site` -> `site_writer`): The tool renders and writes each aggregate and feed template. These run after every page, on the main thread, because each reads the whole sorted entry set.

The page and aggregate phases are not transactional. They do not remove outputs that are absent from the current manifest. A page failure or a late template failure can leave new, old, and stale files together.

## Modules

### Domain (`src/domain`)

- [site_config](src/domain/site_config.h): Defines `SiteConfig` and its load and print operations. An arena owns the copied values that the load operation validates and normalizes. The load operation rejects a key outside the schema, including a misspelled one, and applies optional defaults. Later phases use the resolved config without changing it. `base_url` needs an `http://` or `https://` scheme and a non-empty host. The scheme comparison is case-insensitive. The module removes trailing `/` characters from `base_url` and each configured directory. It also validates template names and permalink expansion.
- [content_entry](src/domain/content_entry.h): Defines the `ContentEntry` metadata struct. Its fields include source data, template data, and generated paths. A per-entry arena owns its strings, except `body_html`, which the entry adopts from `markdown_to_html` and frees in `content_entry_free`. `content_entry_sort` sorts entries from newest to oldest and breaks date ties by output path. `content_entry_latest_date` returns the newest date for `site.updated`. It returns the Unix epoch for an empty site.
- [frontmatter](src/domain/frontmatter.h): `frontmatter_split` splits a Markdown file at its `+++` fences, and `frontmatter_parse` parses the TOML frontmatter into a content entry. The parse rejects a key outside the schema, including a misspelled one.
- [permalink](src/domain/permalink.h): `permalink_expand` creates an entry URL and replaces the `{slug}` and `{section}` tokens. It removes redundant `/` characters and appends `index.html` when the expanded path ends with `/`. The last expanded byte controls this decision, not the last pattern byte. The URL without its leading `/` is the relative output path, so the public URL names the output file. `site_config_load` tests sample values for path safety, distinct paths, and both output-path limits. Real entries repeat these checks with the same functions because they can be longer than the samples.
- [manifest](src/domain/manifest.h): An insertion-ordered map from `output_path` to `source_label`. The tool creates it before it renders page templates or writes output files. A hash index keyed by the ASCII-folded path lets `manifest_add` reject a duplicate in O(1) average time. Two paths that differ only in ASCII case are duplicates. Other bytes, including non-ASCII bytes, compare exactly. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path. For example, it finds the conflict between `a/b` and `a/b/c`, and between `A/b` and `a/b/c`.

### Build (`src/build`)

- [template](src/build/template.h): `template_render_file` uses mustache4c to render a template with a `TemplateContext`. Template names must be safe relative paths, and partial names use a restricted identifier grammar. Each render has three limits. The partial-expansion limit stops a partial that includes itself, so every render terminates. The output-byte limit bounds the output buffer. The distinct-partial limit bounds the compiled-partial cache. The limits do not bound the total memory of a render. Templates are trusted input, and a template's nested sections use memory in proportion to the iterations that they request.
- [entry_renderer](src/build/entry_renderer.h): The first parallel pass. `entry_renderer_render_entries` allocates one result slot per source path and parses each source into its slot. For a non-draft entry, it converts the Markdown body and creates the URL and output path. A draft leaves the slot empty. The module owns this per-job pipeline.
- [page_renderer](src/build/page_renderer.h): The second parallel pass. `page_renderer_render_pages` renders each parsed entry through its content template and writes the page. Each render reads the complete sorted entry set and `site.updated`.
- [render_job](src/build/render_job.h): Defines `RenderJob`, `RenderJobSet`, `render_job_run`, and `render_job_set_free`. `entry_renderer_render_entries` allocates one `RenderJob` slot per source. Each slot contains a parsed entry and a diagnostic. `RenderJobSet` pairs a slot array with its length. `render_job_run` runs one worker-pool pass, reports synchronized progress, and collects at most twenty distinct diagnostics plus a count of the remainder. This module connects the two render passes.
- [output_path](src/build/output_path.h): Reports a generated output path that exceeds either output-path limit, with one wording for every producer. `entry_renderer` and `manifest_builder` share it.
- [manifest_builder](src/build/manifest_builder.h): Builds the [manifest](src/domain/manifest.h) before the tool writes an output file. `manifest_builder_populate` records all content, aggregate, and feed output paths. It rejects duplicate paths, nested paths below an output file, outputs at or below `content_dir` or `templates_dir`, and outputs that overwrite build inputs. `manifest_builder_check_output_dir` rejects an `output_dir` at or below either root before the build walks an input tree, whether or not `output_dir` exists yet. `manifest_builder_populate` runs it again as a backstop, then checks the directories below `output_dir`. The duplicate and nesting checks fold ASCII case. Build inputs include the config, sources, and every file below `templates_dir`, which includes partials. The root and input checks compare device and inode values. They detect alternate spellings, hard links, symlinks, and case-insensitive paths to the same file or directory.
- [site_writer](src/build/site_writer.h): Renders and writes the aggregate and feed templates after every content page is written.

### Formats (`src/formats`)

- [markdown](src/formats/markdown.h): `markdown_to_html` wraps md4c with the project's extension set.
- [html](src/formats/html.h): HTML escaping of untrusted text for generated markup.
- [toml](src/formats/toml.h): TOML text validation plus date/datetime conversion and formatting. `toml_require_known_keys` is the shared unknown-key check: it rejects the first table key that a loader's list of known keys does not contain.

### Runtime (`src/runtime`)

- [pool](src/runtime/pool.h): Runs indexed jobs through a bounded pthread pool. It completes all jobs, even if one job fails. It joins all workers before it returns. It also gets the default worker count from the number of online CPUs.
- [fs](src/runtime/fs.h): Finds and sorts matching regular files in a directory tree. It reads `NUL`-free text files and rejects files that change size during a read. It also creates directories, writes files, requires a path to be a usable directory and reports the cause when it is not, and gets device and inode values. Writes overwrite their destination in place and are neither atomic nor synced: a reproducible output directory makes deleting it and rebuilding a cheaper recovery than a temporary-file protocol. A new file gets mode `0666` reduced by the process umask. The directory walk follows symlinked directories but walks each directory once, by device and inode. A cycle or a second alias of a directory is skipped, so an aliased directory publishes one copy of its entries. A caller can also name one directory to leave out, which the walk records as already visited, so every path to it is skipped. The build leaves out `output_dir`. The walk visits entries in byte order and each symlinked directory only after every real one, so the listed path is the one without a symlink when one exists. It closes each directory before it descends, so only one directory is open at a time. It also skips dangling links and entries removed during the scan. `fs_read_file` takes a size limit from each caller and checks it against the `fstat` size of the opened file before any read or allocation.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [error](src/core/error.h): Uniform diagnostic reporting for fallible actions.
- [grow](src/core/grow.h): Doubling-capacity arithmetic shared by the growable containers.
- [parse](src/core/parse.h): Parsing of terminated text into scalar values.
- [path](src/core/path.h): Path construction and validation. `path_relative_below` returns the part of a path below a root by textual comparison. `entry_renderer` derives each entry's `{section}` from the source path below `content_dir`.
- [path_list](src/core/path_list.h): Growable list of path strings.
- [sosig_version](src/core/sosig_version.h): The tool name and version fixed when the build tree was configured.
- [text](src/core/text.h): Text copying, normalization, and validation.

## Cross-cutting conventions

- **Ownership**: Arenas own groups of allocations. Some producer functions return a separate buffer. These functions include `markdown_to_html`, `template_render_file`, `string_buffer_steal`, and `fs_read_file`. They transfer buffer ownership to the caller. The caller must free the buffer.
- **Text representation**: An owned string is a `NUL`-free C string. Only a borrowed slice carries a separate length. Examples include `FrontmatterSplit` and callback data from md4c and mustache4c. This representation makes `strlen` accurate for owned strings.
- **Text input checks**: `fs_read_file` rejects a file that contains a `NUL`. `toml_datum_is_text` rejects a TOML string that contains a decoded `NUL`. These checks prevent silent truncation of generated output. `fs_read_file` reports this policy failure separately from system errors.
- **Return values**: A producer returns a pointer or `NULL`. An action returns `0` or `-1`. A predicate returns `bool`.
- **Diagnostic buffers**: A fallible action can take a final `(char* err, size_t err_len)` pair. `error_report` fills this buffer and marks truncated text with `...`. Filesystem actions use a `(char* reason, size_t reason_len)` pair instead. This pair contains a reason fragment. The caller adds the operation and file information.
- **Diagnostic text**: First-party diagnostics use lowercase prose and name the failed operation first. They use single quotes for literal names and values. External messages keep their original capitalization and punctuation. An unbounded value follows the cause, so truncation removes the value instead of the reason.
- **Path safety**: Template names must be safe relative paths. Partial names use a more restrictive identifier grammar. Permalink expansion creates a public URL that starts with `/`. The tool removes this `/` to get the relative output path. It checks the path before it joins the relative path to `output_dir`. These text checks prevent `.` and `..` traversal. Input-root identity checks, the walks' exclusion of `output_dir`, and input-identity checks cover existing symlink aliases at their separate boundaries.
- **Input size limits**: Each `fs_read_file` caller passes a limit for the kind of file it reads. The read checks the limit before it allocates, so an oversize file is never loaded. `CONTENT_FILE_LEN_MAX` in `entry_renderer` equals `MARKDOWN_INPUT_LEN_MAX` (256 MiB), which the Markdown body could not exceed anyway, and it also bounds the frontmatter. `CONFIG_FILE_LEN_MAX` in `site_config` is 1 MiB. `TEMPLATE_FILE_LEN_MAX` in `template` is 4 MiB for each template and partial.
- **Path limits**: The tool checks each generated output path against two limits. `OUTPUT_PATH_RELATIVE_LEN_MAX` limits the complete relative path. `FILENAME_LEN_MAX` limits each path segment. `output_path_check_limits` reports either failure the same way for every producer.
- **Parallel ownership**: Each parse job allocates its own entry and that entry's arena. A worker receives immutable shared context and owns one index in each mutable result array. `pool_run` joins all workers before the main thread reads those slots.
