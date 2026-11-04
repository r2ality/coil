use crate::settings::{MediaMode, Settings, AUDIO_QUALITIES, VIDEO_QUALITIES};
use std::collections::VecDeque;
use std::io::{self, BufRead, BufReader};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::thread::{self, JoinHandle};

const OUTPUT_TEMPLATE: &str = "%(title).180B [%(id)s].%(ext)s";
const PROGRESS_TEMPLATE: &str = "download:COIL_PROGRESS|%(progress._percent_str)s|%(progress._speed_str)s|%(progress._eta_str)s|%(info.title)s";

pub struct DownloadJob {
    child: Child,
    lines: Receiver<String>,
    readers: Vec<JoinHandle<()>>,
    finished: bool,
}

impl DownloadJob {
    pub fn poll(&mut self, log: &mut VecDeque<String>) -> io::Result<Option<ExitStatus>> {
        while let Ok(line) = self.lines.try_recv() {
            if !line.trim().is_empty() {
                log.push_back(line);
                while log.len() > 7 {
                    log.pop_front();
                }
            }
        }

        if self.finished {
            return Ok(None);
        }

        if let Some(status) = self.child.try_wait()? {
            self.finished = true;
            for reader in self.readers.drain(..) {
                let _ = reader.join();
            }
            while let Ok(line) = self.lines.try_recv() {
                if !line.trim().is_empty() {
                    log.push_back(line);
                    while log.len() > 7 {
                        log.pop_front();
                    }
                }
            }
            return Ok(Some(status));
        }

        Ok(None)
    }
}

impl Drop for DownloadJob {
    fn drop(&mut self) {
        if !self.finished {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

pub fn start(url: &str, settings: &Settings) -> Result<DownloadJob, String> {
    let url = url.trim();
    if url.is_empty() {
        return Err("Paste a URL first.".to_string());
    }
    if url.contains('\0') {
        return Err("The URL contains an invalid character.".to_string());
    }

    let output_dir = settings.expanded_download_dir();
    std::fs::create_dir_all(&output_dir)
        .map_err(|error| format!("Cannot create download folder: {error}"))?;

    let mut command = Command::new("yt-dlp");
    command
        .arg("--no-color")
        .arg("--newline")
        .arg("--progress")
        .arg("--progress-delta")
        .arg("1")
        .arg("--progress-template")
        .arg(PROGRESS_TEMPLATE)
        .arg("--concurrent-fragments")
        .arg("4")
        .arg("--retries")
        .arg("5")
        .arg("--fragment-retries")
        .arg("5")
        .arg("--file-access-retries")
        .arg("3")
        .arg("--extractor-retries")
        .arg("3")
        .arg("--socket-timeout")
        .arg("20")
        .arg("--continue")
        .arg("--no-overwrites")
        .arg("--paths")
        .arg(output_dir)
        .arg("--output")
        .arg(OUTPUT_TEMPLATE);

    if settings.download_playlist {
        command.arg("--yes-playlist").arg("--ignore-errors");
    } else {
        command.arg("--no-playlist");
    }

    if settings.embed_cover {
        // Keep a standalone image and also embed it where FFmpeg supports it.
        command.arg("--write-thumbnail").arg("--embed-thumbnail");
    }

    match settings.media_mode {
        MediaMode::AudioOnly => {
            let quality = audio_quality_arg(settings.audio_quality);
            command
                .arg("--format")
                .arg("bestaudio/best")
                .arg("--extract-audio")
                .arg("--audio-format")
                .arg("mp3")
                .arg("--audio-quality")
                .arg(quality);
        }
        MediaMode::VideoOnly => {
            command.arg("--format").arg(video_only_format(settings.video_quality));
        }
        MediaMode::AudioVideo => {
            command.arg("--format").arg(audio_video_format(settings.video_quality));
        }
    }

    // The end-of-options marker and Command::arg keep arbitrary URLs out of a shell.
    command
        .arg("--")
        .arg(url)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());

    let mut child = command
        .spawn()
        .map_err(|error| format!("Could not start yt-dlp: {error}. Is it installed and on PATH?"))?;

    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| "Could not capture yt-dlp output.".to_string())?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| "Could not capture yt-dlp errors.".to_string())?;

    let (sender, receiver) = mpsc::channel();
    let stdout_reader = spawn_reader(BufReader::new(stdout), sender.clone());
    let stderr_reader = spawn_reader(BufReader::new(stderr), sender);

    Ok(DownloadJob {
        child,
        lines: receiver,
        readers: vec![stdout_reader, stderr_reader],
        finished: false,
    })
}

fn spawn_reader<R>(reader: BufReader<R>, sender: mpsc::Sender<String>) -> JoinHandle<()>
where
    R: std::io::Read + Send + 'static,
{
    thread::spawn(move || {
        for line in reader.lines() {
            match line {
                Ok(line) => {
                    if sender.send(line).is_err() {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
    })
}

fn audio_quality_arg(index: usize) -> &'static str {
    match AUDIO_QUALITIES[index.min(AUDIO_QUALITIES.len() - 1)] {
        "320 kbps" => "320K",
        "256 kbps" => "256K",
        "192 kbps" => "192K",
        "128 kbps" => "128K",
        _ => "0",
    }
}

fn selected_height(index: usize) -> Option<&'static str> {
    match VIDEO_QUALITIES[index.min(VIDEO_QUALITIES.len() - 1)] {
        "2160p" => Some("2160"),
        "1440p" => Some("1440"),
        "1080p" => Some("1080"),
        "720p" => Some("720"),
        "480p" => Some("480"),
        "360p" => Some("360"),
        _ => None,
    }
}

fn video_only_format(index: usize) -> String {
    match selected_height(index) {
        Some(height) => format!("bv[height<={height}]"),
        None => "bv".to_string(),
    }
}

fn audio_video_format(index: usize) -> String {
    match selected_height(index) {
        Some(height) => format!("bv*+ba/b[height<={height}]"),
        None => "bv*+ba/b".to_string(),
    }
}
