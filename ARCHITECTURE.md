# Architecture

`sosig` is a small C17 static site generator. It reads project configuration, Markdown and Org content, and Mustache templates. It writes the generated files to a site-owned directory. This document describes the code structure. See [README.md](README.md) for usage and template behavior.

## Layout

- [src/app/](src/app/): Process entry point, command-line interface, and command boundaries.
- [src/build/](src/build/): Build orchestration, parallel phases, templates, manifest population, and output writing.
- [src/domain/](src/domain/): Site configuration, content metadata, frontmatter, permalinks, the build cache, and the output manifest.
- [src/formats/](src/formats/): Boundaries for HTML, Markdown, Org, and TOML data.
- [src/runtime/](src/runtime/): The host interface and its POSIX implementation, the directory walk, the out-of-memory report, and the thread pool.
- [src/core/](src/core/): Small reusable primitives, containers, diagnostics, text, and path logic.
- [tests/](tests/): Golden test suite, the shared unit-test support library ([tests/test_support.h](tests/test_support.h)), and the in-memory host ([tests/os_memory.h](tests/os_memory.h)). Each fixture site under [tests/fixtures/](tests/fixtures/) has a matching expected output directory under [tests/expected/](tests/expected/). [tests/fixtures/site_file_permalink/](tests/fixtures/site_file_permalink/) covers Markdown, Org, and templates with the default permalink. [tests/fixtures/site_index_permalink/](tests/fixtures/site_index_permalink/) covers a permalink that publishes a directory index for each entry. [tests/fixtures/site_nested_sections/](tests/fixtures/site_nested_sections/) covers nested and lopsided sections, source names with spaces, punctuation and non-ASCII characters, partials that include partials, and aggregate and feed outputs in subdirectories beside content outputs.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): Project entry point and supported build configurations.
- [cmake/](cmake/): Build profiles, quality tools, packaging, the golden test driver, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): Bundled dependencies (`sharedstuff`, `copt`, `tomlc17`, `md4c`, `org4c`, `mustache4c`, `acutest`).

First-party dependencies point inward:

- `app` can depend on all inner directories.
- `build` can depend on `domain`, `formats`, `runtime`, and `core`.
- `domain` can depend on `formats`, `runtime`, and `core`.
- `formats` and `runtime` can depend on `core`.
- `core` cannot depend on another first-party directory.

A module can skip layers. Modules in the same directory can depend on each other. First-party include directives start at the `src/` root. For example, a module includes `"core/ascii.h"`. This convention makes each dependency visible at the call site. Vendored support is outside the first-party hierarchy.

## Entry point and commands

`main` ([src/app/main.c](src/app/main.c)) only calls `cli_dispatch` ([src/app/cli_dispatch.h](src/app/cli_dispatch.h)) on the POSIX host. This separation lets unit tests link the code that maps an action to an exit code. `cli_dispatch` uses `cli_parse` ([src/app/cli.h](src/app/cli.h)) to parse the command line. `cli_parse` selects a subcommand and one of four actions: version, help, run, or error. For the run action, `cli_dispatch` calls one of these functions:

- `cmd_build_run` ([src/app/cmd_build.h](src/app/cmd_build.h)): generates the site.
- `cmd_config_run` ([src/app/cmd_config.h](src/app/cmd_config.h)): loads `sosig.toml` and prints the resolved config.

`cli.c` is the one translation unit that defines `COPT_IMPL`. `cli_parse` never prints or exits. `cli_dispatch` prints a rejected command line's diagnostic, `--help` and `--version` through `os_print`, and formats each in a reservation of its own, which it releases once the command or the print returns. A command reserves its own memory. Every command returns `enum ExitCode` ([src/app/exit_code.h](src/app/exit_code.h)): `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

## Build pipeline

The build has two layers:

- `cmd_build_run` is the outer boundary. It resolves the worker count, reserves the build's memory once, as one block of address space, and splits the block into arenas: the build's log, and a perm, for data that outlives a phase, and a scratch, for temporaries, for the main thread and for each worker. It calls `cmd_build_execute`, and releases the block. On failure, it prints the build's log to `stderr` exactly once, through `os_print`.
- `cmd_build_execute` owns the `BuildState`, runs the eleven phases below in sequence over the arenas it is given, and writes the verbose log. It runs the first phase itself, and `run_build_phases` runs the other ten. [entry_renderer](src/build/entry_renderer.h) parses entries and renders their Markdown or Org. [manifest_builder](src/build/manifest_builder.h) plans output paths. [page_renderer](src/build/page_renderer.h) renders and writes content pages. [site_writer](src/build/site_writer.h) writes the aggregate and feed outputs and copies the static files. [output_pruner](src/build/output_pruner.h) removes what the build did not write. Every change under `output_dir` goes through [output_writer](src/build/output_writer.h), which a dry run uses to withhold them.

No phase writes a failure diagnostic directly to `stderr`, apart from the out-of-memory report, which `os_oom` prints through `memory_oom_report` on the way out of the process. Every fallible function records its errors in a `struct Log`, a growable buffer with one line per error and an error count, which the command boundary prints once. Until the first write the build keeps going past an error wherever the next phase still has something to check, so one run reports every bad config key, every source that does not parse, and every output collision, and then stops before creating `output_dir` if any error was recorded. After the first write a failed phase is the last one that runs: the page phase still writes every other page and reports each one that fails, and `site_writer` stops at the first aggregate or feed template that fails. The prune runs only after every write succeeded, and reports every entry it cannot list or remove. A parallel phase gives each job a log of its own and appends the failed jobs' logs in job order, at most twenty distinct ones, plus a count of the remainder. A page render ends its failure with `(while rendering '<entry>')`, and the comparison stops there, so one template failure that every page repeats prints once.

Verbose status is separate from diagnostics, and both go through `os_print`. The main thread writes phase status lines between phases. Each worker formats its `\r<label> n/total` progress line whole, such as `parsing content 3/12`, and prints it in one `os_print` call under a lock that `job.c` keeps for it, so two workers' lines never interleave.

The phase order follows the data dependencies. A content template can read `site.updated` and iterate through `content_entries`. These values are not available until the tool parses and sorts all entries. The build uses one parallel phase to parse entries and another to render pages. The collect-and-sort phase runs between the two parallel phases.

1. **Load inputs** (`load_build_inputs`): The tool uses `site_config_load` to read and validate `sosig.toml`. It requires each configured read root to be a usable directory, reporting the cause when one is not, and skipping the optional static root when a site ships none. `manifest_builder_check_output_dir` then rejects an `output_dir` at or below `content_dir`, `templates_dir`, or `static_dir`, and one that holds any of them or the config file, before any input tree is walked. Then `walk_files_with_suffixes(paths, content_dir, output_dir, suffixes, ...)` finds and sorts the `.md` and `.org` content sources in one walk and the static files in another. The block was reserved at startup, with arenas for at most four workers per online CPU, however many `-w` asks for, and a parallel phase starts no more workers than it has jobs, so `-w` is a maximum. Every walk of an input tree leaves `output_dir` out by identity, so a symlink into the output tree cannot feed an earlier build's outputs back in as inputs.
2. **Parse and render entries** (`render_content_entries` -> `entry_renderer_render_entries`): The tool allocates `source_entries`, one slot per source file, and `job_run` dispatches one job for each source file. Each job stats its file, for the build cache, and reads it, up to `CONTENT_FILE_LEN_MAX`, so a source that grew past the limit since the walk fails its own job. For a Markdown source, it separates the frontmatter from the body and parses the frontmatter. For an Org source, it reads the keywords at the top of the file, and the body is the whole file. For a non-draft, the job converts the body to HTML with the converter of its format. It also creates the URL and output path. The job builds the entry in a copy of its worker's perm and commits the copy only when it fills the slot, so a draft, which stops before these steps, and a failed source leave their slot `NULL` and nothing in the arena. Each job writes its entry to its own `source_entries` slot and its errors to its own log, in its worker's log share, which prevents shared mutable render state. After all workers finish, `pool_run` joins them. Then `job_run` appends each distinct job's log to the build's log, in job order, up to twenty, followed by a count of the remaining failures. The optional progress line is the only worker output to `stderr`, apart from an out-of-memory report.
3. **Collect** (`collect_content_entries`): The tool copies non-draft entries into one compact array. `content_entry_sort` sorts them from newest to oldest. `content_entry_date_latest` calculates the last-updated timestamp once. Each later render reads the sorted array and this timestamp.
4. **Manifest** (`populate_output_manifest` -> `manifest_builder_populate`): The tool records all output paths for content, aggregates, feeds and static files, and the build cache's path last. It does this before it renders a page. It rejects four types of conflict:
   1. Two producers can claim the same path. `manifest_add` rejects the second claim. Paths that differ only in ASCII case are the same path, because a case-insensitive filesystem such as the macOS default stores them as one file. The comparison folds only `A-Z` to `a-z`. Other bytes must match exactly.
   2. One output can use `a/b` while another uses `a/b/c`. The first path must be a file, but the second path requires a directory at `a/b`. `manifest_find_prefix_collision` detects this conflict after the tool records all paths. It folds ASCII case in the same way, so `A/B` and `a/b/c` also conflict.
   3. An output can name an existing build input. `manifest_builder_populate` compares the device and inode of each file. It checks the config, all discovered sources, the static files, and every file below `templates_dir`. The sources include drafts. The template tree includes partials, per-entry overrides, and templates that no configuration names. A missing `templates_dir` adds no inputs, although load inputs has already rejected one in a full build. A `templates_dir` that exists but cannot be walked fails the build.
   4. An output can land at or below `content_dir`, `templates_dir`, or `static_dir`, even where no file exists yet. The next build would read a new content file below `content_dir` as a source and would copy a new file below `static_dir` as a static file. The same build could read a new file below `templates_dir` as a partial. `manifest_builder_check_output_dir` has already compared the device and inode of each root with the nearest existing ancestor of `output_dir` and each of that directory's ancestors, during load inputs. That rejects an `output_dir` inside a root before any walk, whether or not `output_dir` exists yet. `manifest_builder_populate` then compares each root with each existing directory between `output_dir` and the output. That catches a symlink inside `output_dir` that leads into a root. These comparisons detect alternate spellings and symlinked roots. A sibling directory such as `contentx` does not match. A missing root is skipped. The tool runs this check before the input-file check, so an output below a root reports this conflict.
5. **Load the build cache** (`load_build_cache`): The tool reads `_sosig/v<n>-build-cache.tsv` below `output_dir`, unless `--force` asks it to ignore the file. Its first line is the page recipe: the config fields a render reads, the generator string and `PAGE_RECIPE_VERSION`. Each row holds the size and nanosecond modification time of one input the pages depend on: each entry's source, named by its page's output path, with whether that page read the entry list, and each file below `templates_dir`. A missing file, one that cannot be read, and one that does not parse or whose first line is another page recipe are each an empty build cache and a verbose note, never an error. The tool then stats every template and decides which outputs the build cache vouches for. Every one needs every template row to match, with no template added or removed, because a page's partials are tracked as a whole. A content page also needs its own row to match its source. A page that read the entry list, `content_entries` or `site.updated`, and every aggregate and feed, also need every entry's row to match, with no entry added or removed. Drafts have no rows, so a draft published, or a page turned into a draft, counts as an added or removed entry.
6. **Prepare output** (`prepare_output_dir`, then `empty_build_cache`): The tool creates `output_dir` through `output_writer_mkdirs`. This is the build's first write, so a build that an earlier phase refuses creates nothing, not even an empty `output_dir`. Later writes create their own parent directories. From here on every change under `output_dir` goes through the build's `OutputWriter`, which marks each output path written or kept in the manifest. A dry run's writer withholds every change: it creates no `output_dir`, writes no file, and records what the prune would remove instead of removing it. The tool then writes the build cache as zero bytes, so a build that fails or is interrupted from here on leaves a build cache the next build does not use, and no later build keeps a page this one may have cut short or written under other inputs.
7. **Render and write pages** (`render_content_pages` -> `page_renderer_render_pages`): A second `job_run` phase renders each content template, one job per `source_entries` slot, and skips a `NULL` slot. A job whose page the build cache vouches for, and whose page exists, keeps it through the output writer instead, with no render, and its new row carries the old row's entry-list flag forward. Every other job records whether its render read the entry list. The render context contains the sorted entries and `site.updated`. Each job renders its page into a copy of its worker's perm, past the entries stored there, and writes it to the entry's output path through the output writer, which marks that path in the manifest. The copy is dropped when the job returns, so each worker holds at most one page in memory. Jobs write concurrently. The manifest guarantees that no two jobs write the same file. Jobs can create the same parent directory at the same time, and `os_write` accepts a directory that already exists. A failed write is that entry's diagnostic, like a failed render. The other pages are still written.
8. **Write aggregates and feeds, and copy static files** (`write_generated_site` -> `site_writer`): The tool renders and writes each aggregate and feed template, or keeps one that exists when the build cache vouches for every entry and template. These run after every page, on the main thread, because each reads the whole sorted entry set. Then it copies the discovered static files into `output_dir`, each at its path below `static_dir`. Writes truncate their destination in place, so this phase and the page phase are not transactional at any level: a page failure or a late template failure can leave new, old, and stale files together, and an interrupted write can leave one output truncated. The recovery for both is to delete the output directory and build again.
9. **Write the build cache** (`write_build_cache`): The tool writes the build cache through the output writer, with the page recipe and a row for every current entry and template, once every output is written or kept. A dry run withholds this write and the emptying, as every write.
10. **Remove stale outputs** (`prune_stale_outputs` -> `output_pruner_prune`): Once every output is written, the tool removes every entry below `output_dir` that is not an output and not a directory one is below, so the tree holds only what the current inputs produce. It compares device and inode values rather than names, so a case-insensitive filesystem's old spelling of a current output is kept, and the identities are sorted once so each lookup is a bisection. It removes a symlink as a link and never descends into one, so nothing outside `output_dir` is touched, and it never enters a directory that is an input root. A stale directory is removed once everything in it is. A failure to list or remove an entry is reported and the walk goes on, and a directory that a failure left non-empty is not reported again. A failed build never reaches this phase, so it removes nothing. A dry run's prune finds no `output_dir` when no earlier build made one, and then has nothing to remove.
11. **Report the dry run** (`report_dry_run` -> `output_writer_report`): A dry run only. The tool prints to `stdout` a `write` or `keep` line for each output path, in manifest order, then a `remove` line for each path the prune would have removed, sorted bytewise, in one `os_print` call. An output path still unmarked means a site wrote without the output writer, so the report fails instead of printing a listing that leaves the write out.

## Modules

### Domain (`src/domain`)

- [site_config](src/domain/site_config.h): Defines `SiteConfig` and its load and print operations. The arena the caller passes holds the copied values that the load operation validates and normalizes. The load operation rejects a key outside the schema and applies optional defaults. Later phases use the resolved config without changing it. `base_url` needs an `http://` or `https://` scheme and a non-empty host. The scheme comparison is case-insensitive. The module removes trailing `/` characters from `base_url` and each configured directory. It also validates template names and permalink expansion.
- [content_entry](src/domain/content_entry.h): Defines the `ContentEntry` metadata struct. Its fields include source data, template data, and generated paths. The entry and its strings, `body_html` included, live in the perm of the worker that parsed it. `content_entry_sort` sorts entries from newest to oldest and breaks date ties by output path. `content_entry_date_latest` returns the newest date for `site.updated`. It returns the Unix epoch for an empty site.
- [frontmatter](src/domain/frontmatter.h): `frontmatter_split` splits a Markdown file at its `+++` fences, and `frontmatter_parse` parses the TOML frontmatter into a content entry. The parse rejects a key outside the schema. `frontmatter_parse_org` reads the keywords at the top of an Org file into a content entry, with the same keys, required fields and template check, and ignores every other keyword.
- [permalink](src/domain/permalink.h): `permalink_expand` creates an entry URL and replaces the `{slug}` and `{section}` tokens. It removes redundant `/` characters and appends `index.html` when the expanded path ends with `/`. The last expanded byte controls this decision, not the last pattern byte. The URL without its leading `/` is the relative output path, so the public URL names the output file. `site_config_load` expands sample values to check that every expansion is a safe relative path and that distinct entries get distinct paths, so a bad pattern is one config error. Each entry's output path is checked against both output-path limits when the entry is parsed, because only a real entry's slug and section give its length.
- [build_cache](src/domain/build_cache.h): Parses and formats the build cache: the page recipe as its first line, then one tab-separated row per input, kind, name, size, modification time and, for an entry, its page's entry-list flag, sorted by kind and then by name. A name that holds a tab or a newline gets no row, so no name needs escaping. `BUILD_CACHE_VERSION` names the row format in the file's name, and `PAGE_RECIPE_VERSION` is the one constant to bump when a code or vendored-renderer change must make current-looking outputs again.
- [manifest](src/domain/manifest.h): An insertion-ordered map from `output_path` to `source_label`. The tool creates it before it renders page templates or writes output files. A 4-ary hash trie keyed by the ASCII-folded path lets `manifest_add` reject a duplicate: each level takes the next two bits of the path's 64-bit FNV-1a hash, so a lookup descends O(log n) levels in expectation, and the trie never moves an entry it has placed. Two paths that differ only in ASCII case are duplicates. Other bytes, including non-ASCII bytes, compare exactly. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path. For example, it finds the conflict between `a/b` and `a/b/c`, and between `A/b` and `a/b/c`. Each entry also holds a mark, pending until the output writer marks the path written or kept. Marking changes no trie link, so page jobs mark their own paths at once.

### Build (`src/build`)

- [template](src/build/template.h): `template_render_file` uses mustache4c to render a template with a `TemplateContext`. Template names must be safe relative paths, and partial names use a restricted identifier grammar. Each render has three limits. The partial-expansion limit stops a partial that includes itself, so every render terminates. The output-byte limit bounds the output buffer. The distinct-partial limit bounds the compiled-partial cache. The limits do not bound the total memory of a render. Templates are trusted input, and a template's nested sections use memory in proportion to the iterations that they request. A render also reports whether it resolved `content_entries` or `site.updated`, the two names whose values depend on every entry, which the build cache records for each page.
- [entry_renderer](src/build/entry_renderer.h): The first parallel phase. `entry_renderer_render_entries` parses each source into its slot of a caller-owned entry array, one slot per source path. For a non-draft entry, it converts the Markdown or Org body and creates the URL and output path. A draft leaves the slot `NULL`. The module owns this per-job pipeline.
- [job](src/build/job.h): Defines `job_run`, which connects the two parallel phases to the worker pool. It gives each job a log of its own, in the share of the phase's scratch that its worker owns, reports synchronized progress, and merges the failed jobs' logs in job order, at most twenty distinct ones plus a count of the remainder. The caller can name the text that starts each job's own name at the end of a line. Two jobs whose lines differ only after that text are one failure. Each job writes its results into its own caller-owned slot.
- [output_path](src/build/output_path.h): Reports a generated output path that exceeds either output-path limit, with one wording for every producer. `entry_renderer` and `manifest_builder` share it.
- [manifest_builder](src/build/manifest_builder.h): Builds the [manifest](src/domain/manifest.h) before the tool writes a generated file. `manifest_builder_populate` records every content, aggregate, feed, and static file output. It rejects duplicate paths, nested paths below an output file, outputs at or below `content_dir`, `templates_dir`, or `static_dir`, and outputs that overwrite build inputs. `manifest_builder_check_output_dir` rejects an `output_dir` at or below any of the three roots before the build walks an input tree, whether or not `output_dir` exists yet, and an existing one that holds a root or the config file. `manifest_builder_populate` then checks the directories below `output_dir`. The duplicate and nesting checks fold ASCII case. Build inputs include the config, sources, static files, and every file below `templates_dir`, which includes partials. The root and input checks compare device and inode values. They detect alternate spellings, hard links, symlinks, and case-insensitive paths to the same file or directory. The manifest and `site_writer` both join each aggregate and feed template name, and each static file's path below `static_dir`, to `output_dir` with `path_join`, so the path the manifest checked is the path that is written.
- [output_pruner](src/build/output_pruner.h): Removes every entry below `output_dir` that the manifest does not name and that no output is below, comparing identities rather than names. It removes symlinks as links, never enters an input root, and removes a stale directory once it is empty.
- [output_writer](src/build/output_writer.h): Makes every change one build makes under `output_dir`: the writes, the creation of `output_dir`, and the prune's removals. Each write or keep marks its path in the manifest and fails on a path the manifest does not name, because the same build's prune would remove it. A dry run's writer withholds every change and records the removals, and `output_writer_report` lists its marks and removals. The page renderer, the site writer, and the prune take the writer for their changes and keep `struct Os` for their reads.
- [page_renderer](src/build/page_renderer.h): The second parallel phase. `page_renderer_render_pages` renders each parsed entry through its content template and writes the page, or keeps a page the build cache vouches for. Each render reads the complete sorted entry set and `site.updated` through a deeply `const` template context.
- [site_writer](src/build/site_writer.h): Renders and writes the aggregate and feed templates after every content page is written, or keeps those the build cache vouches for, then copies each static file with one streamed `os_write`, and stops at the first template or file that fails. `cmd_build` gives a feed only the newest `feed_count` entries.

### Formats (`src/formats`)

- [markdown](src/formats/markdown.h): `markdown_to_html` wraps md4c with the project's extension set.
- [org](src/formats/org.h): `org_to_html` wraps org4c with the project's flags.
- [html](src/formats/html.h): HTML escaping of untrusted text for generated markup.
- [toml](src/formats/toml.h): TOML text validation plus date/datetime conversion and formatting. `toml_require_known_keys` is the shared unknown-key check: it reports each table key that a loader's list of known keys does not contain.

### Runtime (`src/runtime`)

- [memory](src/runtime/memory.h): Keeps what the out-of-memory report prints: the command's log, and for each thread the file it works on and its job's log, which `memory_oom_set_log` and `memory_oom_set_job` name.
- [pool](src/runtime/pool.h): Runs indexed jobs through a bounded pthread pool. It completes all jobs, even if one job fails. It joins all workers before it returns. Each job learns the index of the worker running it, so it can use that worker's arenas. It also gets the default worker count from the number of online CPUs.
- [os](src/runtime/os.h): The host interface. Every read, write, directory creation, removal, listing, `stat` and print the program performs goes through `os_read`, `os_write`, `os_mkdirs`, `os_remove`, `os_remove_dir`, `os_list`, `os_stat` and `os_print`, and each function that reaches the host takes a `struct Os*`. `os_read` takes a size limit from each caller and checks it before any read or allocation, rejects a file that is not regular, one holding a `NUL`, and one that changes size during the read. `os_write` takes one of two sources, bytes or a file it streams, and creates parent directories. It overwrites its destination in place, through a symlink or a hard link, neither atomically nor synced: a reproducible output directory makes deleting it and rebuilding a cheaper recovery than a temporary-file protocol. A new file gets mode `0666` reduced by the process umask. `os_list` closes a directory before it returns, classifies each entry after following a symlink, and skips dangling links and entries removed since it was read. `os_print` carries every byte the program prints: the log a command ends with, a rejected command line, `--help` and `--version`, the resolved config, the verbose status lines and the workers' progress line, so a test on the in-memory host sees all of it. One call writes its text whole, and a caller that prints from several threads keeps its own lock around it. `os_oom` is the remaining member, declared by `sharedstuff` and defined by each program.
- [fs](src/runtime/fs.h): The POSIX implementation of `os.h`, an object library of its own (`sosig_os_posix`). The `sosig` executable and every unit test registered without `OS memory` link it. Its `struct Os` has no state.
- [walk](src/runtime/walk.h): Finds and sorts matching regular files in a directory tree, through `os_list` and `os_stat` only, so it behaves the same on every host. It follows symlinked directories but walks each directory once, by device and inode. A cycle or a second alias of a directory is skipped, so an aliased directory publishes one copy of its entries. A caller can also name one directory to leave out, which the walk records as already visited, so every path to it is skipped. The build leaves out `output_dir`. The walk visits entries in byte order and each symlinked directory only after every real one, so the listed path is the one without a symlink when one exists. Only one directory is open at a time, and a level of the recursion keeps no path buffer in its frame.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [error](src/core/error.h): Turns an `errno` into text, thread-safely. Diagnostics themselves go to the vendored `shared/log.h`, as `printf` formats that the compiler checks.
- [parse](src/core/parse.h): Parsing of text into scalar values.
- [path](src/core/path.h): Path construction and validation. `path_is_below` finds the part of a path below a root by textual comparison. `entry_renderer` derives each entry's `{section}` from the source path below `content_dir`. `manifest_builder` plans each static file output from it, and `site_writer` copies the static file to the same path below `output_dir`.
- [path_list](src/core/path_list.h): Growable list of path strings.
- [sosig_version](src/core/sosig_version.h): The tool name and version fixed when the build tree was configured.
- [text](src/core/text.h): Text normalization and validation.

## Cross-cutting conventions

- **Memory**: Every first-party allocation comes from an arena, a `{beg, end, limit}` triple from `sharedstuff` that frees nothing piecemeal. Each thread has two: its perm, for data that outlives the function that makes it, which a function takes as `struct Arena* perm`, and its scratch, for temporaries, which a function takes by value, so they are free again when it returns. `markdown_to_html`, `org_to_html`, `template_render_file`, and `os_read` all return their result in the caller's `perm`. The build's log has an arena of its own. A command reserves its memory once, when it starts, as one block, through `arena_reserve_halving`, and splits the block into its arenas, each of a fixed size that the input and render limits bound, as its definition in `cmd_build.c` states. When an address-space limit refuses the block, the build halves its worker count, down to one, before `arena_reserve_halving` halves every arena. A reservation costs address space only: an arena commits pages as it fills, so only the pages a build uses count against the system's memory. Where a `malloc` design would free a buffer that a worker is done with while the worker's arena lives on, `arena_discard` gives its pages back when the work that filled them costs more than faulting them in again. A source under `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) costs less to read again into the same pages, so it keeps them. An allocation that does not fit calls `os_oom`. In `sosig` it prints the command's log, the log of the calling thread's job, and `sosig: out of memory (in '<file>')`, naming the file the thread was working on, then exits with `EXIT_CODE_FAILURE`, so no call site checks for allocation failure. The vendored parsers keep their own `malloc`: tomlc17's allocator is replaced by one that calls `os_oom`, and an md4c, org4c, or mustache4c failure that reports no reason is treated as running out of memory at the wrapper. Under the address sanitizer two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`.
- **Text representation**: Every first-party string is a counted `struct Str` (`vendor/sharedstuff/include/shared/str.h`) with a signed `ptrdiff_t` length, owned or borrowed alike, and a `NUL` byte is an ordinary byte to it. A C string exists only where text reaches libc, POSIX or a vendored API that takes one: the POSIX host copies each path into a `PATH_MAX` buffer at the system call (`copy_cstr`), and `error_system_message` fills a caller's buffer through `strerror_r`. Lengths and counts are `ptrdiff_t`, and convert to `size_t` only at those calls.
- **Text input checks**: `os_read` rejects a file that contains a `NUL`. `toml_datum_is_text` rejects a TOML string that contains a decoded `NUL`, and `toml_require_known_keys` rejects a TOML key that contains one before it names the key. No input format gives a `NUL` a meaning, so these keep it out of the generated output. `os_read` reports this policy failure separately from system errors. A path holding a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read it only up to that byte.
- **Return values**: An action, and a producer that can fail, returns `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, such as a `struct Str` by value. A predicate returns `bool`. An allocation that does not fit is never a return value.
- **Diagnostic logs**: A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log` and records every error it finds there, never truncated. A callback type puts its `void* userdata` after the log. A message is one `printf` format, and the `format` attribute makes the compiler check its arguments. A `struct Str` prints through `%.*s` and `STR_FMT`. Filesystem actions also take the caller's `operation`, such as `failed to read config`, and log the whole message, because only they know which path failed. A failure found inside a file names it as `(in '<file>')` through `log_error_in`. A log's arena is never one that a callee is given by value, so the log never grows into bytes a callee is using.
- **Diagnostic text**: First-party diagnostics use lowercase prose and name the failed operation first. They use single quotes for literal names and values. External messages keep their original capitalization and punctuation. A value follows the cause.
- **Path safety**: Template names must be safe relative paths. Partial names use a more restrictive identifier grammar. Permalink expansion creates a public URL that starts with `/`. The tool removes this `/` to get the relative output path. `site_config_load` has already checked that every expansion of the pattern is a safe relative path, before the tool joins one to `output_dir`. These text checks prevent `.` and `..` traversal. Input-root identity checks, the walks' exclusion of `output_dir`, and input-identity checks cover existing symlink aliases at their separate boundaries.
- **Input size limits**: Each `os_read` caller passes a limit for the kind of file it reads. The read checks the limit before it allocates, so an oversize file is never loaded. `CONTENT_FILE_LEN_MAX` in `entry_renderer` equals `MARKDOWN_INPUT_LEN_MAX` and `ORG_INPUT_LEN_MAX` (256 MiB), which the body could not exceed anyway, and it also bounds the frontmatter. `CONFIG_FILE_LEN_MAX` in `site_config` is 1 MiB. `TEMPLATE_FILE_LEN_MAX` in `template` is 4 MiB for each template and partial. `os_write` streams a static file from its file source, so it has no size limit.
- **Path limits**: The tool checks each generated output path against two limits. `OUTPUT_PATH_RELATIVE_LEN_MAX` limits the complete relative path. `FILENAME_LEN_MAX` limits each path segment. `output_path_check_limits` reports either failure the same way for every producer.
- **Host access**: Nothing in `src/` outside `runtime/fs.c` makes a system call on a path or prints through `stdio`. Each executable chooses its host when it is linked. `sosig` links `runtime/fs.c`, and so does every unit test registered without `OS memory`: the POSIX layer's own tests (`fs`, `os_posix`) and the test-support test, which need a real filesystem, the suites not yet converted to the in-memory host (`cli`, `cli_dispatch`, `cmd_config`, `entry_renderer`, `site_config`, `template`), and the tests that reach no host. The tests registered with `OS memory` link [tests/os_memory.c](tests/os_memory.c), an in-memory tree with symlinks, hard links, injected failures and captured output. A new test links it too, unless it needs a fact only a real filesystem has. A link-time choice needs no table of function pointers, because no executable needs two hosts. `src/runtime/test_os.c` builds once against each host and runs the same cases, so every property a test relies on in the in-memory host also holds on POSIX. The workers' progress line is one `os_print` call under a lock of its own, which keeps it whole across workers.
- **Parallel ownership**: No arena or log is shared between two jobs running at once. Each job uses the arenas of the worker running it, and each worker has its own. A parse job allocates its entry in its worker's perm. A worker receives immutable shared context, apart from the manifest mark of the page it writes, and owns one index in each mutable result array. `pool_run` joins all workers before the main thread reads those slots, and the entries stay in the worker arenas until the build ends.

## Design decisions

This section records each design decision and the reason for it. Read it before you change one of these designs. A change must answer the reason that this section gives.

These terms have one meaning in this section:

- **Block**: the one reservation a command makes when it starts. The command splits it into its arenas and releases it at its end.
- **Region**: a range of address space that one arena uses. Two arenas never use the same region at the same time.
- **Reserve**: to get address space from the system without memory behind it.
- **Commit**: to make reserved pages usable. The system charges committed pages against its memory limit.
- **Scratch**: a thread's arena for temporary data. A function gets it by value, as `scratch`.
- **Perm**: a thread's arena for data that must stay after the function returns. A function gets it by pointer, as `perm`.
- **Log**: a `struct Log`, the error lines a command prints once. It grows in an arena of its own, which no callee gets by value.

### Memory: arenas only

**Decision.** All first-party memory comes from arenas. An arena is two pointers, `beg` and `end`, into one region, and a third, `limit`, when the region is reserved and commits pages on demand. The code never frees one allocation. A scratch goes by value, so its memory is free again when the function returns. A perm goes by pointer.

**Reasons.**

- No comment must say who frees a buffer.
- The bounds check is one comparison that cannot overflow.
- A `Buf` can grow in place when it is the last allocation.
- An arena is one region, not a list of chunks, so a callee that allocates in a by-value copy leaks nothing.

### Memory: a perm and a scratch for each worker

**Decision.** Each pool worker has its own perm and its own scratch. A job uses the arenas of the worker that runs it. A parse job builds its entry in a copy of the perm. The job keeps the copy only when the entry is complete. The build log has an arena of its own, and `job_run` splits the main thread's scratch into one share of job logs per worker.

**Reasons.**

- A bump arena is not thread-safe. Separate arenas need no lock.
- A draft or a failed source leaves nothing in the arena.
- Arenas belong to threads, not to entries, because thousands of entries, each with a cap large enough for the largest, need too much address space.

### Memory: reserve, then commit on demand

**Decision.**

1. The build reserves its block with `PROT_NONE` when it starts, and releases it at its end. The reservation costs no memory.
2. An arena commits more pages only when an allocation does not fit in the committed part. The fast path stays one bounds check. The slow path commits pages a mebibyte past what the allocation needs, up to the end of the region.
3. Each arena has a fixed size, derived from the input and render limits that bound what it holds: the content, template and render limits, and the expansion limit. Every thread's perm is `BUILD_PERM_LEN` (4 GiB) and every thread's scratch `BUILD_SCRATCH_LEN` (3 GiB). The log is `BUILD_LOG_LEN` (256 MiB), and the main thread's scratch holds one more `BUILD_LOG_LEN` share of job logs for each worker and one for `job_run`. The sizes are in `cmd_build.c`, each with the bound that justifies it. The only input the reservation depends on is the worker count.
4. The build runs the requested number of workers, or one per online CPU by default, and at most `BUILD_WORKER_PER_CPU_MAX` (four) per online CPU, and reserves arenas for that many before it reads any input. `pool_run` starts no more threads than a phase has jobs. The verbose status lines name the count the build has arenas for.
5. If the system refuses the block, `arena_reserve_halving` halves it, and every arena with it, up to six times, and halves the first size that fits once more, so `malloc` in the vendored libraries and thread stacks keep room. If even the smallest size is refused, the build ends with the out-of-memory report.
6. A worker's arenas outlive its jobs, so `arena_discard` gives back the pages of a buffer a worker is done with: a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more once its body is converted, a template's bytes once it is compiled, and a page once it is written.

**Reasons.**

- Each arena's size comes from the limits rather than from an estimate of what the inputs need. An estimate takes a budget read from `RLIMIT_AS`, per-region estimates, least and most sizes and sized refusals, helps only under `ulimit -v`, and can be wrong in both directions: a few hundred workers leave no room for one large render, and a deep or render-heavy site outgrows its estimate after the reservation has succeeded.
- A fixed reservation from the limits holds every input the limits accept. A build without an address-space limit runs out of memory only when the system does.
- An arena commits its pages on demand, not all at once, because strict overcommit charges every committed page against the system's limit, and Linux ignores `MAP_NORESERVE` under it.
- Reserving costs system time in proportion to the size, about 1 µs per GiB to reserve and as much to release, on macOS. Sizes from the limits keep that to a fraction of a millisecond, where a reservation as large as the address space allows would cost tens of milliseconds on every build.
- One reservation at startup, for the capped worker count, puts the whole layout in one function. A site with fewer sources than workers reserves arenas that some workers never use, which costs address space only.
- A by-value copy cannot tell the original what it committed, so each job's copies commit their pages again, about one system call per job. Sharing a committed end between copies would skip the poisoning that `arena_commit` does under the address sanitizer, so bytes a dead copy unpoisoned would be handed out unpoisoned. Committing a worker's first step once, before its jobs, would save about 1% of system time for a new mechanism, so it is not done.
- A smaller source is not given back, because the worker's next job reads into the same pages, and faulting them in again would cost more than reading it. A template or a page costs far more to compile or render than to fault in, and a large template's bytes and page would otherwise both stay resident in every worker: on a 4 MiB template, a worker holds 99 MiB with neither given back and 71 MiB with both.

**Cost.** Under a `ulimit -v` below the full reservation, the build runs with halved arenas, which hold less than the limits allow, and a large input then ends in the out-of-memory report rather than a sized refusal. Under such a limit only correctness counts: the build succeeds or ends with that report, and never writes wrong bytes.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, it calls `os_oom`. Each program defines `os_oom`. The `sosig` version does these steps:

1. It prints the errors that the command's log and the thread's job log have.
2. It prints `sosig: out of memory` and the name of the file that the thread was processing. A command names its log with `memory_oom_set_log`, and a job names its file and log with `memory_oom_set_job`.
3. It exits with `EXIT_CODE_FAILURE`.

**Reasons.**

- `sosig` is a CLI tool. Its output directory is easy to make again.
- No call site checks for allocation failure.
- The report names the file the thread was working on, which a user needs to find the input that failed.

**Vendored code.** md4c, org4c, mustache4c, and tomlc17 keep `malloc`. Each wrapper changes their allocation failure into a call to `os_oom`.

### Address sanitizer coverage in arenas

**Decision.** In a build with the address sanitizer, an arena over a reserved region poisons the pages it commits. `arena_alloc` unpoisons only the bytes that it returns. In such an arena it leaves a poisoned gap before each allocation, so two of its allocations are never adjacent. An arena over memory the caller owns has no gap.

**Reasons.** ASan cannot see an overflow that stays inside one large mapping. Without this step, the debug preset loses most of its overflow checks.

**Limit.** The arena does not poison scratch memory again when a function returns. ASan does not find a use of scratch memory after the function that owned it returns. A job's copy of its worker's arenas poisons the free space again when it commits, so a job that allocates past the usable part does not read what an earlier job allocated there. `arena_release` leaves the shadow to the sanitizer's `munmap` interceptor, which clears it, so a later mapping at the same addresses inherits no poison.

### A log must not grow in a borrowed arena

**Decision.** A `struct Log` and its `Buf` live in an arena that no callee gets by value. The build gives each log an arena of its own, or an arena that everything using it reaches by pointer.

**Reason.** A `Buf` grows in place. Assume that a callee gets a copy of the same arena by value. The callee then allocates the same bytes that the log grows into. The two overwrite each other with no crash and no ASan report.

**How to apply.** [STYLE.md](STYLE.md#memory-and-resources) states the rule. Treat a break of it as a bug, not as a style problem.

### Counted strings with signed lengths

**Decision.** Every first-party string is a `struct Str`, a pointer and a `ptrdiff_t` length. Every length and count is `ptrdiff_t`. A value changes to `size_t` only at a libc, POSIX, or vendored call, or in a C buffer that goes to one, such as the `PATH_MAX` buffer of `copy_cstr`. Each such change asserts that the value is not negative, unless that holds by construction. A C string exists only where libc, POSIX, or a vendored API needs one.

**Reasons.**

- Subtraction cannot wrap to a very large value.
- A `NUL` byte is an ordinary byte, so no code calls `strlen` on owned text.

**Kept on purpose.** `os_read` rejects a file that contains a `NUL`. `toml_datum_is_text` rejects a TOML string that contains `\u0000`, and `toml_require_known_keys` a TOML key. This is a product decision, and the string type does not force it. If the product decision changes, these checks and the tests that pin them can go.

### A growable log, and every error in one run

**Decision.**

- Each fallible function whose caller needs a message takes `struct Log* log` as its last parameter. A callback type puts its `void* userdata` after the log. The log keeps every error with no truncation.
- The build continues after an error while the next phase still has something to check.
- The build stops before its first write if the log has an error.
- The log records the phases' errors in phase order, and the errors of one phase in walk order or in job order. The order does not change with the number of workers.
- A parallel phase prints at most twenty distinct job errors and a count of the others.
- A rejected config value keeps its default, so one bad value causes one error.

**Reasons.**

- A user sees all errors in one run.
- No message buffer has a fixed size, so no message is truncated, and none is ordered to survive truncation.

### Log messages use checked format strings

**Decision.** A log call takes one `printf` format and its arguments. `log_error`, `log_error_in`, `log_error_v`, `buf_printf`, and `buf_vprintf` have `__attribute__((format(printf, ...)))`. A `struct Str` prints with `%.*s` and `STR_FMT`. An integer prints with the conversion of its type. An example:

```c
log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path));
```

**Reasons.**

- Each message is one sentence in the source. A reader can read the wording as one line, and `grep` finds it.
- The compiler checks the number and the types of the arguments against the format. `-Wformat=2` is on for first-party code and for `sharedstuff`.
- `%td` prints a `ptrdiff_t` with no cast. Only a string needs a helper, `STR_FMT`.
- A first-party function that formats a message gets the same check: it has the attribute.

**Costs and limits.**

- `STR_FMT` evaluates its argument two times. Give it a variable or another lvalue. Put a computed string in a variable first.
- A string longer than `INT_MAX` bytes trips an assertion at run time, because the `%.*s` precision is an `int`.
- The attribute is the only guard on the code that reads the arguments. Each formatting function measures the text with `vsnprintf`, allocates it, and writes it with a second `vsnprintf`: `buf_printf` and `buf_vprintf` in `sharedstuff`, which grow the buffer in place, and the first-party functions that format their own message. The measured `int` length becomes a `ptrdiff_t` only in those functions.
- A first-party variadic function does not pass its `va_list` on to `log_error_v` or `buf_vprintf`. Clang's analyzer, version 22, targeting AArch64 Linux, where a `va_list` is a structure, reports a false `va_end` on an uninitialized list when it does. The function starts its list once for each `vsnprintf` pass instead, as `buf_printf` does: `record_error` in `cli.c`, `build_verbose` in `cmd_build.c`, `render_fail` in `template.c`, and `test_log_is` in `tests/test_support.c`.
- `%.*s` stops at a `NUL` byte. The input checks reject a `NUL` in file content, in TOML strings and keys, and in paths at the `os` boundary, so a message gets no `NUL` from input. A path that holds a `NUL` is quoted only up to that byte in its `EINVAL` message. The contract test for this shows the result.
- An `off_t` has no conversion of its own. It prints with `%jd` and a cast to `intmax_t`.
- `clang-format` breaks a string literal that passes the column limit. Some long messages are in two literals.

### The `os` interface

**Decision.** All host access goes through `src/runtime/os.h`. `fs.c` is the POSIX implementation. `tests/os_memory.c` is an in-memory implementation for unit tests. The link step selects one implementation for each executable. A function that touches the host takes `struct Os*`, just before its arenas, and an `os_*` function takes it first.

**Reasons.**

- Tests of the walk, the manifest, and the writes need no temporary directories.
- A fuzzing harness can drive the manifest logic without disk access.

**Condition.** Each property that a test checks against the in-memory host must have a contract test. One test source compiles into two executables: one links the POSIX host and one links the in-memory host. Both must pass. Without the contract tests, the in-memory host can accept what POSIX rejects. For example, an in-memory host could accept `file/` as a file, but POSIX returns `ENOTDIR`, and a contract test checks it.

**The writes.** `os_write` takes one of two sources: bytes, or a file that it streams in 64 KiB pieces, so `os_write`, `os_mkdirs`, `os_remove` and `os_remove_dir` are the only operations that change the host, and a copy needs no operation of its own. It writes every output in place, through a symlink or a hard link at the destination, and neither atomically nor synced, so an interrupted build can leave a cut file. Deleting `output_dir` and building again recovers it.

### Where the shared code lives

**Decision.** `sharedstuff` holds the generic parts: the arena with reserve, commit, and poisoning, `Buf`, `Str`, and `Log`. Each program defines `os_oom`. Each program keeps its own `os` interface and its own arena layout. `arena_release` does not unpoison a whole region under the address sanitizer, which would write an eighth of each reservation as resident shadow.

**Reason.** These parts do not depend on what a program does.
