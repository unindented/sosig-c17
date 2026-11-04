<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>sosig</code> /ˈsɔsɪdʒ/</h1>

`sosig` is a small static site generator written in C17. It uses Mustache templates to convert Markdown and Org files into HTML pages.

## LLM disclosure

> [!warning]
> I used LLMs extensively to create this project, mostly Claude Opus 4.8, 5, and 5.5.

## Installation

Download a prebuilt binary from the [Releases](https://github.com/unindented/sosig-c17/releases) page, or [build it from source](#building).

## Usage

### Commands

`sosig` has two commands:

- `sosig build`: Generate the site in `output_dir`. This command has these flags:
  - `-w, --workers N`: Set the maximum number of worker threads for the build. `N` must be a positive integer. The default is the number of CPUs that `sosig` detects. The build never runs more workers than the number of content files, and never more than four workers for each online CPU.
  - `-n, --dry-run`: Report the changes that the build would make, but do not make them. See [Dry run](#dry-run).
  - `--force`: Ignore the [build cache](#build-cache) for this build.
  - `-v, --verbose`: Print build progress to `stderr`.
- `sosig config`: Print the resolved project configuration as TOML.

There's also these options:

- `sosig --version` (or `-V`): Print the version, and the grammars that the binary includes for [syntax highlighting](#syntax-highlighting).
- `sosig --help` (or `-h`): Print the list of commands.
- `sosig <command> --help`: Print the help for one command.

A command exits with status 0 when it succeeds, 1 when it fails, and 2 when it rejects the command line.

### Project layout

Run `sosig` from a directory that has this layout. The example uses the default directory names:

```text
content/
  post-1.md
  post-2.md
static/
  assets/
    site.css
  robots.txt
templates/
  partials/
    header.html
    footer.html
  atom.xml
  content.html
  index.html
sosig.toml
```

Each part of the project has one purpose:

- `content/` holds the content files: the regular files whose names end in `.md` or `.org`, in any directory below `content/`. Each content file is one entry. `sosig` ignores every other file in `content/`, so it does not copy an image or a file named `post.MD`. Put a file that the site must publish as it is in `static/`.
- `static/` holds the files that `sosig` copies into `output_dir` without changes, at the same relative path. For example, `static/robots.txt` becomes `output_dir/robots.txt`.
- `templates/` holds the Mustache templates and their partials, as [Templates](#templates) describes.
- `sosig.toml` is the config file, as [Configuration](#configuration) describes.

The content directory and the templates directory must exist, and the static directory is optional. The repository and the release archives contain no starter templates or static files, so each site supplies its own.

When `sosig` searches `content/` and `static/`, it follows symlinks to directories, but it reads each directory only once:

- If more than one path leads to a directory, `sosig` uses only one of them: the path that goes through no symlink, or, if every path goes through a symlink, the first of those paths in byte order. For example, if `blog` is a symlink to `posts`, the entries publish only under `posts`.
- `sosig` skips a symlink that points back into one of its own parent directories.

### Configuration

`sosig.toml` must set `base_url`, `title`, and `author`:

```toml
base_url = "https://www.example.com"
title = "C17 Notes"
author = "Author Name"
```

This table lists every key that the config file can contain. `sosig` rejects any other key. For example, if the file contains the misspelled key `ouput_dir`, `sosig` cannot load the file. It does not ignore the key and use the default value of `output_dir`.

| Key | Default | Meaning |
| --- | --- | --- |
| `base_url` | Required | The absolute URL of the site, for links in feeds. It must be an `http://` or `https://` URL with a host. It can have a port, a subpath, and a query string. `sosig` removes any trailing `/`, so a template can join it with the `url` of an entry and not produce `//`. |
| `title` | Required | The title of the site. |
| `author` | Required | The name of the author. |
| `permalink` | `"/{section}/{slug}.html"` | The pattern for the URL and output path of each entry. See [URLs and slugs](#urls-and-slugs). |
| `output_dir` | `"public"` | The directory that the build writes to. The build controls all of its contents. See [Output directory](#output-directory). |
| `content_dir` | `"content"` | The directory that contains the content files. |
| `templates_dir` | `"templates"` | The directory that contains the templates and partials. |
| `static_dir` | `"static"` | The directory of files that the build copies into `output_dir` without changes. |
| `content_template` | `"content.html"` | The template for each entry, unless the `template` field of the entry overrides it. |
| `aggregate_templates` | `["index.html"]` | The templates that the build renders once, with every entry that is not a draft. |
| `feed_templates` | `["atom.xml"]` | The templates that the build renders once, with the newest `feed_count` entries. |
| `feed_count` | `10` | The maximum number of entries in each feed. |

`sosig` removes trailing `/` characters from the values of the four directory keys. A directory value must not be empty. It can be an absolute path, or a relative path that contains `..`.

A template name in the configuration, or in the `template` field of an entry, must be a safe relative path. It can contain only letters, digits, `_`, `-`, `.`, and `/`. It cannot contain an empty segment, or the segment `.` or `..`. The name of an aggregate or feed template is both its source path below `templates_dir` and its output path below `output_dir`. So a name that contains `/` creates the same subdirectories in `output_dir`. If `aggregate_templates` or `feed_templates` is an empty array, the build renders no templates of that kind.

### Output directory

> [!caution]
> The first build into a directory removes everything in that directory that the build did not write, including hidden files. Set `output_dir` to a directory that you use for nothing else.

When a build writes an output, it replaces any file that is already at that path. After a successful build, the build removes every file and directory below `output_dir` that it did not write, so `output_dir` contains only the outputs of the current inputs. For example, the build removes the pages of deleted or renamed entries, the outputs of removed templates and removed static files, and anything that you added by hand. If a symlink does not lead to a current output, the build removes the symlink itself. It does not change the file or directory that the symlink points to. A failed build removes nothing. If rendering or writing fails before the build completes, some outputs can be new while other outputs stay unchanged.

Before a build searches the content directory, `sosig` rejects `output_dir` in these cases:

- `output_dir` is at or below `content_dir`, `templates_dir`, or `static_dir`. `sosig` also makes this check when `output_dir` does not exist yet.
- `output_dir` exists and contains one of those directories or the config file, for example with `output_dir = "."`.

Every search of an input directory skips `output_dir`, so a symlink that leads into `output_dir` cannot make the build read generated files as inputs.

Before it renders page templates or writes output files, `sosig` rejects an output plan that has one of these conflicts:

- Two outputs have the same path, for example a static file at the same path as a content page, an aggregate, or a feed. Paths that differ only in ASCII letter case, such as `About/index.html` and `about/index.html`, count as the same path, because a case-insensitive filesystem such as the macOS default stores them as one file.
- One output path must be both a file and a directory. This check also ignores ASCII letter case.
- An output is at or below `content_dir`, `templates_dir`, or `static_dir`, for example through a symlink inside `output_dir`. This check also applies when no file exists at that path yet.
- An output overwrites the config file, a content file, a static file, or any file below `templates_dir`, including partials.

These checks compare the directories themselves, not the text of the paths, so they also find a different spelling of the same directory, or a symlink to it. The build creates `output_dir` only after all of these checks pass, so a rejected build creates nothing.

### Dry run

`sosig build --dry-run` runs every phase of a build and renders every template. But it changes nothing in `output_dir`, and it does not create `output_dir`. For each path that the build would change, it prints one line to `stdout`. Each line has a mark, a tab, and the path:

```
write	public/about/index.html
write	public/index.html
write	public/feed.xml
remove	public/old-post
remove	public/old-post/index.html
```

| Mark | Meaning |
| --- | --- |
| `write` | The build would write this output. |
| `keep` | The build would not change this content page, aggregate, or feed, because the [build cache](#build-cache) shows that it is current. |
| `remove` | The build would remove this path, because the current inputs no longer produce it. The path can be a directory. |

The `write` and `keep` lines come first, in the order that the build plans them:

1. Content pages, newest first.
2. Aggregates.
3. Feeds.
4. Static files.
5. The build cache.

The `remove` lines come after them, in sorted order. To get only the paths for `sort`, `diff`, or `comm`, use `cut -f2-`. A dry run reports a failure in the same way as a build: on `stderr`, with the same exit code. A dry run that fails prints no listing. With `-v`, the progress messages still go to `stderr`, so `stdout` contains only the listing when you send it through a pipe.

A dry run writes nothing, so a build can still fail after a dry run succeeds. A dry run cannot find these failures:

- The build has no permission to write, or the disk is full.
- `sosig` cannot read a static file. A dry run does not open the files that it would copy.
- An old file is at a path where a new output needs a directory, or an old directory is at a path where a new output needs a file. For example, the old output is the file `public/blog`, and the new output is `public/blog/index.html`.
- `output_dir` exists but is not a usable directory.

### Build cache

A build writes the build cache to `_sosig/v1-build-cache.tsv` below `output_dir`. The build cache records the size and modification time of every input that the pages depend on: each content file and each file below `templates_dir`. Its first line records the configuration values that rendering reads, the `sosig` version, and the grammars that the binary includes.

The next build keeps a content page, and does not render it again, if its content file, its templates, and the configuration did not change. The build keeps the aggregates and feeds if no content file changed. Some changes make the build render pages again:

- If any entry changes, the build renders again each page whose template reads `content_entries` or `site.updated`.
- If any file below `templates_dir` changes, such as a partial, the build renders every page again.

Every build still converts the Markdown or Org of every content file.

Some tools, such as `exiftool -P`, change a file but keep its size and modification time. After such a change, the build can keep a page that no longer matches its content file. To correct this, run `sosig build --force` once. That build renders every page again and writes a new build cache.

When you publish `output_dir`, you also publish the build cache. Its output paths are already public, but it also contains the names of the template files, and the size and modification time of each content file and template. If you do not want to publish this information, exclude `_sosig/v1-build-cache.tsv` when you deploy.

### Limits

The build rejects an input file that is too large before it reads the file. The error message gives the limit and the file size.

| File                | Limit   |
| ------------------- | ------- |
| Content file        | 256 MiB |
| Config file         | 1 MiB   |
| Template or partial | 4 MiB   |

The build reserves its memory once, when it starts, in fixed sizes that come from these limits. Reserved memory that the build does not use takes address space but no memory. An address-space limit (`ulimit -v`) can refuse the full reservation. When that happens, the build runs fewer workers, down to one. If the reservation still does not fit, the build makes it smaller until it fits, so the build has less room for its data. [ARCHITECTURE.md](ARCHITECTURE.md#memory-reserve-then-commit-on-demand) describes this process. Because the build runs fewer workers before it makes its reservation smaller, the address-space limit that a build needs does not increase with `-w`:

- A build needs about 280 MB (`ulimit -v 270000`). With a lower limit, the build stops at startup with `sosig: out of memory`.
- The stacks of the worker threads need room beside the reservation: about 24 MB for two workers. If the limit leaves less room than this, the build fails with `cannot start worker thread`.
- A build can need more memory than it reserved, for example for a template whose nested sections expand without bound. Then the build prints the errors that the command and the failing job logged up to that point, prints `sosig: out of memory (in '<file>')`, and exits with status 1.
- Each worker has at least 256 MiB to keep the errors that it reports. A worker that reports about a million one-line errors, or about 290 files that each contain about 16,000 unknown keys, can also make the build end with that out-of-memory report.

## Writing content

Each content file is one entry. A Markdown entry starts with TOML frontmatter, and an Org entry starts with keywords. Both parsers accept a UTF-8 BOM and CRLF line endings.

> [!warning]
> Markdown and Org can contain raw HTML, and `sosig` writes this HTML to the output without sanitizing it. Build only content files that you trust.

### Entry fields

| Field | Markdown frontmatter | Org keyword | Value |
| --- | --- | --- | --- |
| `title` | `title` | `#+title:` | Required. Plain text, so Org markup such as `=foo=` shows as it is written. |
| `date` | `date`, a TOML date or datetime | `#+date:`, an Org timestamp such as `[2024-07-12 Fri]`, `<2024-08-19 Mon 11:02>`, or `2024-07-12`, read as UTC | Required. Templates receive it in RFC 3339 format, and `sosig` sorts the entries by it. |
| `description` | `description` | `#+description:` | Optional summary. The default is an empty string. |
| `tags` | `tags`, a list | `#+filetags:`, where `:Emacs:Org:` gives `Emacs` and `Org` | Optional. |
| `slug` | `slug` | `#+slug:` | Optional. The default is the name of the content file without its extension, normalized as [URLs and slugs](#urls-and-slugs) describes. |
| `draft` | `draft` | `#+draft:`, `true` or `false` | Optional. The default is `false`. `sosig` validates the fields of a draft, but skips its body and all of its output. |
| `template` | `template` | `#+template:` | Optional. A template name that overrides `content_template`. It must obey the [template name rules](#configuration). |

### Markdown frontmatter

The frontmatter is TOML between `+++` fences. This example uses every key that the frontmatter can contain:

```toml
+++
title = "Content Entry Title"
date = 2026-07-01T12:00:00Z
description = "Short summary for listings."
slug = "content-entry-title"
tags = ["c", "sosig"]
draft = false
template = "feature.html"
+++
```

`sosig` rejects any other key. For example, the misspelled key `drfat` makes the build fail. The build does not ignore the key and use the default value of `draft`.

### Org keywords

An Org entry needs no fences:

```org
#+title: Content Entry Title
#+date: [2026-07-01 Wed]
#+filetags: :c:sosig:
```

`sosig` reads the keywords at the top of the file. It stops at the first line that is not a blank line, a comment, a property drawer, or a keyword. It does not read a keyword in the body, such as a keyword inside a code block. `sosig` trims the value of a keyword and matches its name in any letter case, so `#+TITLE:` and `#+title:` are the same. Emacs also reads `#+title:`, `#+date:`, `#+description:`, and `#+filetags:`, so `sosig` can publish a note that you write for Emacs export, and you do not have to change the note.

`sosig` ignores every other keyword, such as `#+options:`, `#+author:`, or `#+startup:`, because Org files contain keywords for other tools. So a misspelled optional keyword such as `#+drfat:` has no effect. To mark an entry as a draft, use `#+draft: true`.

`sosig` merges a repeated keyword in the same way as Emacs:

- It joins `#+title:` lines with a space.
- It joins `#+description:` lines with a newline.
- It adds the tags of each `#+filetags:` line.
- It uses the first `#+slug:`, `#+draft:`, or `#+template:` line, as Emacs does for most keywords that have one value.
- It also joins `#+date:` lines. The result contains more than one timestamp, so the build rejects it.

The filename of a denote or org-roam note starts with a timestamp. The filename of a denote note also contains the tags. If such a note has no `#+slug:`, `sosig` publishes it at a URL such as `/20240819t110231-a-test-note-typescript.html`. This URL changes when you change the tags of the note. To keep the URL fixed, set `#+slug:`.

### URLs and slugs

The `permalink` pattern sets the URL and output path of each entry. It supports the placeholders `{slug}` and `{section}`. It must contain `{slug}`, so that each entry gets a different path. `{section}` is the directory of the content file, relative to `content_dir`, with each directory segment normalized as a slug. If an expanded path ends in `/`, `sosig` adds `index.html` to it. For example, `/{slug}/` gives an entry a URL such as `/<slug>/index.html`.

`sosig` normalizes a slug in these steps:

1. It changes ASCII letters to lowercase.
2. It replaces each run of ASCII punctuation or whitespace with `-`.
3. It removes leading and trailing dashes.
4. It replaces each non-ASCII input byte with lowercase hexadecimal digits.
5. It uses `untitled` if no characters remain.

### Markdown

`sosig` uses md4c to parse Markdown, and writes the HTML itself. It enables these extensions:

- Tables
- Strikethrough
- Task lists
- LaTeX math spans
- Spoilers
- Superscript and subscript
- Admonitions
- Footnotes
- Highlights

### Org

`sosig` uses org4c to parse Org, and writes the HTML itself. It renders `a_{b}` as a subscript and keeps `snake_case` as text, in the same way as `#+OPTIONS: ^:{}`. Its output differs from the HTML export of Emacs in these ways:

- An `#+OPTIONS:` line in a file overrides these settings, for example with `^:t`, `f:nil`, or `tex:nil`. This also happens when the line is inside a code block.
- `#+BEGIN_EXPORT html` blocks and `@@html:...@@` snippets pass raw HTML through to the output. `sosig` omits `#+HTML:` lines from the output.
- If Markdown has the same feature, `sosig` writes the same HTML as for Markdown. See [Generated HTML](#generated-html).
- `sosig` writes an inline `src_c{…}` as its code, unless its `:exports` is `none` or `results`. By default, Emacs exports the results of an inline `src_c{…}`, but `sosig` never evaluates code.
- A `[[id:...]]` link points to an anchor on the same page, so a link to another note does not work. A `[[file:other.org]]` link points to `other.html` in the same directory as the page. This link works only if the permalink of the other note gives that path.

As in Emacs, a property drawer at the top of the file renders nothing. A `Footnotes` headline and its subtree also render nothing, but their footnote definitions go into the footnote list at the end. The Org renderer of `sosig` is a port of the renderer in org4c, so the other differences that the org4c [README](https://github.com/unindented/org4c#differences-from-emacs-ox-html) lists also apply to `sosig`, except for the shared HTML that the next section describes.

### Generated HTML

For the features that Markdown and Org share, `sosig` writes the same HTML, so one stylesheet and one math script work for both formats:

- Bold text is `<strong>`, and italic text is `<em>`.
- Headings start at `<h2>`, one level below the `<h1>` of the page title in a content template. Markdown's `#` and Org's `*` are `<h2>`, and each lower level adds one, up to `<h6>`.
- A footnote reference is `<sup><a href="#fn-1" id="fnref-1-1" role="doc-noteref">1</a></sup>`. The footnotes come after the body, in a `<section class="footnotes" role="doc-endnotes">` list. Each footnote has `role="doc-backlink"` links back to its references. A footnote with one paragraph has no `<p>`. An Org footnote with more than one paragraph keeps each paragraph as a `<p>`.
- A task list item starts with a disabled `<input type="checkbox" class="task-list-item-checkbox">`. Org's partly done `[-]` item is unchecked.
- A note, tip, important note, warning, or caution is a `<div class="admonition-note">` (or `admonition-tip`, and so on) that starts with a `<p class="admonition-title">`. In Markdown, it is a `> [!NOTE]` block. In Org, it is a `#+BEGIN_NOTE` block. In both formats, the block name can be in any letter case.
- Inline math is `<x-equation>`, and displayed math is `<x-equation type="display">`. The element contains the LaTeX, for a script such as MathJax or KaTeX. In Org, `$y$` and `\(y\)` are inline, and `$$y$$`, `\[y\]`, and a LaTeX environment are displayed.
- A code block is `<pre><code class="language-c">`. An Org code block with a `#+NAME:`, a `#+CAPTION:`, line numbers, or code references keeps the Org markup for these features: an `id`, a `Listing` label, and `linenr` and `coderef` spans around each line. An inline `src_c{…}` is Org's own `<code class="src src-c">`. `sosig` highlights both of them in the same way as any other code block.

### Syntax highlighting

If the binary includes a grammar for the language of a code block, `sosig` writes each token of the block as a `<span>`. The class of the `<span>` tells the kind of token:

- `tok-attribute`
- `tok-comment`
- `tok-constant`, `tok-constant-builtin`
- `tok-constructor`
- `tok-delimiter`
- `tok-embedded`
- `tok-escape`
- `tok-function`, `tok-function-builtin`, `tok-function-method`, `tok-function-special`
- `tok-keyword`
- `tok-label`
- `tok-number`
- `tok-operator`
- `tok-property`
- `tok-punctuation-bracket`, `tok-punctuation-delimiter`, `tok-punctuation-special`
- `tok-string`, `tok-string-special`
- `tok-tag`, `tok-tag-error`
- `tok-type`, `tok-type-builtin`
- `tok-variable`, `tok-variable-builtin`, `tok-variable-parameter`

`sosig` does not supply a stylesheet, so each site must set the colors for these classes.

The release binaries highlight these languages:

| Language | Names for code blocks |
| --- | --- |
| C | `c` |
| Bourne shells | `sh`, `shell`, `bash`, and the other Bourne-family names that Emacs gives to `sh-mode`, such as `ksh`, `dash`, and `zsh` |
| HTML | `html` |
| CSS | `css` |
| JavaScript with JSX | `javascript`, `js`, `jsx` |
| TypeScript | `typescript`, `ts` |
| TSX | `tsx` |
| JSON | `json` |

A language name matches in any letter case, so `sosig` highlights a `Bash` code block, but Emacs does not. If you compile `sosig` yourself, you can choose its grammars with `SOSIG_GRAMMARS`, as [BUILD.md](BUILD.md#grammars) describes. `sosig --version` lists the grammars that a binary includes.

`sosig` highlights each code block in only one language, so in an HTML code block it only escapes the contents of the `<script>` and `<style>` elements. `sosig` does not highlight a code block, and only escapes its text, in these cases:

- The code block has no language.
- The binary includes no grammar for the language of the code block.
- The code block is too large or too complex to highlight: tree-sitter would need more than 128 MiB for it, or it contains tens of thousands of nested or unclosed brackets.

## Templates

Templates use [Mustache](https://mustache.github.io/) syntax and are in `templates_dir`. There are three kinds of templates:

| Kind | Configured by | Rendered |
| --- | --- | --- |
| Content | `content_template`, or the `template` field of an entry | Once for each entry, to the output path of the entry. The fields of the entry are available at the top level. |
| Aggregate | `aggregate_templates` | Once, to the same relative path below `output_dir`. The fields of an entry are not available at the top level. |
| Feed | `feed_templates` | In the same way as an aggregate template, but with only the newest `feed_count` entries. |

`{{value}}` escapes its output for HTML, and `{{{value}}}` writes its output without escaping. Use `{{{body}}}` to write the rendered body.

| Variable | Templates | Value |
| --- | --- | --- |
| `site.title`, `site.base_url`, `site.author` | All | The values from the configuration. |
| `site.updated` | All | The newest entry date, or `1970-01-01T00:00:00Z` for an empty site. |
| `content_entries` | All | Every entry that is not a draft, newest first. A feed template receives only the newest `feed_count` entries. Use `{{#content_entries}}` to iterate through them. |
| `generator` | All | The name and version of `sosig`, for a tag such as `<meta name="generator" content="{{generator}}">`. |
| `title`, `date`, `description`, `slug`, `url`, `body`, `tags` | Content templates, and any template inside `{{#content_entries}}` | The fields of the entry. Use `{{#tags}}{{.}}{{/tags}}` to iterate through the tags. |

A partial reference `{{> name}}` loads `templates_dir/partials/<name>.html`. A partial name can contain only letters, digits, `_`, and `-`.

```html
{{> header}}
<article>
  <h1>{{title}}</h1>
  <p class="meta">{{date}} | {{#tags}}{{.}} {{/tags}}</p>
  {{{body}}}
</article>
{{> footer}}
```

## Development

[ARCHITECTURE.md](ARCHITECTURE.md) describes the code. [BUILD.md](BUILD.md) describes the CMake targets and the reasons for the configurations below. [STYLE.md](STYLE.md) describes the code style.

### Prerequisites

- A POSIX-like system with CMake 3.25 or later, a C17 compiler such as Clang or GCC, and pthreads.
- Optional build tools: `ninja` for the `multi` preset.
- Optional quality tools: `cwrap`, `clang-format`, `clang-tidy`, `cppcheck`.
- Zig 0.17 and `llvm-strip` for the Linux `musl` archives.

The source tree includes the dependencies in [vendor/](vendor/).

### Building

Each preset builds in `build/<preset>`, outside the source tree, and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>` instead, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own CMake cache. The commands use eight parallel jobs. Change that number to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | Binary |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are available. | `build/debug/bin/sosig` |
| `tsan` | `Debug` with ThreadSanitizer. | `build/tsan/bin/sosig` |
| `release` | `RelWithDebInfo`, with no tests. The binary in the build tree keeps its debug information, and packaging strips the installed copy. | `build/release/bin/sosig` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/bin/Debug/sosig`, `build/multi/bin/RelWithDebInfo/sosig` |

`clang-tidy` runs only with Clang, because it rejects the warning options of GCC. To run linting, configure the `debug` preset, and then run this command:

```sh
cmake --build --preset lint
```

### Testing

CTest registers the unit tests and the end-to-end golden tests. Build a preset, and then run its tests:

```sh
ctest --preset debug -j 8
```

The test presets are `debug`, `tsan`, `multi-debug`, and `multi-relwithdebinfo`. The `multi-relwithdebinfo` tests check the same CMake configuration as the `release` preset, which builds no tests. To run only part of a suite, select the tests by label or by name pattern:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -L golden -j 8
ctest --preset debug -R template -j 8
```

### Continuous integration

The workflow presets run the same configure, build, and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run linting, and run all ASan and UBSan tests. |
| `cmake --workflow --preset ci-tsan` | Build with TSan and run all TSan tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-grammars` | Build in `RelWithDebInfo` with only the TypeScript grammar. CI runs this workflow only on Linux. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. CI runs this workflow a second time with `CC=gcc-14` to check GCC. |

To run the Linux workflows on a machine with Podman, build the pinned Ubuntu image from [Containerfile](Containerfile). Then run each workflow in a new container:

```sh
podman build --tag sosig-linux-ci --file Containerfile .
podman run --rm sosig-linux-ci ci-debug
podman run --rm sosig-linux-ci ci-tsan
podman run --rm sosig-linux-ci ci-release
podman run --rm sosig-linux-ci ci-grammars
podman run --rm sosig-linux-ci ci-multi
podman run --rm -e CC=gcc-14 sosig-linux-ci ci-multi
```

The image contains a snapshot of the source tree, including the vendored dependencies, without `.git/` and `build/`, which [.containerignore](.containerignore) excludes, so the Linux build products stay in the container and do not go into the `build/` directory of the host. The image supports x86-64 and AArch64 hosts, and pins LLVM 23 and GCC 14. It does not include Zig, because only the release archives need Zig.

### Packaging

Only the release configurations have package presets. Build a preset, and then package it:

```sh
cmake --preset release
cmake --build --preset release -j 8
cpack --preset release
```

| Preset | Archive |
| --- | --- |
| `release` | A stripped native archive in `build/release/`. |
| `release-linux-x86_64` | `build/release-linux-x86_64/sosig-<version>-x86_64-linux-musl.tar.gz`, cross-built with Zig. |
| `release-linux-aarch64` | `build/release-linux-aarch64/sosig-<version>-aarch64-linux-musl.tar.gz`, cross-built with Zig. |

A source archive needs a configured tree but no compiled binary. It does not contain build products or private workspace metadata:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The source archive is `build/release/sosig-<version>-source.tar.gz`.

The version of every archive comes from `project(VERSION)` in [CMakeLists.txt](CMakeLists.txt). Tag each release commit with the matching `vMAJOR.MINOR.PATCH`.

## Dependencies

[vendor/](vendor/) contains these dependencies. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists their licenses.

- [sharedstuff](https://github.com/unindented/sharedstuff-c17): Arena allocation, counted strings, output buffers, and the diagnostic log.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [tomlc17](https://github.com/cktan/tomlc17): TOML configuration and frontmatter parsing.
- [md4c](https://github.com/mity/md4c): Markdown parsing.
- [org4c](https://github.com/unindented/org4c): Org parsing.
- [mustache4c](https://github.com/mity/mustache4c): Mustache template rendering.
- [tree-sitter](https://github.com/tree-sitter/tree-sitter): Syntax highlighting, with these grammars:
  - [tree-sitter-c](https://github.com/tree-sitter/tree-sitter-c)
  - [tree-sitter-bash](https://github.com/tree-sitter/tree-sitter-bash)
  - [tree-sitter-html](https://github.com/tree-sitter/tree-sitter-html)
  - [tree-sitter-css](https://github.com/tree-sitter/tree-sitter-css)
  - [tree-sitter-javascript](https://github.com/tree-sitter/tree-sitter-javascript)
  - [tree-sitter-typescript](https://github.com/tree-sitter/tree-sitter-typescript)
  - [tree-sitter-json](https://github.com/tree-sitter/tree-sitter-json)
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

`sosig` is MIT-licensed, as [LICENSE.txt](LICENSE.txt) states.
