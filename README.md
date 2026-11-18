# epub

A tiny terminal EPUB reader written in C. One source file, one dependency (zlib), no ncurses.

```
$ ./epub book.epub
```

## Features

- Reads `.epub` files directly: a built-in ZIP parser plus zlib inflate, with no `libzip` or `unzip` needed
- Follows the EPUB spine, so chapters appear in the correct reading order
- Converts XHTML to clean text: strips tags, skips `<head>`, `<script>` and `<style>`, decodes HTML entities, renders lists as bullets
- UTF-8 aware word wrapping (French accents, Arabic and other scripts are counted by code point, not byte)
- Centered reading column (max 80 columns) that re-wraps on terminal resize
- Status bar with book title, chapter number and progress
- `--dump` mode to print the whole book as plain text
- Restores the terminal cleanly on quit, Ctrl-C or `SIGTERM`

## Build

Requirements: a C compiler and zlib headers.

```sh
# Debian / Ubuntu / WSL
sudo apt install build-essential zlib1g-dev

# macOS
xcode-select --install

# Build
cc -O2 -Wall -o epub epub.c -lz
```

Optional install:

```sh
sudo cp epub /usr/local/bin/
```

### Windows

The program uses POSIX terminal APIs (`termios`, `poll`, `ioctl`), so it does not build with MSVC or plain MinGW. Use one of:

- **WSL** (recommended): `wsl --install`, then follow the Debian/Ubuntu steps above. Windows files are under `/mnt/c/...`.
- **MSYS2** (`MSYS2 MSYS` shell): `pacman -S gcc zlib-devel`, then `gcc -O2 -o epub epub.c -lz`.

## Usage

```sh
./epub book.epub            # interactive reader
./epub book.epub --dump     # print the whole book as plain text
```

### Keys

| Key | Action |
|---|---|
| `Space`, `PgDn`, `f` | Page down (continues into the next chapter at the end) |
| `b`, `PgUp` | Page up (continues into the end of the previous chapter) |
| `j`, `↓`, `Enter` | Scroll one line down |
| `k`, `↑` | Scroll one line up |
| `n`, `→` | Next chapter |
| `p`, `←` | Previous chapter |
| `g`, `Home` | Top of chapter |
| `G`, `End` | Bottom of chapter |
| `q` | Quit |

## How it works

1. **ZIP**: locate the End Of Central Directory record, read the entry table, and inflate entries with `inflateInit2(&s, -15)` (raw deflate).
2. **Container**: `META-INF/container.xml` names the OPF package file.
3. **Package**: the OPF manifest maps ids to files, and the `<spine>` gives the chapter order. The book title comes from `<dc:title>`.
4. **Text**: each chapter's XHTML is converted to plain text in a single pass.
5. **Layout**: greedy word wrap, then paging with raw-mode terminal input.

## Limitations

- Text only: no images, CSS or fonts
- No table of contents yet, so chapters are shown as `ch N/M` rather than by title
- No ZIP64, encrypted or DRM-protected books
- Right-to-left scripts (Arabic, Hebrew) display, but shaping and ordering depend on your terminal's bidi support
- Reading position is not saved between sessions

## Ideas / TODO

- [ ] Table of contents from the EPUB 3 nav document / EPUB 2 NCX
- [ ] `/` search
- [ ] Save and restore reading position per book
- [ ] Jump to chapter by number
- [ ] Basic bold/italic via ANSI styles

## License

MIT. Add a `LICENSE` file, or replace this section with your preferred license.
