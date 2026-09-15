<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>sosig</code> /ˈsɔsɪdʒ/</h1>

This is a small static site generator written in C17.

## LLM disclosure

> [!warning]
> I used LLMs extensively to create this project, mostly Claude Opus 4.8 and 5.

## Installation

Download a prebuilt binary from the [Releases](https://github.com/unindented/sosig-c17/releases) page, or [build from source](#contributing).

## Usage

`sosig` is command-based:

- `sosig build`: Generate the site in the configured output directory. Use `-w, --workers N` to set the number of render threads. The value must be from 1 through 1024. By default, the tool uses the detected CPU count. Use `-v, --verbose` to print build progress to `stderr`.
- `sosig config`: Print the resolved project configuration as TOML.

Run these commands for help:

- `sosig --version` (or `-V`): Print the version.
- `sosig --help` (or `-h`): Print the list of available commands.
- `sosig <command> --help`: Print the help for one command.

### Site layout

Run the tool from a location with these contents:

```text
content/
  post-1.md
  post-2.md
templates/
  partials/
    header.html
    footer.html
  atom.xml
  content.html
  index.html
sosig.toml
```

`sosig.toml` must contain the required metadata fields. The example below also shows the default values for optional fields.

The tool removes trailing `/` characters from `content_dir`, `output_dir`, and `templates_dir`. These fields must not be empty. Each field can contain an absolute or parent-relative path.

The build searches all directories under `content_dir` for regular files that end in `.md`. It creates `output_dir` and all required output subdirectories. It overwrites each output file that the current build generates.

Before the tool renders page templates or writes files, it rejects an output plan with one of these conflicts:

- Two producers use the same output path.
- One output path must be both a file and a directory.
- An output overwrites the config, a content source, or a template named in config or frontmatter.

The build does not remove stale files from an earlier build. If the write phase fails, some outputs can be new while others remain unchanged.

```toml
# Required absolute site URL for feed links. It must be an `http://` or `https://` URL with a host.
# The tool accepts a port, subpath, and query string. The tool trims any trailing `/`. A template
# can then join it with an entry's `url` and not produce `//`.
base_url = "https://www.example.com"

# Required site title.
title = "C17 Notes"

# Required author name.
author = "Author Name"

# Pattern for each content URL and output path. It supports `{slug}` and `{section}`. The pattern
# must use `{slug}` to produce a different path for each entry. `{section}` is the normalized source
# directory relative to `content_dir`. If an expanded path ends in `/`, the tool appends
# `index.html`. For example, `/{slug}/` gives the entry a URL such as `/<slug>/index.html`.
permalink = "/{section}/{slug}.html"

# Directory containing Markdown source files.
content_dir = "content"

# Directory where generated files are written. This directory is site-owned.
output_dir = "public"

# Directory containing templates and partials.
templates_dir = "templates"

# Template used for content entries unless a frontmatter template overrides it.
content_template = "content.html"

# Aggregate templates rendered with all non-draft content entries.
aggregate_templates = ["index.html"]

# Feed templates rendered with `feed_count` newest content entries.
feed_templates = ["atom.xml"]

# Maximum number of content entries included in each feed template.
feed_count = 10
```

Template names in the configuration must be safe relative paths. They can contain letters, digits, `_`, `-`, `.`, and `/`. They cannot contain empty segments or the segments `.` and `..`.

An aggregate or feed template name has two uses. It is the source path below `templates_dir` and the output path below `output_dir`. A nested name creates the same subdirectories in `output_dir`. Use an empty array to disable aggregate templates or feed templates.

### Content

Each content entry starts with TOML frontmatter between `+++` fences. The parser accepts a UTF-8 BOM and CRLF line endings. For a nested source, `{section}` contains its source directory relative to `content_dir`. The tool normalizes each directory segment as a slug.

```toml
+++
# Required content entry title.
title = "Content Entry Title"

# Required TOML date or datetime. The tool converts it to RFC 3339 for templates and uses the
# equivalent Unix time for sorting.
date = 2026-07-01T12:00:00Z

# Optional summary. Defaults to an empty string.
description = "Short summary for listings."

# Optional URL slug source. Defaults to the source filename without extension. Either value is
# normalized to a lowercase URL-safe slug.
slug = "content-entry-title"

# Optional tag list.
tags = ["c", "sosig"]

# Optional draft flag. The tool validates draft frontmatter but skips its body and all output.
draft = false

# Optional template name override for this content entry.
template = "feature.html"
+++
```

The tool normalizes a slug as follows:

1. It changes ASCII letters to lowercase.
2. It replaces each run of ASCII punctuation or whitespace with `-`.
3. It removes leading and trailing dashes.
4. It replaces each non-ASCII input byte with lowercase hexadecimal digits.
5. It uses `untitled` if no characters remain.

A frontmatter `template` override follows the same safe-relative-path rules as configured template names.

### Markdown

The tool renders Markdown with md4c. It enables these extensions:

- Tables
- Strikethrough
- Task lists
- LaTeX math spans
- Spoilers
- Superscript and subscript
- Admonitions
- Footnotes
- Highlights

The renderer permits raw HTML and does not sanitize it. Only use content files that you trust.

### Templates

Templates use [Mustache](https://mustache.github.io/) syntax and live in `templates_dir`. There are three kinds:

- **Content templates** (`content_template`, or a per-entry `template` override): The tool renders one per content entry. You can access that entry's fields in the template.
- **Aggregate templates** (`aggregate_templates`): The tool renders each template once. It writes the result to the same relative path below `output_dir`. Entry fields are not available at the top level.
- **Feed templates** (`feed_templates`): These work like aggregate templates. They can access only the newest `feed_count` entries.

`{{value}}` HTML-escapes its output. `{{{value}}}` writes it raw. Use `{{{body}}}` to emit the rendered Markdown body.

Available variables:

- `site.*` (all templates): `site.title`, `site.base_url`, `site.author`, and `site.updated`. `site.updated` contains the newest entry date. For an empty site, it contains `1970-01-01T00:00:00Z`.
- `content_entries` (all templates): This list contains all non-draft entries from newest to oldest. Iterate through it with `{{#content_entries}}`. A feed template receives only the newest `feed_count` entries.
- Entry fields: A content template can access `title`, `date`, `description`, `slug`, `url`, `body`, and `tags` at the top level. These fields are also available inside a `{{#content_entries}}` section. Iterate through tags with `{{#tags}}{{.}}{{/tags}}`.

A partial reference loads `templates_dir/partials/<name>.html`. The name can contain only letters, digits, `_`, and `-`:

```html
{{> header}}
<article>
  <h1>{{title}}</h1>
  <p class="meta">{{date}} | {{#tags}}{{.}} {{/tags}}</p>
  {{{body}}}
</article>
{{> footer}}
```

## Contributing

### Prerequisites

- POSIX-like system with CMake 3.25 or later, a C17 compiler such as Clang or GCC, and pthreads.
- Optional build tools: `ninja` for the multi-config portability check.
- Optional quality tools: `clang-format`, `clang-tidy`, `cppcheck`.

Vendored dependencies are included under `vendor/`:

- [`copt`](https://github.com/fardaniqbal/copt): Command line option parsing.
- [`tomlc17`](https://github.com/cktan/tomlc17): TOML frontmatter parsing.
- [`md4c`](https://github.com/mity/md4c): Markdown-to-HTML conversion.
- [`mustache4c`](https://github.com/mity/mustache4c): Mustache template rendering.
- [`acutest`](https://github.com/mity/acutest): Tests.

### Building

Presets keep every build out of the source tree. The commands below use eight parallel build jobs. Adjust that number for the machine.

#### Debug build

The debug build enables `AddressSanitizer` and `UndefinedBehaviorSanitizer`. It also runs `clang-tidy` and `cppcheck` during compilation when they are available.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

The binary is `build/debug/bin/sosig`.

To run linting after configuring this preset:

```sh
cmake --build --preset lint
```

#### TSan build

The TSan build uses the `Debug` configuration, and instruments the build for data races using `ThreadSanitizer`.

```sh
cmake --preset tsan
cmake --build --preset tsan -j 8
```

The binary is `build/tsan/bin/sosig`.

#### Release build

The release preset uses `RelWithDebInfo`, treats compiler warnings as errors, and does not build the tests. The build-tree binary retains its debug information. Packaging strips the installed copy.

```sh
cmake --preset release
cmake --build --preset release -j 8
```

The binary is `build/release/bin/sosig`.

#### Multi-config build

The multi-config preset uses the `Ninja Multi-Config` generator. One configured tree can build both configurations:

```sh
cmake --preset multi
cmake --build --preset multi-debug -j 8
cmake --build --preset multi-relwithdebinfo -j 8
```

The binaries are `build/multi/bin/Debug/sosig` and `build/multi/bin/RelWithDebInfo/sosig`.

### Testing

CTest registers the colocated unit test suite and the end-to-end golden test suite.

#### Debug tests

```sh
cmake --preset debug
cmake --build --preset debug -j 8
ctest --preset debug -j 8
```

To select part of the debug suite:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -L golden -j 8
ctest --preset debug -R template -j 8
```

#### TSan tests

```sh
cmake --preset tsan
cmake --build --preset tsan -j 8
ctest --preset tsan -j 8
```

#### Multi-config tests

Each configuration must be named when building and testing:

```sh
cmake --preset multi

cmake --build --preset multi-debug -j 8
ctest --preset multi-debug -j 8

cmake --build --preset multi-relwithdebinfo -j 8
ctest --preset multi-relwithdebinfo -j 8
```

The `multi-relwithdebinfo` test preset exercises the same CMake configuration used by the release build. The release preset itself does not build tests.

#### CI workflows

Workflow presets run the complete configure, build, and test sequences used by CI:

- `cmake --workflow --preset ci-debug`: `Debug` build, linting, and all ASan/UBSan tests.
- `cmake --workflow --preset ci-tsan`: TSan build, and all tests.
- `cmake --workflow --preset ci-release`: `Release` build.
- `cmake --workflow --preset ci-multi`: `Debug` and `RelWithDebInfo` builds and tests under the `Ninja Multi-Config` generator.

To run the same Linux workflows from a machine with Podman, build the pinned Ubuntu image:

```sh
podman build --tag sosig-linux-ci --file Containerfile .
```

The image contains a snapshot of the source tree, so container builds do not mix Linux products with the host's `build/` directory. Run each workflow preset in a fresh container:

```sh
podman run --rm sosig-linux-ci ci-debug
podman run --rm sosig-linux-ci ci-tsan
podman run --rm sosig-linux-ci ci-release
podman run --rm sosig-linux-ci ci-multi
```

The image supports x86-64 and AArch64 hosts and pins LLVM 22. Zig remains a release-only dependency and is not included in the CI image.

See [BUILD.md](BUILD.md) for the target graph and the reasons behind these configurations.

### Packaging

Only release configurations have package presets. There is intentionally no debug package.

#### Native release package

```sh
cmake --preset release
cmake --build --preset release -j 8
cpack --preset release
```

The stripped archive is written to `build/release/`.

#### x86-64 Linux `musl` package

This cross-build requires Zig 0.16 and `llvm-strip`.

```sh
cmake --preset release-linux-x86_64
cmake --build --preset release-linux-x86_64 -j 8
cpack --preset release-linux-x86_64
```

The archive is `build/release-linux-x86_64/sosig-<version>-x86_64-linux-musl.tar.gz`.

#### AArch64 Linux `musl` package

This cross-build also requires Zig 0.16 and `llvm-strip`.

```sh
cmake --preset release-linux-aarch64
cmake --build --preset release-linux-aarch64 -j 8
cpack --preset release-linux-aarch64
```

The archive is `build/release-linux-aarch64/sosig-<version>-aarch64-linux-musl.tar.gz`.

#### Source package

A source package needs configuration but does not need a compiled binary:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The archive is `build/release/sosig-<version>-source.tar.gz`. It excludes build products and private workspace metadata.

Every archive version comes from `project(VERSION)` in `CMakeLists.txt`. A release commit must be tagged with the matching `vMAJOR.MINOR.PATCH`.
