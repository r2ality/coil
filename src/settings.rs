use std::env;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::PathBuf;

pub const AUDIO_QUALITIES: [&str; 5] = [
    "Best",
    "320 kbps",
    "256 kbps",
    "192 kbps",
    "128 kbps",
];

pub const VIDEO_QUALITIES: [&str; 7] = [
    "Best",
    "2160p",
    "1440p",
    "1080p",
    "720p",
    "480p",
    "360p",
];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MediaMode {
    AudioOnly,
    VideoOnly,
    AudioVideo,
}

impl MediaMode {
    pub fn label(self) -> &'static str {
        match self {
            Self::AudioOnly => "Audio only",
            Self::VideoOnly => "Video only",
            Self::AudioVideo => "Audio + Video",
        }
    }

    fn as_config(self) -> &'static str {
        match self {
            Self::AudioOnly => "audio",
            Self::VideoOnly => "video",
            Self::AudioVideo => "both",
        }
    }

    fn from_config(value: &str) -> Self {
        match value {
            "audio" => Self::AudioOnly,
            "video" => Self::VideoOnly,
            _ => Self::AudioVideo,
        }
    }

    pub fn next(self, backwards: bool) -> Self {
        match (self, backwards) {
            (Self::AudioOnly, false) => Self::VideoOnly,
            (Self::VideoOnly, false) => Self::AudioVideo,
            (Self::AudioVideo, false) => Self::AudioOnly,
            (Self::AudioOnly, true) => Self::AudioVideo,
            (Self::VideoOnly, true) => Self::AudioOnly,
            (Self::AudioVideo, true) => Self::VideoOnly,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Settings {
    pub download_dir: String,
    pub download_playlist: bool,
    pub embed_cover: bool,
    pub media_mode: MediaMode,
    pub audio_quality: usize,
    pub video_quality: usize,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            download_dir: "~/Downloads/Coil".to_string(),
            download_playlist: false,
            embed_cover: false,
            media_mode: MediaMode::AudioVideo,
            audio_quality: 0,
            video_quality: 0,
        }
    }
}

impl Settings {
    pub fn load() -> Self {
        let mut settings = Self::default();
        let Ok(contents) = fs::read_to_string(config_path()) else {
            return settings;
        };

        for line in contents.lines() {
            let Some((key, value)) = line.split_once('=') else {
                continue;
            };
            match key.trim() {
                "download_dir" if !value.trim().is_empty() => {
                    settings.download_dir = value.trim().to_string();
                }
                "download_playlist" => {
                    settings.download_playlist = value.trim() == "true";
                }
                "embed_cover" => {
                    settings.embed_cover = value.trim() == "true";
                }
                "media_mode" => {
                    settings.media_mode = MediaMode::from_config(value.trim());
                }
                "audio_quality" => {
                    settings.audio_quality = value
                        .trim()
                        .parse::<usize>()
                        .unwrap_or(0)
                        .min(AUDIO_QUALITIES.len() - 1);
                }
                "video_quality" => {
                    settings.video_quality = value
                        .trim()
                        .parse::<usize>()
                        .unwrap_or(0)
                        .min(VIDEO_QUALITIES.len() - 1);
                }
                _ => {}
            }
        }

        settings
    }

    pub fn save(&self) -> io::Result<()> {
        let path = config_path();
        let parent = path.parent().ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "invalid Coil config path")
        })?;
        fs::create_dir_all(parent)?;

        let contents = format!(
            "download_dir={}\ndownload_playlist={}\nembed_cover={}\nmedia_mode={}\naudio_quality={}\nvideo_quality={}\n",
            self.download_dir.replace('\n', "").replace('\r', ""),
            self.download_playlist,
            self.embed_cover,
            self.media_mode.as_config(),
            self.audio_quality,
            self.video_quality,
        );

        let temp_path = path.with_extension(format!("tmp-{}", std::process::id()));
        let mut options = OpenOptions::new();
        options.write(true).create(true).truncate(true);

        // Keep the user's download path and preferences private on Unix systems.
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }

        let write_result = (|| {
            let mut file = options.open(&temp_path)?;
            file.write_all(contents.as_bytes())?;
            file.sync_all()?;
            fs::rename(&temp_path, &path)
        })();

        if write_result.is_err() {
            let _ = fs::remove_file(&temp_path);
        }
        write_result
    }

    pub fn expanded_download_dir(&self) -> PathBuf {
        let value = self.download_dir.trim();
        if value == "~" {
            return home_dir().unwrap_or_else(|| PathBuf::from(value));
        }
        if let Some(rest) = value.strip_prefix("~/") {
            if let Some(home) = home_dir() {
                return home.join(rest);
            }
        }
        PathBuf::from(value)
    }
}

fn config_path() -> PathBuf {
    if let Some(xdg) = env::var_os("XDG_CONFIG_HOME").filter(|v| !v.is_empty()) {
        return PathBuf::from(xdg).join("coil/config");
    }
    home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join(".config/coil/config")
}

fn home_dir() -> Option<PathBuf> {
    env::var_os("HOME").map(PathBuf::from)
}
