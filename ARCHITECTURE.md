# Architecture

`sosig` is a small C17 static site generator. It reads a TOML config, Markdown and Org content, and Mustache templates, and it writes the generated files to a directory that the site owns. This document describes the code: the directories and their dependency rules, the build pipeline, the job of each module, the conventions that every module follows, and the design decisions behind them. [README.md](README.md) covers usage and template behavior, [BUILD.md](BUILD.md) covers the CMake build, and [STYLE.md](STYLE.md) covers the code style.

## Layout

- [src/app/](src/app/): The process entry point, the command-line interface, and the command boundaries.
- [src/build/](src/build/): Build orchestration, the parallel phases, templates, manifest population, and output writing.
- [src/domain/](src/domain/): Site configuration, content metadata, frontmatter, permalinks, the build cache, and the output manifest.
- [src/formats/](src/formats/): The boundaries for HTML, Markdown, Org, and TOML data, and syntax highlighting.
- [src/runtime/](src/runtime/): The host interface and its POSIX implementation, the directory walk, the out-of-memory report, and the thread pool.
- [src/core/](src/core/): Small reusable primitives, containers, diagnostics, text, and path logic.
- [tests/](tests/): The golden test suite, the shared unit-test support library [tests/test_support.h](tests/test_support.h), and the in-memory host [tests/os_memory.h](tests/os_memory.h). Each fixture site under [tests/fixtures/](tests/fixtures/) has a matching directory of expected output under [tests/expected/](tests/expected/):
  - [site_file_permalink](tests/fixtures/site_file_permalink/) covers Markdown, Org, and templates with the default permalink.
  - [site_index_permalink](tests/fixtures/site_index_permalink/) covers a permalink that publishes a directory index for each entry.
  - [site_nested_sections](tests/fixtures/site_nested_sections/) covers nested and lopsided sections, source names with spaces, punctuation and non-ASCII characters, partials that include partials, and aggregate and feed outputs in subdirectories next to content outputs.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and the supported CMake configurations.
- [cmake/](cmake/): CMake profiles, quality tools, packaging, the golden test driver, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): The bundled dependencies `sharedstuff`, `copt`, `tomlc17`, `md4c`, `org4c`, `mustache4c`, `acutest`, and `tree-sitter` with these grammars:
  - `tree-sitter-c`
  - `tree-sitter-bash`
  - `tree-sitter-html`
  - `tree-sitter-css`
  - `tree-sitter-javascript`
  - `tree-sitter-typescript`
  - `tree-sitter-json`

### Dependency rules

First-party dependencies point inward:

- `app` can depend on every other directory.
- `build` can depend on `domain`, `formats`, `runtime`, and `core`.
- `domain` can depend on `formats`, `runtime`, and `core`.
- `formats` and `runtime` can depend on `core`.
- `core` depends on no other first-party directory.

A module can skip layers, and modules in the same directory can depend on each other. Vendored code is outside this hierarchy. A first-party include starts at the `src/` root, for example `#include "core/ascii.h"`, so every include names the directory that it depends on.

## Terms

These words have one meaning everywhere in this document:

- **Arena.** A `struct Arena` from `sharedstuff`. It has two pointers, `beg` and `end`, into one region. When the region is reserved and the arena commits pages on demand, it has a third pointer, `limit`. An arena never frees an individual allocation.
- **Perm.** A thread's arena for data that lives longer than the function that makes it. A function takes it by pointer, as `struct Arena* perm`.
- **Scratch.** A thread's arena for temporary data. A function takes it by value, as `struct Arena scratch`, so its allocations are free again when the function returns.
- **Log.** A `struct Log`: a growable buffer with one line for each error, and an error count. A command prints its log once. A log grows in an arena of its own, or in an arena that all users of the log get by pointer, so no callee gets the log's arena by value.
- **Block.** The one reservation that a command makes when it starts. The command splits the block into its arenas, and releases the block when it ends.
- **Code block.** A piece of code in a Markdown or Org body, which `body_html` writes and can highlight. The word "block" alone always means the reservation above.
- **Region.** A range of address space that one arena uses. Two arenas never use the same region at the same time.
- **Reserve.** To get address space from the system without memory behind it.
- **Commit.** To make reserved pages usable. The system charges committed pages against its memory limit.
- **Build.** One run of `sosig build`. [BUILD.md](BUILD.md) documents the CMake build of `sosig` itself.
- **Entry.** One content source that a build parses, as a `ContentEntry`. A record in the manifest is a manifest entry, and a file, directory or symlink in a directory listing is a directory entry.
- **Keep.** To leave an existing output in place without a new render, because the build cache shows that its inputs did not change. The output writer marks a kept output in the manifest, as it marks a written output.

## Entry point and commands

[main.c](src/app/main.c) defines `main`. `main` only calls `cli_dispatch` with the POSIX host, so the unit tests can link the code that maps an action to an exit code. These are the other modules in `src/app`:

- [cli](src/app/cli.h): `cli_parse` parses the command line into a subcommand and one of four actions: version, help, run, or error. It never prints and never exits. `cli.c` is the only translation unit that defines `COPT_IMPL`.
- [cli_dispatch](src/app/cli_dispatch.h): `cli_dispatch` calls `cli_parse`. For the run action, it calls the subcommand's function. For each other action, it prints one text through `os_print`: the diagnostic for the rejected command line, the `--help` text, or the `--version` text. It formats these texts in a reservation of its own, and releases that reservation when the command or the print returns. A command reserves its own memory.
- [cmd_build](src/app/cmd_build.h): `cmd_build_run` generates the site, as [Build pipeline](#build-pipeline) describes.
- [cmd_config](src/app/cmd_config.h): `cmd_config_run` loads `sosig.toml` and prints the resolved config.
- [exit_code](src/app/exit_code.h): Every command returns an `enum ExitCode`: `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

## Build pipeline

A build runs in two layers:

- `cmd_build_run` is the outer boundary. It resolves the worker count, and reserves the build's memory once, as one block of address space. It splits the block into arenas: the build's log, and a perm and a scratch for the main thread and for each worker. It calls `cmd_build_execute`, and then releases the block. If the build fails, it prints the build's log to `stderr` once, through `os_print`.
- `cmd_build_execute` owns the `BuildState`. It runs the eleven phases below in sequence, with the arenas that it gets, and it prints the verbose status lines. It runs the first phase itself, and `run_build_phases` runs the other ten.

The phases call these modules in `src/build`:

- `entry_renderer` parses entries and renders their Markdown or Org.
- `manifest_builder` plans the output paths.
- `page_renderer` renders and writes the content pages.
- `site_writer` writes the aggregate and feed outputs, and copies the static files and the assets.
- `output_pruner` removes what the build did not write.

Every change under `output_dir` goes through `output_writer`, which does not make the changes in a dry run.

### Phases

The phase order follows the data dependencies. A content template can read `site.updated` and iterate through `content_entries`, and neither value exists until the build parses and sorts every entry, so one parallel phase parses the entries, and the collect phase sorts them. A second parallel phase renders the pages only after that.

1. **Load inputs** (`load_build_inputs`): `site_config_load` reads and validates `sosig.toml`. Each configured read root must be a usable directory, and the build reports the cause when one is not. The build skips the optional static root when a site does not have one. Then, before the build walks an input tree, `manifest_builder_check_output_dir` rejects an `output_dir` that is at or below `content_dir`, `templates_dir`, or `static_dir`, and an `output_dir` that contains one of them or the config file. Then `walk_files_with_suffixes(paths, content_dir, output_dir, suffixes, ...)` finds and sorts every file below `content_dir` in one walk. `load_build_inputs_split_sources` splits that list into the content sources, the files that end in `.md` or `.org` in any letter case, and the assets, which are all the other files. A second walk finds the static files. Each walk of an input tree excludes `output_dir` by its identity, so a symlink into the output tree cannot send the outputs of an earlier build back in as inputs. A failure in this phase stops the build, with one exception: a rejected config key gets its default value, and the build continues. This exception does not apply to a directory key, `permalink`, `aggregate_templates`, or `feed_templates`, because their defaults name directories or outputs that the user did not choose.
2. **Parse and render entries** (`render_content_entries` -> `entry_renderer_render_entries`): The build allocates `source_entries`, with one slot for each source file, and `job_run` runs one job for each source file. Each job stats its file for the build cache, and reads the file up to `CONTENT_FILE_LEN_MAX`, so if a source grew past the limit after the walk, only its own job fails. For a Markdown source, the job separates the frontmatter from the body and parses the frontmatter. For an Org source, the job reads the keywords at the top of the file, and the body is the whole file. For an entry that is not a draft, the job converts the body to HTML with the converter for its format, and creates the URL and the output path. The job builds the entry in a copy of its worker's perm, and it stores the copy back as the worker's perm only when it fills the slot. A draft stops before conversion. A draft and a failed source leave their slot `NULL` and leave nothing in the arena. Each job writes its entry to its own `source_entries` slot, and writes its errors to its own log, in the log share of its worker. `pool_run` joins every worker before `job_run` merges the job logs into the build's log.
3. **Collect** (`collect_content_entries`): The build copies the entries that are not drafts into one compact array. `content_entry_sort` sorts them from newest to oldest, and `content_entry_date_latest` calculates the last-updated timestamp once. Every later render reads the sorted array and this timestamp.
4. **Manifest** (`populate_output_manifest` -> `manifest_builder_populate`): Before the build renders a page, it records every output path for content, aggregates, feeds, static files and assets, and it records the build cache's path last. It rejects four kinds of conflict:
   1. Two producers claim the same path, and `manifest_add` rejects the second claim. Two paths that differ only in ASCII case are the same path, because a case-insensitive filesystem, such as the macOS default, stores them as one file.
   2. One output uses `a/b` and another output uses `a/b/c`. The first path must be a file, but the second path needs a directory at `a/b`. `manifest_find_prefix_collision` finds this conflict after the build records every path. It folds ASCII case in the same way, so `A/B` and `a/b/c` also conflict.
   3. An output names an existing build input. `manifest_builder_populate` compares the device and inode of each output with the device and inode of these inputs: the config, every discovered source (drafts included), the static files, the assets, and every file below `templates_dir`. The template tree includes partials, per-entry overrides, and templates that no configuration names. A missing `templates_dir` adds no inputs, although in a full build the load inputs phase already rejected a missing `templates_dir`. If `templates_dir` exists but the build cannot walk it, the build fails.
   4. An output is at or below `content_dir`, `templates_dir`, or `static_dir`, even when no file exists there yet. Such an output would come back as an input: the next build would read it as a source or copy it as an asset below `content_dir`, or copy it as a static file below `static_dir`, and the same build could read it as a partial below `templates_dir`. In the load inputs phase, `manifest_builder_check_output_dir` already compared the device and inode of each root with the nearest existing ancestor of `output_dir`, and with each ancestor of that directory. This rejects an `output_dir` inside a root before any walk, whether `output_dir` exists yet or not. `manifest_builder_populate` then compares each root with each existing directory between `output_dir` and the output. This catches a symlink inside `output_dir` that points into a root. These comparisons detect alternate spellings and symlinked roots. A sibling directory such as `contentx` does not match, and the comparisons skip a missing root. This check runs before the input-file check, so for an output below a root, the build reports this conflict.
5. **Load the build cache** (`load_build_cache`): The build reads `_sosig/v<n>-build-cache.tsv` below `output_dir`, unless `--force` tells it to ignore the file. The file's first line is the page recipe: the config fields that a render reads, the generator string, the grammars that `sosig` links, and `PAGE_RECIPE_VERSION`. Each row holds the size and the nanosecond modification time of one input that the pages depend on. There is a row for the source of each entry, and a row for each file below `templates_dir`. An entry's row has its page's output path as its name, and records whether that page read the entry list. If the file is missing, cannot be read, does not parse, or starts with a different page recipe, the build uses an empty build cache and gives a verbose note. None of these cases is an error. The build then walks `templates_dir`, stats every template, and decides which outputs it can keep:
   - Every output needs a match for every template row, with no template added or removed, because the build cache tracks all the partials together and not the partials of each page.
   - A content page also needs a match between its own row and its source.
   - A page that read the entry list (`content_entries` or `site.updated`), and every aggregate and feed, also need a match for every entry's row, with no entry added or removed. Drafts have no rows, so when the user publishes a draft, or changes a page into a draft, the build counts an added or a removed entry.
6. **Prepare output** (`prepare_output_dir`, then `empty_build_cache`): The build creates `output_dir` through `output_writer_mkdirs`. This is the build's first write, so when an earlier phase stops a build, the build creates nothing, not even an empty `output_dir`. Later writes create their own parent directories. From this point, every change under `output_dir` goes through the build's `OutputWriter`. In a dry run, the writer makes no change: it creates no `output_dir`, writes no file, and records what the prune would remove instead of removing it. The build then writes the build cache as zero bytes. If a build fails or is interrupted after this point, it leaves a build cache that the next build does not use, so no later build keeps a page from this build, which could be incomplete or come from different inputs.
7. **Render and write pages** (`render_content_pages` -> `page_renderer_render_pages`): A second `job_run` phase renders each content template, with one job for each `source_entries` slot, and skips a `NULL` slot. If the build can keep a job's page, and the page exists, the job keeps the page through the output writer without a render. The job's new row then copies the entry-list flag from the old row. Every other job renders its page, and records whether the render read the entry list. The render context holds the sorted entries and `site.updated`. Each job renders its page into a copy of its worker's perm, after the entries stored there, and writes the page to the entry's output path through the output writer. The job drops the copy when it returns, so each worker holds at most one page in memory. Jobs write at the same time. The manifest makes sure that no two jobs write the same file. When two jobs create the same parent directory at the same time, both jobs succeed, because `os_write` accepts a directory that already exists. A failed write is a diagnostic for that entry, as a failed render is, and the build still writes the other pages.
8. **Write aggregates and feeds, and copy static files** (`write_generated_site` -> `site_writer`): The build renders and writes each aggregate and feed template. It keeps an existing aggregate or feed when every entry row and every template row match. This work runs on the main thread after every page, because each aggregate and feed reads the whole sorted set of entries. Then the build copies the discovered static files into `output_dir`, each at its path below `static_dir`, and then the assets, each at its path below `content_dir`. Each write truncates its destination in place, so neither this phase nor the page phase is transactional at any level. A page failure or a late template failure can leave new, old, and stale files together, and an interrupted write can leave one output truncated. To recover from both, delete the output directory and build again.
9. **Write the build cache** (`write_build_cache`): After the build writes or keeps every output, it writes the build cache through the output writer, with the page recipe and a row for each current entry and template. A dry run does not make this write or the earlier write of zero bytes, as it does not make any write.
10. **Remove stale outputs** (`prune_stale_outputs` -> `output_pruner_prune`): After the build writes every output, it removes every directory entry below `output_dir` that is not an output and is not a directory that an output is below, so the tree holds only what the current inputs produce. The prune compares device and inode values, not names, so on a case-insensitive filesystem it does not remove the old spelling of a current output. It sorts the identities once, so each lookup is a binary search. It removes a symlink as a link and never goes into one, so it touches nothing outside `output_dir`. It also never goes into a directory that is an input root. It removes a stale directory after it removes everything in it. When it cannot list or remove a directory entry, it reports the failure and continues. It does not report again a directory that such a failure left non-empty. A failed build never gets to this phase, so it removes nothing. In a dry run, the prune finds no `output_dir` when no earlier build made one, and then it has nothing to remove.
11. **Report the dry run** (`report_dry_run` -> `output_writer_report`): This phase runs only in a dry run. The build prints to `stdout`, in one `os_print` call, a `write` or `keep` line for each output path in manifest order. Then it prints a `remove` line for each path that the prune would remove, sorted bytewise. If an output path is still unmarked, a call site wrote without the output writer. In that case the report fails, so that it does not print a list that leaves out the write.

### Errors and status output

No phase writes a failure diagnostic to `stderr` itself. The one exception is the out-of-memory report, which `os_oom` prints through `memory_oom_report` before the process exits. Every fallible function records its errors in the log that its caller gives, and `cmd_build_run` prints the build's log once.

Until the first write, the build continues after an error wherever the next phase still has something to check. One run reports every bad config key, every source that does not parse, and every output collision. If the build recorded an error, it then stops before it creates `output_dir`. After the first write, a failed phase is the last phase that runs. The page phase still writes every other page, and reports each page that fails. `site_writer` stops at the first aggregate or feed template that fails. The prune runs only when every write succeeded, and it reports every directory entry that it cannot list or remove.

A parallel phase gives each job a log of its own. After `pool_run` joins the workers, `job_run` appends the logs of the failed jobs to the build's log in job order: at most `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct logs, then a count of the others. A page render ends its failure message with `(while rendering '<entry>')`, and the comparison of two logs stops at that text, so when every page repeats the same template failure, the build prints that failure once.

Verbose status is separate from diagnostics, and both go through `os_print`. The main thread writes a status line between phases. A worker prints nothing to `stderr` except its progress line and an out-of-memory report. The worker formats the whole progress line (`\r<label> n/total`, such as `parsing content 3/12`) before it prints it in one `os_print` call, under a lock that `job.c` owns, so the lines of two workers never interleave.

## Modules

Each item below links to the module's header, which documents its interface. The `src/app` modules are in [Entry point and commands](#entry-point-and-commands).

### Build (`src/build`)

- [entry_renderer](src/build/entry_renderer.h): The first parallel phase. `entry_renderer_render_entries` parses each source into its slot of an entry array that the caller owns, with one slot for each source path. For an entry that is not a draft, it converts the Markdown or Org body, and creates the URL and the output path. A draft leaves its slot `NULL`. The module owns this pipeline for each job.
- [job](src/build/job.h): `job_run` connects the two parallel phases to the worker pool. It gives each job a log of its own, in the share of the phase's scratch that the job's worker owns. It reports progress under a lock, and merges the logs of the failed jobs as [Errors and status output](#errors-and-status-output) describes. The caller can name the text that comes before each job's name at the end of a line. When the lines of two jobs differ only after that text, the two jobs count as one failure. Each job writes its results into its own slot, which the caller owns.
- [manifest_builder](src/build/manifest_builder.h): Builds the [manifest](src/domain/manifest.h) before the build writes a generated file. `manifest_builder_check_output_dir` checks `output_dir` against the input roots before the build walks an input tree. `manifest_builder_populate` records every content, aggregate, feed, static file, and asset output, and rejects the conflicts that the manifest phase lists. The root and input checks compare device and inode values, so they detect alternate spellings, hard links, symlinks, and case-insensitive paths to the same file or directory. The manifest and `site_writer` both use `path_join` to join each aggregate and feed template name, and the path of each static file below `static_dir` or asset below `content_dir`, to `output_dir`, so the path that the manifest checked is the path that the build writes.
- [output_path](src/build/output_path.h): Reports a generated output path that exceeds either output-path limit, with the same wording for every producer. `entry_renderer` and `manifest_builder` share it.
- [output_pruner](src/build/output_pruner.h): `output_pruner_prune` removes every directory entry below `output_dir` that the manifest does not name and that no output is below, as the prune phase describes.
- [output_writer](src/build/output_writer.h): Makes every change that a build makes under `output_dir`: the writes, the creation of `output_dir`, and the prune's removals. Each write or keep marks its path in the manifest. The writer fails on a path that the manifest does not name, because the same build's prune would remove that path. In a dry run, the writer makes no change and records the removals, and `output_writer_report` lists its marks and removals. The page renderer, the site writer, and the prune use the writer for their changes, and use `struct Os` for their reads.
- [page_renderer](src/build/page_renderer.h): The second parallel phase. `page_renderer_render_pages` renders each parsed entry through its content template and writes the page, or keeps the page when the build cache shows that its inputs did not change. Each render reads the complete sorted set of entries and `site.updated` through a template context that is `const` at every level.
- [site_writer](src/build/site_writer.h): After the page phase, renders and writes the aggregate and feed templates, or keeps the outputs that the build cache allows it to keep. Then it copies each static file and each asset with one streamed `os_write`. It stops at the first template or file that fails. `cmd_build` gives a feed only the newest `feed_count` entries.
- [template](src/build/template.h): `template_render_file` uses mustache4c to render a template with a `TemplateContext`. Template names must be safe relative paths, and partial names use a restricted identifier grammar. Each render has three limits. The partial-expansion limit stops a partial that includes itself, so every render ends. The output-byte limit sets the maximum size of the output buffer. The distinct-partial limit sets the maximum size of the compiled-partial cache. The limits do not bound a render's total memory, because templates are trusted input, and a template's nested sections use memory in proportion to the iterations that they request. A render also reports whether it resolved `content_entries` or `site.updated`, the two names whose values depend on every entry. The build cache records this for each page.

### Domain (`src/domain`)

- [build_cache](src/domain/build_cache.h): Parses and formats the build cache. The first line is the page recipe. Each later line is a row for one input, with tab-separated fields: kind, name, size, modification time and, for an entry, its page's entry-list flag. The rows are in order of kind, then of name. A name that contains a tab or a newline gets no row, so no name needs escaping. `BUILD_CACHE_VERSION` names the row format in the file name. `PAGE_RECIPE_VERSION` is the one constant to increase when a change to the code or to a vendored renderer must make the build rewrite the outputs that the build cache would keep.
- [content_entry](src/domain/content_entry.h): Defines the `ContentEntry` metadata struct. Its fields hold source data, template data, and generated paths. The entry and its strings, including `body_html`, live in the perm of the worker that parsed it. `content_entry_sort` sorts entries from newest to oldest, and uses the output path to order entries with the same date. `content_entry_date_latest` returns the newest date for `site.updated`, or the Unix epoch for an empty site.
- [frontmatter](src/domain/frontmatter.h): `frontmatter_split` splits a Markdown file at its `+++` fences. `frontmatter_parse` parses the TOML frontmatter into a content entry, and rejects a key that is not in the schema. `frontmatter_parse_org` reads the keywords at the top of an Org file into a content entry, with the same keys, required fields and template check, and ignores all other keywords.
- [manifest](src/domain/manifest.h): An insertion-ordered map from `output_path` to `source_label`. The build makes it before it renders a page template or writes an output file. A 4-ary hash trie, with the ASCII-folded path as its key, lets `manifest_add` reject a duplicate. Each trie level uses the next two bits of the path's 64-bit FNV-1a hash, so a lookup goes down O(log n) levels in expectation, and the trie never moves a manifest entry that it placed. Two paths that differ only in ASCII case are duplicates. The fold changes only `A-Z` to `a-z`, and all other bytes, including non-ASCII bytes, must match exactly. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path, such as `a/b` and `a/b/c`, or `A/b` and `a/b/c`. Each manifest entry also holds a mark, which is pending until the output writer marks the path as written or kept. A mark changes no trie link, so page jobs mark their own paths at the same time.
- [permalink](src/domain/permalink.h): `permalink_expand` creates an entry's URL: it replaces the `{slug}` and `{section}` tokens. It removes redundant `/` characters, and appends `index.html` when the expanded path ends with `/`. The expanded path's last byte decides this, not the pattern's. The URL without its leading `/` is the relative output path, so the public URL names the output file. `site_config_load` expands sample values to check two things: that every expansion is a safe relative path, and that different entries get different paths, so a bad pattern causes one config error. The build checks each entry's output path against both output-path limits when it parses the entry, because only a real entry's slug and section give the path's length.
- [site_config](src/domain/site_config.h): Defines `SiteConfig` and its load and print operations. The load validates and normalizes each value, and copies it into the arena that the caller gives. It rejects a key that is not in the schema, and applies the defaults of optional keys. Later phases use the resolved config and do not change it. `base_url` needs an `http://` or `https://` scheme, which the load compares without regard to case, and a host that is not empty. The load removes trailing `/` characters from `base_url` and from each configured directory, and validates template names and permalink expansion.

### Formats (`src/formats`)

- [body_html](src/formats/body_html.h): The markup that the Markdown and Org renderers share, written in one place for both: headings, footnotes, task checkboxes, admonitions, math, code blocks that `highlight` highlights, and the escaping of link targets.
- [highlight](src/formats/highlight.h): Syntax highlighting of a code block with tree-sitter. `highlight_grammar_find` finds the grammar for a language name among the grammars that `SOSIG_GRAMMARS` compiles in. `vendor/CMakeLists.txt` declares these grammars in a header that it generates. `highlight_grammar_list` names those grammars. `highlight_write` writes the code with a `<span class="tok-…">` around each token that the grammar's highlight query captures. It writes the code plain, only escaped, when there is no grammar or when the code block goes past a memory limit.
- [html](src/formats/html.h): HTML escaping of untrusted text for generated markup.
- [markdown](src/formats/markdown.h): `markdown_to_html` parses Markdown with md4c and the project's extension set, and writes the HTML through its own callbacks.
- [org](src/formats/org.h): `org_to_html` parses Org with org4c, the project's flags, and the file's `#+OPTIONS:` lines, and writes the HTML through its own callbacks.
- [toml](src/formats/toml.h): TOML text validation, and the conversion and formatting of dates and datetimes. `toml_require_known_keys` is the shared check for unknown keys. It reports each table key that is not in the list of known keys that a loader gives.

### Runtime (`src/runtime`)

- [fs](src/runtime/fs.h): The POSIX implementation of `os.h`, in an object library of its own, `sosig_os_posix`. The `sosig` executable and every unit test that is registered without `OS memory` link it, as [BUILD.md](BUILD.md#unit-tests) lists. Its `struct Os` has no state.
- [memory](src/runtime/memory.h): Holds what the out-of-memory report prints. This is the command's log, which `memory_oom_set_log` names, and, for each thread, the file that the thread works on and its job's log, which `memory_oom_set_job` names.
- [os](src/runtime/os.h): The host interface. Every read, write, directory creation, removal, listing, `stat` and print that the program does goes through `os_read`, `os_write`, `os_mkdirs`, `os_remove`, `os_remove_dir`, `os_list`, `os_stat` and `os_print`. Each function that accesses the host takes a `struct Os*`.
  - `os_read` takes a size limit from each caller, and checks it before any read or allocation. It rejects a file that is not a regular file, a file that contains a `NUL`, and a file that changes size during the read.
  - `os_write` writes either bytes or the contents of a file that it streams, and it creates parent directories. It overwrites its destination in place, through a symlink or a hard link, and the write is neither atomic nor synced, as [The `os` interface](#the-os-interface) explains. A new file gets mode `0666` reduced by the process umask.
  - `os_stat` reports identity, type, size, and modification time.
  - `os_list` closes a directory before it returns. It classifies each directory entry, and for a symlink it classifies the target. It skips dangling links, and directory entries that were removed after it read them.
  - `os_print` carries every byte that the program prints: the log at the end of a command, a rejected command line, `--help` and `--version`, the resolved config, the verbose status lines, and the workers' progress lines, so a test on the in-memory host sees all of it. One call writes its text as a whole. A caller that prints from more than one thread holds its own lock around the call.
  - `os_oom` is the one other member. `sharedstuff` declares it, and each program defines it.
- [pool](src/runtime/pool.h): Runs indexed jobs through a pthread pool of bounded size. It completes every job, even when one job fails, and joins every worker before it returns. Each job gets the index of the worker that runs it, so the job can use that worker's arenas. The module also gets the default worker count from the number of online CPUs.
- [walk](src/runtime/walk.h): Finds and sorts the matching regular files in a directory tree. It uses only `os_list` and `os_stat`, so it behaves the same on every host. It follows symlinked directories, but walks each directory once, by device and inode. It skips a cycle or a second alias of a directory, so an aliased directory publishes one copy of its files. A caller can name one directory to exclude. The walk records that directory as already visited, so it skips every path to it. The build excludes `output_dir`. The walk visits directory entries in byte order, and visits each symlinked directory only after every real directory, so it lists the path without a symlink when such a path exists. Only one directory is open at a time, and no level of the recursion holds a path buffer in its frame.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [error](src/core/error.h): Turns an `errno` into text, and is thread-safe. Diagnostics themselves go to the vendored `shared/log.h`, as `printf` formats that the compiler checks.
- [parse](src/core/parse.h): Parsing of text into scalar values.
- [path](src/core/path.h): Path construction and validation. `path_is_below` finds the part of a path below a root by text comparison. `entry_renderer` uses it to get the `{section}` of each entry from the source path below `content_dir`. `manifest_builder` uses it to plan the output of each static file and asset, and `site_writer` copies the file to the same path below `output_dir`.
- [path_list](src/core/path_list.h): A growable list of path strings.
- [sosig_version](src/core/sosig_version.h): The tool name and version, which CMake sets when it configures `sosig`.
- [text](src/core/text.h): Text normalization, matching and validation.

## Cross-cutting conventions

Every module follows these rules. When a design decision is behind a rule, the link goes to its reasons and costs.

### Memory

- Every first-party allocation comes from an arena, and each thread has a perm and a scratch. The build's log has an arena of its own. `markdown_to_html`, `org_to_html`, `template_render_file`, and `os_read` return their result in the caller's perm. See [Memory: arenas only](#memory-arenas-only) and [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).
- A command reserves its memory once, when it starts. It splits the block into arenas of fixed sizes, and the input and render limits bound those sizes. An arena commits pages only when its allocations get to them, and then commits one mebibyte more than the allocation needs. Only committed pages count against the system's memory. See [Memory: reserve, then commit on demand](#memory-reserve-then-commit-on-demand).
- An allocation that does not fit calls `os_oom`, which reports and exits, so no call site checks for allocation failure. The vendored libraries that still use `malloc` also end in `os_oom`, with the exceptions that the decision lists. See [Out of memory exits the process](#out-of-memory-exits-the-process).
- Under the address sanitizer, two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`. See [Address sanitizer coverage in arenas](#address-sanitizer-coverage-in-arenas).
- A log's arena is never an arena that a callee gets by value, so the log never grows into bytes that a callee uses. See [A log must not grow in a borrowed arena](#a-log-must-not-grow-in-a-borrowed-arena).

### Threads and ownership

Two jobs that run at the same time never share an arena or a log. Each job uses the arenas of the worker that runs it, and each worker has its own arenas. A parse job allocates its entry in its worker's perm. A worker gets immutable shared context, except for the manifest mark of the page that it writes, and it owns one index in each mutable result array. All workers share the highlighter's grammars and their compiled queries, which are immutable after they load under a lock. `pool_run` joins every worker before the main thread reads the result slots, and the entries stay in the worker arenas until the build ends. See [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).

### Strings and text input

Every first-party string is a counted `struct Str` with a signed `ptrdiff_t` length, and every length and count is a `ptrdiff_t`. See [Counted strings with signed lengths](#counted-strings-with-signed-lengths).

No input format gives a meaning to a `NUL` byte, so the input checks stop a `NUL` before it gets into the generated output. `os_read` rejects a file that contains a `NUL`, and reports this policy failure separately from system errors. `toml_datum_is_text` rejects a TOML string that contains a decoded `NUL`. `toml_require_known_keys` rejects a TOML key that contains a `NUL`, before it names the key. A path that contains a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read the path only up to that byte.

### Errors and diagnostics

- A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log`. It records every error that it finds in that log, and never truncates one. A message is one `printf` format that the compiler checks. See [A growable log, and every error in one run](#a-growable-log-and-every-error-in-one-run) and [Log messages use checked format strings](#log-messages-use-checked-format-strings).
- A filesystem action also takes the `operation` of the caller, such as `failed to read config`, and logs the whole message, because only the action knows which path failed. A failure inside a file names the file as `(in '<file>')` through `log_error_in`.
- An action, and a producer that can fail, return `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, for example a `struct Str` by value. A predicate returns `bool`. Running out of memory is never a return value. [STYLE.md](STYLE.md#function-signatures) gives the full rules.
- A first-party diagnostic is lowercase prose. It names the failed operation first, puts literal names and values in single quotes, and puts a value after the cause. Messages from the system or from a vendored library use their original capitalization and punctuation. [STYLE.md](STYLE.md#errors-and-diagnostics) gives the full rules.

### Paths and limits

- **Path safety.** Template names must be safe relative paths, and partial names use a more restrictive identifier grammar. Permalink expansion creates a public URL that starts with `/`, and the build removes that `/` to get the relative output path. Before the build joins an expansion to `output_dir`, `site_config_load` already checked that every expansion of the pattern is a safe relative path. These text checks prevent `.` and `..` traversal. Three checks cover symlink aliases that already exist, each at its own boundary: the identity checks of the input roots, the exclusion of `output_dir` from the walks, and the identity checks of the inputs.
- **Input size limits.** Each caller of `os_read` gives a limit for the kind of file that it reads. The read checks the limit before it allocates, so it never loads a file that is too large. `CONTENT_FILE_LEN_MAX` in `entry_renderer` equals `MARKDOWN_INPUT_LEN_MAX` and `ORG_INPUT_LEN_MAX` (256 MiB), which the body could not exceed anyway, and it also bounds the frontmatter. `CONFIG_FILE_LEN_MAX` in `site_config` is 1 MiB. `TEMPLATE_FILE_LEN_MAX` in `template` is 4 MiB for each template and each partial. `os_write` streams a static file from the file itself, so a static file or an asset has no size limit.
- **Path limits.** The build checks each generated output path against two limits. `OUTPUT_PATH_RELATIVE_LEN_MAX` limits the whole relative path, and `FILENAME_LEN_MAX` limits each path segment. `output_path_check_limits` reports a failure of either limit in the same way for every producer.

### Host access

No code in `src/` outside `runtime/fs.c` makes a system call on a path or prints through `stdio`. All host access goes through `os.h`, and the link step selects the host for each executable. A new unit test links the in-memory host, unless it needs a fact that only a real filesystem has. See [The `os` interface](#the-os-interface).

## Design decisions

Each decision below states what the code does and why, and some decisions also state what they cost. Read the decision before you change one of these designs, and make sure that the change deals with each of its reasons. The decisions come in groups, in this order: memory, strings, logs and their messages, host access, and where the shared code lives.

### Memory: arenas only

**Decision.** All first-party memory comes from arenas, and the code never frees an individual allocation. A scratch goes by value, so its memory is free again when the function returns. A perm goes by pointer.

**Reasons.**

- No comment needs to say which code frees a buffer.
- The bounds check is one comparison, and the comparison cannot overflow.
- A `Buf` can grow in place when it is the last allocation.
- An arena is one region, not a list of chunks, so a callee that allocates in a by-value copy leaks nothing.

### Memory: a perm and a scratch for each worker

**Decision.** Each pool worker has its own perm and its own scratch, and a job uses the arenas of the worker that runs it. A parse job builds its entry in a copy of the perm, and stores the copy back as the perm only when the entry is complete. The build's log has an arena of its own, and `job_run` splits the main thread's scratch into one share of job logs for each worker.

**Reasons.**

- A bump arena is not thread-safe, and separate arenas need no lock.
- A draft or a failed source leaves nothing in the arena.
- Arenas belong to threads, not to entries. Thousands of entries, each with a cap large enough for the largest entry, would need too much address space.

### Memory: reserve, then commit on demand

**Decision.**

1. When the build starts, it reserves its block with `PROT_NONE`, and it releases the block when it ends. The reservation costs no memory.
2. An arena commits more pages only when an allocation does not fit in the committed part, so the fast path is still one bounds check. The slow path commits pages to one mebibyte past what the allocation needs, but not past the end of the region.
3. Each arena has a fixed size, which comes from the limits that bound what it holds: the content, template and render limits, and the expansion limit. Every thread's perm is `BUILD_PERM_LEN` (4 GiB), and every thread's scratch is `BUILD_SCRATCH_LEN` (3 GiB). The log is `BUILD_LOG_LEN` (256 MiB). The main thread's scratch also holds one more `BUILD_LOG_LEN` share of job logs for each worker, and one for `job_run`. `cmd_build.c` defines each size with the bound that justifies it. The worker count is the only input that the reservation depends on.
4. The build runs the number of workers that `-w` requests, or by default one worker for each online CPU. It never runs more than `BUILD_WORKER_PER_CPU_MAX` (four) workers for each online CPU, whatever number `-w` requests. It reserves arenas for that number of workers before it reads any input. `pool_run` starts no more threads than a phase has jobs, so `-w` is a maximum. The verbose status lines give the worker count that the build has arenas for.
5. If the system refuses the block, the build halves its worker count, down to one worker. If the system still refuses, `arena_reserve_halving` halves the block, and every arena with it, up to `ARENA_RESERVE_HALVING_COUNT_MAX` (six) times. It then halves the first size that fits one more time, so that `malloc` in the vendored libraries and the thread stacks have room. If the system refuses even the smallest size, the build ends with the out-of-memory report.
6. A worker's arenas live longer than its jobs. Where a `malloc` design would free a buffer that the worker no longer needs, `arena_discard` gives back the buffer's pages instead:
   - a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more, after its body is converted
   - the bytes of a template, after the template is compiled
   - a page, after it is written

**Reasons.**

- Each arena's size comes from the limits, not from an estimate of what the inputs need. An estimate needs a budget that the build reads from `RLIMIT_AS`, estimates for each region, minimum and maximum sizes, and sized refusals. An estimate helps only under `ulimit -v`, and it can be wrong in both directions: a few hundred workers leave no room for one large render, and a deep or render-heavy site grows past its estimate after the reservation succeeded.
- A fixed reservation from the limits holds every input that the limits accept. A build without an address-space limit runs out of memory only when the system runs out of memory.
- An arena commits its pages on demand, not all at once, because strict overcommit charges every committed page against the system's limit, and Linux ignores `MAP_NORESERVE` under strict overcommit.
- On macOS, a reservation costs system time in proportion to its size: about 1 µs per GiB to reserve, and the same to release. With sizes from the limits, this cost is a fraction of a millisecond. A reservation as large as the address space allows would cost tens of milliseconds on every build.
- One reservation at startup, for the capped worker count, puts the whole layout in one function. A site with fewer sources than workers reserves arenas that some workers never use, and this costs only address space.
- A by-value copy cannot tell the original what it committed, so each job's copies commit their pages again, at a cost of about one system call per job. If the copies shared a committed end, they would skip the poisoning that `arena_commit` does under the address sanitizer. The arena would then hand out bytes that a dead copy left unpoisoned. Committing a worker's first step once, before its jobs, would save about 1% of system time, but it would add a new mechanism, so the code does not do it.
- A job does not give back the pages of a source under `JOB_SOURCE_DISCARD_LEN_MIN`, because the worker's next job reads into the same pages, and faulting them in again would cost more than the read. A template or a page costs far more to compile or render than to fault in. Without the discard, a large template's bytes and page would both stay resident in every worker. With a 4 MiB template, a worker holds 99 MiB when it gives back neither, and 71 MiB when it gives back both.

**Cost.** Under a `ulimit -v` that is below the full reservation, the build runs with halved arenas, which hold less than the limits allow. A large input then ends in the out-of-memory report, not in a sized refusal. Under such a limit, only correctness counts: the build succeeds or ends with that report, and it never writes wrong bytes.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, it calls `os_oom`. Each program defines `os_oom`, and the `sosig` version does these steps:

1. It prints the errors in the command's log and in the thread's job log.
2. It prints `sosig: out of memory` and the name of the file that the thread was processing, as `(in '<file>')`. A command names its log with `memory_oom_set_log`, and a job names its file and log with `memory_oom_set_job`.
3. It exits with `EXIT_CODE_FAILURE`.

**Reasons.**

- `sosig` is a CLI tool, and it is easy to make its output directory again.
- No call site checks for allocation failure.
- The report names the file that the thread was working on. A user needs that name to find the input that failed.

**Vendored code.** md4c, org4c, mustache4c, and tomlc17 still use `malloc`. md4c and org4c only parse, and their callbacks write into arenas. `sosig` replaces tomlc17's allocator with one that calls `os_oom` when `realloc` fails. When md4c, org4c, or mustache4c fails and reports no reason, the wrapper treats the failure as out of memory and calls `os_oom`. `sosig` also replaces tree-sitter's allocator, as [Highlighting: shared queries, memory per code block](#highlighting-shared-queries-memory-per-code-block) describes, so its failures also end in `os_oom`. The one exception is the Bash scanner's own `calloc` of its fixed-size state.

### Address sanitizer coverage in arenas

**Decision.** When `sosig` is compiled with the address sanitizer, an arena over a reserved region poisons the pages that it commits, and `arena_alloc` unpoisons only the bytes that it returns. Such an arena leaves a poisoned gap before each allocation, so two of its allocations are never adjacent, and code that extends the last allocation in place uses `arena_extend`. An arena over memory that the caller owns has no gap.

**Reasons.** ASan cannot see an overflow that stays inside one large mapping. Without the poisoning, the debug preset loses most of its overflow checks.

**Limits.**

- The arena does not poison scratch memory again when a function returns, so ASan does not find a use of scratch memory after the function that owned it returned.
- When a job's copy of its worker's arenas commits, it poisons the free space again, so a job that allocates past the usable part does not read what an earlier job allocated there.
- `arena_release` leaves the shadow memory to the sanitizer's `munmap` interceptor, which clears it, so a later mapping at the same addresses gets no old poison. Unpoisoning the whole region instead would write an eighth of each reservation as resident shadow memory.

### Highlighting: shared queries, memory per code block

**Decision.**

1. `highlight` compiles each grammar's highlight query at the grammar's first lookup, under a lock. The query lives until the process exits, and every worker reads it.
2. For each code block, the writer makes a new tree-sitter parser and query cursor, and deletes them and the tree before it returns.
3. While the writer highlights a code block, tree-sitter allocates from the scratch that the writer gets. A thread-local variable points the allocator at that scratch. The allocator puts a freed allocation on a free list for its exact size, and all of these allocations go away with the scratch. At other times, for example when `highlight` compiles a query, the allocator calls `malloc`, and calls `os_oom` when `malloc` fails.
4. If the parse of a code block takes more than `HIGHLIGHT_MEMORY_LEN_MAX` (128 MiB), or its tree has an error node with more than 4096 children, the writer writes the code block plain, as for a language with no grammar. If a query would take more memory, or more work than the length of its code block allows, the writer writes the rest of the code block plain.

**Reasons.**

- Query compilation takes 2 to 15 ms, so it occurs once. A new parser and cursor take less than a microsecond to make, so each code block gets its own, and no job or pool carries state for each worker.
- Tree-sitter recovers from tens of thousands of unclosed brackets with one error node that has that many children, and its query steps over the later siblings of a node at each of them. It also follows a match at every level of brackets nested 65,536 deep or more. Each of these cases takes seconds on a code block of a few hundred KB. A well-formed wide node, such as a long table of numbers, is as fast as other code, and real code takes at most three work reports per KiB, so the bounds catch only such code blocks.
- The queries are immutable after compilation, and a site that names no language compiles none. If the code freed them at exit, the free could race a worker that still highlights when `os_oom` ends the process. They are the one tree-sitter allocation that no call frees.
- With musl's `malloc`, a parse on eight threads is no faster than on one thread. Allocation from each worker's scratch makes it scale, and when the scratch runs out, the result is the arena's `os_oom`.
- If `free` did nothing, text that a grammar cannot parse could commit memory up to the limit before the writer writes the code block plain. The free lists reuse each allocation for the next allocation of the same size.
- The limit bounds the scratch that one code block can use. Each converter splits a region of twice the limit off its scratch for its code blocks, so that region, like every arena, gets its size from a limit, and `BUILD_SCRATCH_LEN` counts it, together with the code that a converter holds until a code block ends.
- A plain code block is an exception to the rule to fail immediately at a limit, because a build that fails over a code block's colors helps no one, and `formats/` has no log.

**Cost.** An Org code block that is almost all code references with a short `-l` label format is the one input that the limits accept and `BUILD_SCRATCH_LEN` does not hold, as the definition of `BUILD_SCRATCH_LEN` explains.

### Counted strings with signed lengths

**Decision.** Every first-party string is a `struct Str` from [str.h](vendor/sharedstuff/include/shared/str.h): a pointer and a `ptrdiff_t` length, for both owned and borrowed text. Every length and count is a `ptrdiff_t`. A value changes to `size_t` only at a libc, POSIX, or vendored call, or in a C buffer that goes to such a call. Each such change asserts that the value is not negative, unless this is true by construction. A C string exists only where libc, POSIX, or a vendored API needs one. The POSIX host copies each path into a `PATH_MAX` buffer at the system call, with `copy_cstr`, and `error_system_message` fills a caller's buffer through `strerror_r`.

**Reasons.**

- Subtraction cannot wrap to a very large value.
- A `NUL` byte is an ordinary byte, so no code calls `strlen` on owned text.

**Deliberate.** The `NUL` checks in [Strings and text input](#strings-and-text-input) are a product decision, and the string type does not force them. If the product decision changes, you can remove those checks and the tests that pin them.

### A growable log, and every error in one run

**Decision.**

- Each fallible function whose caller needs a message takes `struct Log* log` as its last parameter, and a callback type puts its `void* userdata` after the log. The log holds every error, with no truncation.
- After an error, the build continues while the next phase still has something to check. It stops before its first write if the log holds an error.
- The log records the errors of the phases in phase order, and the errors of one phase in walk order or job order, so the order does not change with the number of workers.
- A parallel phase prints at most `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct job errors, and a count of the others.
- A rejected config value gets its default, so one bad value causes one error.

**Reasons.**

- A user sees every error in one run.
- No message buffer has a fixed size, so no message is truncated, and no message must be early enough in the order to survive truncation.

### A log must not grow in a borrowed arena

**Decision.** A `struct Log` and its `Buf` live in an arena that no callee gets by value. The build gives each log an arena of its own, or an arena that all users of the log get by pointer.

**Reason.** A `Buf` grows in place. If a callee gets a copy of the same arena by value, the callee allocates the same bytes that the log grows into. The two then write over each other, with no crash and no ASan report.

[STYLE.md](STYLE.md#memory-and-resources) states the rule. Treat a break of the rule as a bug, not as a style problem.

### Log messages use checked format strings

**Decision.** A log call takes one `printf` format and its arguments. `log_error`, `log_error_in`, `log_error_v`, `buf_printf`, and `buf_vprintf` have `__attribute__((format(printf, ...)))`. A `struct Str` prints with `%.*s` and `STR_FMT`, and an integer prints with the conversion for its type. For example:

```c
log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path));
```

**Reasons.**

- Each message is one sentence in the source, so a reader reads its wording on one line, and `grep` finds it.
- The compiler checks the number and the types of the arguments against the format. `-Wformat=2` is on for first-party code and for `sharedstuff`.
- `%td` prints a `ptrdiff_t` with no cast. Only a string needs a helper, `STR_FMT`.
- Each first-party function that formats a message has the attribute, so its callers get the same check.

**Costs and limits.**

- `STR_FMT` evaluates its argument two times. Give it a variable or another lvalue, and put a computed string in a variable first.
- A string longer than `INT_MAX` bytes causes an assertion failure at run time, because the `%.*s` precision is an `int`.
- The attribute is the only guard on the code that reads the arguments. Each formatting function measures the text with `vsnprintf`, allocates the text, and writes it with a second `vsnprintf`. These functions are `buf_printf` and `buf_vprintf` in `sharedstuff`, which grow the buffer in place, and the first-party functions that format their own message. Only those functions change the measured `int` length to a `ptrdiff_t`.
- A first-party variadic function does not pass its `va_list` on to `log_error_v` or `buf_vprintf`. If it did, the Clang analyzer, version 22, would report a false `va_end` on an uninitialized list when it targets AArch64 Linux, where a `va_list` is a structure. Instead, the function starts its list once for each `vsnprintf` pass, as `buf_printf` does. `record_error` in `cli.c`, `build_verbose` in `cmd_build.c`, `render_fail` in `template.c`, and `test_log_is` in `tests/test_support.c` follow this rule.
- `%.*s` stops at a `NUL` byte. The input checks reject a `NUL` in file content, in TOML strings and keys, and in paths at the `os` boundary, so a message gets no `NUL` from input. The `os` boundary's `EINVAL` message quotes a path that contains a `NUL` only up to that byte, and the contract test for this case shows the result.
- An `off_t` has no conversion of its own. It prints with `%jd` and a cast to `intmax_t`.
- `clang-format` breaks a string literal that is longer than the column limit, so some long messages are in two literals.

### The `os` interface

**Decision.** All host access goes through `src/runtime/os.h`. `fs.c` is the POSIX implementation. [tests/os_memory.c](tests/os_memory.c) is an in-memory implementation for unit tests: an in-memory tree with symlinks, hard links, a modification-time counter, injected failures, and captured output. The link step selects one implementation for each executable. A function that accesses the host takes `struct Os*` just before its arenas, and an `os_*` function takes it first.

**Reasons.**

- Tests of the walk, the manifest, the pages, the writes, and the prune need no temporary directories.
- A link-time choice needs no table of function pointers, because no executable needs two hosts.

**Linking.** `sosig` and every unit test registered without `OS memory` link `runtime/fs.c`. The tests registered with `OS memory` link `tests/os_memory.c`, as [BUILD.md](BUILD.md#unit-tests) lists.

**Condition.** Each property that a test checks against the in-memory host must have a contract test. `src/runtime/test_os.c` compiles once against each host and runs the same cases, and both executables must pass, so every property that a test relies on in the in-memory host is also true on POSIX. Without the contract tests, the in-memory host can accept what POSIX rejects. For example, an in-memory host could accept `file/` as a file where POSIX returns `ENOTDIR`, and a contract test checks this case.

**Writes.** `os_write` writes either bytes or a file that it streams in 64 KiB pieces. `os_write`, `os_mkdirs`, `os_remove` and `os_remove_dir` are the only operations that change the host. A copy is an `os_write` from a file, and has no operation of its own. `os_write` writes every output in place, through a symlink or a hard link at the destination, and the write is neither atomic nor synced, so an interrupted build can leave a truncated file. Because the output directory is reproducible, deleting `output_dir` and building again is a cheaper recovery than a temporary-file protocol. `os_stat` reports the size and modification time that the build cache reads.

### Where the shared code lives

**Decision.** `sharedstuff` holds the generic parts: the arena with reserve, commit, and poisoning, `Buf`, `Str`, and `Log`. Each program defines `os_oom`, and has its own `os` interface and its own arena layout.

**Reason.** These parts do not depend on what a program does.
