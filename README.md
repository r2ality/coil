# SoundFetch

A lightweight SoundCloud downloader with a native C++/FLTK desktop interface and a Go backend. It is designed around the simple workflow of a desktop `yt-dlp` frontend: paste a SoundCloud URL, inspect the resolved tracks, choose an audio quality, and download one track or a whole playlist.

## Features

- Download individual public SoundCloud tracks.
- Fetch public playlists / sets and download selected tracks or the complete list.
- Embed available track metadata and cover artwork.
- Preserve the best available source audio, or convert to MP3 at 320 / 256 / 192 / 128 kbps.
- Up to three concurrent track downloads, with retries for transient network and fragment failures.
- Optional HTTP, HTTPS, SOCKS4 and SOCKS5 proxy.
- Configurable download directory; settings are saved under `~/.config/soundfetch/settings.json` (or `$XDG_CONFIG_HOME/soundfetch/settings.json`).
- Per-track progress, speed / ETA, error reporting and queue cancellation.

## Architecture

- `frontend/main.cpp`: C++17 desktop GUI using FLTK.
- `backend/main.go`: Go JSON-lines service. It validates input, resolves lists, runs a small bounded worker pool and emits structured progress events.
- `yt-dlp`: SoundCloud extractor / transfer engine. Keeping the extraction layer external means SoundFetch benefits from yt-dlp's maintained SoundCloud support instead of relying on a brittle, hard-coded private API client ID.
- `ffmpeg`: audio extraction / conversion, metadata and cover embedding.

The C++ app starts `soundfetch-backend` as a child process and exchanges one JSON object per line over stdin/stdout. This keeps the interface responsive and means the Go worker can be tested or replaced independently.

## Build on NixOS

```sh
nix build
./result/bin/soundfetch
```

The Nix package includes FLTK, nlohmann-json, Go, yt-dlp and FFmpeg. CMake receives FLTK’s Nix-store include path explicitly, and the build only requires FLTK’s core library (it does not require the unused OpenGL, Forms, Images or Fluid components). The launcher adds yt-dlp and FFmpeg to `PATH` for the backend.

## Build manually (Linux)

Dependencies:

- Go 1.22 or newer
- CMake 3.16 or newer
- C++17 compiler
- FLTK development files
- nlohmann-json development headers
- yt-dlp
- FFmpeg (including ffprobe)

Build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The executable files are `build/soundfetch` and `build/soundfetch-backend`. Keep them in the same directory. Make sure `yt-dlp` and `ffmpeg` are available in `PATH` before launching the GUI.

## Audio quality

- **Best available (keep source):** asks yt-dlp for the best audio-only stream and avoids a lossy MP3 re-encode.
- **MP3 options:** convert with FFmpeg to the selected target bitrate. A higher target bitrate cannot restore detail missing from the SoundCloud source stream.

Metadata and artwork are embedded when the source container and the installed FFmpeg / yt-dlp post-processors support it. The downloaded file retains the stream's actual quality; SoundFetch does not claim to turn a compressed source into lossless audio.

## URL support and limitations

The app accepts SoundCloud track links, public sets / playlists, album links and artist track-list URLs when yt-dlp can resolve them. Private, removed, preview-only, geo-blocked, paid or otherwise restricted tracks may fail or be unavailable. SoundFetch does not include cookie import, account-token extraction, DRM circumvention or access-control bypass features. A configured proxy is only a network-routing option; it is not intended to unlock restricted content.

Only download works that you own, that the creator has made downloadable, or that you otherwise have permission to save. Respect SoundCloud's Terms of Use and the rights of creators.

## Troubleshooting

- **“yt-dlp was not found”**: install yt-dlp and make sure it is on `PATH`. On NixOS, use the provided flake package.
- **No track list**: update yt-dlp (`yt-dlp -U` for standalone installs, or update your Nix flake input), then retry. SoundCloud changes its web extraction behavior periodically.
- **Artwork is not embedded**: check that FFmpeg and the optional image-tagging dependencies shipped with your yt-dlp install are available. Some stream containers have limited cover-art support.
- **Proxy error**: include the scheme, e.g. `socks5h://127.0.0.1:9050`, not just `127.0.0.1:9050`.

## License

The SoundFetch source code is MIT licensed. yt-dlp, FFmpeg, FLTK and nlohmann-json are separate projects with their own licenses; see their respective documentation for terms.
