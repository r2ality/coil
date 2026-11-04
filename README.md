# Coil

A minimal Rust terminal interface for [yt-dlp](https://github.com/yt-dlp/yt-dlp).

Coil intentionally does not implement its own website extractors. It passes the URL and selected options directly to `yt-dlp`, so supported sites and extractor fixes follow the installed yt-dlp version.

## Requirements

- Rust and Cargo
- `yt-dlp` available on `PATH`
- `ffmpeg` and `ffprobe` strongly recommended; they are needed for audio extraction and merging separate audio/video streams, and support post-processing.
- For full YouTube support with current yt-dlp releases, install `yt-dlp-ejs` and a supported JavaScript runtime as recommended by yt-dlp.

## Build and run

```sh
cargo build --release
./target/release/coil
```

## Controls

- `Tab`: switch between Main and Settings
- `↑` / `↓`: select a row
- `←` / `→`: change toggles and quality/mode selectors
- `Enter`: edit the URL or download folder, or activate the selected action
- Paste: paste a URL while the URL row is selected; paste a path while the folder row is selected
- `d`: start a download
- `q`: quit (when not editing; a running yt-dlp process is terminated)
- `Ctrl+U`: clear the field currently being edited
- `Esc`: stop editing; on Settings, return to Main

## Main page

- Playlist download can be explicitly enabled or disabled.
- Cover embedding uses yt-dlp's `--embed-thumbnail` option.
- Media mode can be Audio only, Video only, or Audio + Video.
- Audio-only downloads are extracted to MP3 at the selected quality.
- Video quality is a maximum-height cap; yt-dlp chooses the best available format up to that resolution. Video-only mode requests a video-only stream, so a site that exposes only combined audio/video formats may not offer a suitable result.

## Settings

The default output directory is `~/Downloads/Coil`. Coil expands `~` and creates the directory when a download starts. Preferences are stored in:

- `$XDG_CONFIG_HOME/coil/config`, when `XDG_CONFIG_HOME` is set
- otherwise `~/.config/coil/config`

On Unix, the config file is created with user-only read/write permissions.

## Notes

- Coil invokes `yt-dlp` directly with process arguments; it does not build a shell command or pass the URL through a shell.
- TLS certificate verification is left enabled. Coil does not disable yt-dlp's security checks, import browser cookies, or accept arbitrary extra command-line options.
- `--concurrent-fragments 4`, retries, and resumable downloads are enabled as conservative defaults. Actual speed depends on the website and available formats.
- Playlist mode continues past individual item errors, so the rest of a playlist can finish.
- A cover or format conversion/merge may require FFmpeg. Without it, yt-dlp can still download formats that do not need post-processing.
