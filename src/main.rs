mod download;
mod settings;

use crossterm::cursor::{Hide, MoveTo, Show};
use crossterm::event::{self, DisableBracketedPaste, EnableBracketedPaste, Event, KeyCode, KeyEvent, KeyModifiers};
use crossterm::style::{Attribute, Color, Print, ResetColor, SetAttribute, SetBackgroundColor, SetForegroundColor};
use crossterm::terminal::{self, Clear, ClearType, EnterAlternateScreen, LeaveAlternateScreen};
use crossterm::{execute, queue};
use download::DownloadJob;
use settings::{MediaMode, Settings, AUDIO_QUALITIES, VIDEO_QUALITIES};
use std::collections::VecDeque;
use std::io::{self, Write};
use std::time::Duration;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Page {
    Main,
    Settings,
}

struct App {
    page: Page,
    focus: usize,
    url: String,
    editing_url: bool,
    editing_path: bool,
    settings: Settings,
    job: Option<DownloadJob>,
    log: VecDeque<String>,
    message: String,
}

impl App {
    fn new(settings: Settings) -> Self {
        Self {
            page: Page::Main,
            focus: 0,
            url: String::new(),
            editing_url: false,
            editing_path: false,
            settings,
            job: None,
            log: VecDeque::new(),
            message: "Paste a link, then press Enter on the URL row to edit it.".to_string(),
        }
    }

    fn start_download(&mut self) {
        if self.job.is_some() {
            self.message = "A download is already running.".to_string();
            return;
        }

        match download::start(&self.url, &self.settings) {
            Ok(job) => {
                self.log.clear();
                self.job = Some(job);
                self.message = "Download started. Press q to cancel and quit.".to_string();
                let _ = self.settings.save();
            }
            Err(error) => self.message = error,
        }
    }

    fn switch_page(&mut self) {
        self.editing_url = false;
        self.editing_path = false;
        self.page = match self.page {
            Page::Main => Page::Settings,
            Page::Settings => Page::Main,
        };
        self.focus = 0;
    }

    fn move_focus(&mut self, down: bool) {
        let count = match self.page {
            Page::Main => 8,
            Page::Settings => 3,
        };
        self.focus = if down {
            (self.focus + 1) % count
        } else if self.focus == 0 {
            count - 1
        } else {
            self.focus - 1
        };
    }

    fn adjust(&mut self, backwards: bool) {
        match (self.page, self.focus) {
            (Page::Main, 1) => self.settings.download_playlist = !self.settings.download_playlist,
            (Page::Main, 2) => self.settings.embed_cover = !self.settings.embed_cover,
            (Page::Main, 3) => self.settings.media_mode = self.settings.media_mode.next(backwards),
            (Page::Main, 4) => {
                self.settings.audio_quality = cycle(
                    self.settings.audio_quality,
                    AUDIO_QUALITIES.len(),
                    backwards,
                );
            }
            (Page::Main, 5) => {
                self.settings.video_quality = cycle(
                    self.settings.video_quality,
                    VIDEO_QUALITIES.len(),
                    backwards,
                );
            }
            _ => {}
        }
    }

    fn activate(&mut self) {
        match self.page {
            Page::Main => match self.focus {
                0 => self.editing_url = !self.editing_url,
                1 => self.settings.download_playlist = !self.settings.download_playlist,
                2 => self.settings.embed_cover = !self.settings.embed_cover,
                3 => self.settings.media_mode = self.settings.media_mode.next(false),
                4 => {
                    self.settings.audio_quality = cycle(
                        self.settings.audio_quality,
                        AUDIO_QUALITIES.len(),
                        false,
                    );
                }
                5 => {
                    self.settings.video_quality = cycle(
                        self.settings.video_quality,
                        VIDEO_QUALITIES.len(),
                        false,
                    );
                }
                6 => self.start_download(),
                7 => self.switch_page(),
                _ => {}
            },
            Page::Settings => match self.focus {
                0 => self.editing_path = !self.editing_path,
                1 => match self.settings.save() {
                    Ok(()) => self.message = "Settings saved.".to_string(),
                    Err(error) => self.message = format!("Could not save settings: {error}"),
                },
                2 => self.switch_page(),
                _ => {}
            },
        }
    }

    fn handle_key(&mut self, key: KeyEvent) -> bool {
        if key.code == KeyCode::Tab && !key.modifiers.contains(KeyModifiers::CONTROL) {
            self.switch_page();
            return false;
        }

        if self.editing_url || self.editing_path {
            match key.code {
                KeyCode::Esc => {
                    self.editing_url = false;
                    self.editing_path = false;
                }
                KeyCode::Enter => {
                    self.editing_url = false;
                    self.editing_path = false;
                }
                KeyCode::Backspace => {
                    if self.editing_url {
                        self.url.pop();
                    } else {
                        self.settings.download_dir.pop();
                    }
                }
                KeyCode::Delete => {
                    if self.editing_url {
                        self.url.clear();
                    } else {
                        self.settings.download_dir.clear();
                    }
                }
                KeyCode::Char('u') if key.modifiers.contains(KeyModifiers::CONTROL) => {
                    if self.editing_url {
                        self.url.clear();
                    } else {
                        self.settings.download_dir.clear();
                    }
                }
                KeyCode::Char(character) if !key.modifiers.contains(KeyModifiers::CONTROL) => {
                    if self.editing_url {
                        self.url.push(character);
                    } else if !character.is_control() {
                        self.settings.download_dir.push(character);
                    }
                }
                _ => {}
            }
            return false;
        }

        match key.code {
            KeyCode::Char('q') => return true,
            KeyCode::Char('d') => self.start_download(),
            KeyCode::Char('s') => {
                self.page = Page::Settings;
                self.focus = 0;
            }
            KeyCode::Up => self.move_focus(false),
            KeyCode::Down => self.move_focus(true),
            KeyCode::Left => self.adjust(true),
            KeyCode::Right => self.adjust(false),
            KeyCode::Enter => self.activate(),
            KeyCode::Esc => {
                if self.page == Page::Settings {
                    self.switch_page();
                }
            }
            _ => {}
        }
        false
    }

    fn handle_paste(&mut self, pasted: String) {
        if self.editing_url || (self.page == Page::Main && self.focus == 0) {
            self.url.push_str(&pasted.replace('\n', "").replace('\r', ""));
            self.editing_url = true;
        } else if self.editing_path || (self.page == Page::Settings && self.focus == 0) {
            self.settings
                .download_dir
                .push_str(&pasted.replace('\n', "").replace('\r', ""));
            self.editing_path = true;
        }
    }
}

fn cycle(current: usize, length: usize, backwards: bool) -> usize {
    if backwards {
        if current == 0 {
            length - 1
        } else {
            current - 1
        }
    } else {
        (current + 1) % length
    }
}

struct TerminalGuard;

impl TerminalGuard {
    fn new() -> io::Result<Self> {
        terminal::enable_raw_mode()?;
        if let Err(error) = execute!(io::stdout(), EnterAlternateScreen, Hide, EnableBracketedPaste) {
            let _ = terminal::disable_raw_mode();
            return Err(error);
        }
        Ok(Self)
    }
}

impl Drop for TerminalGuard {
    fn drop(&mut self) {
        let _ = terminal::disable_raw_mode();
        let _ = execute!(io::stdout(), DisableBracketedPaste, Show, LeaveAlternateScreen);
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let settings = Settings::load();
    let mut app = App::new(settings);
    let _terminal = TerminalGuard::new()?;
    run(&mut app)?;
    let _ = app.settings.save();
    Ok(())
}

fn run(app: &mut App) -> io::Result<()> {
    loop {
        let poll_result = app.job.as_mut().map(|job| job.poll(&mut app.log));
        if let Some(result) = poll_result {
            match result {
                Ok(Some(status)) => {
                    app.message = if status.success() {
                        "Download completed.".to_string()
                    } else {
                        format!("yt-dlp exited with status: {status}")
                    };
                    app.job = None;
                }
                Ok(None) => {}
                Err(error) => {
                    app.message = format!("Could not read download process: {error}");
                    app.job = None;
                }
            }
        }

        draw(app)?;

        if event::poll(Duration::from_millis(100))? {
            match event::read()? {
                Event::Key(key) => {
                    if app.handle_key(key) {
                        break;
                    }
                }
                Event::Paste(text) => app.handle_paste(text),
                _ => {}
            }
        }
    }
    Ok(())
}

fn draw(app: &App) -> io::Result<()> {
    let mut out = io::stdout();
    let (width, height) = terminal::size().unwrap_or((80, 24));
    let width = width.max(36);

    queue!(out, Clear(ClearType::All))?;
    line(&mut out, 0, width, " COIL  ·  minimal yt-dlp TUI ", Color::Cyan, true, false)?;
    let tabs = match app.page {
        Page::Main => " [ Main ]     Settings ",
        Page::Settings => "   Main     [ Settings ] ",
    };
    line(&mut out, 2, width, tabs, Color::White, true, false)?;

    match app.page {
        Page::Main => draw_main(app, &mut out, width, height)?,
        Page::Settings => draw_settings(app, &mut out, width, height)?,
    }

    line(
        &mut out,
        height.saturating_sub(2),
        width,
        &format!(" {}", app.message),
        Color::Yellow,
        false,
        false,
    )?;
    line(
        &mut out,
        height.saturating_sub(1),
        width,
        " Tab page  ↑/↓ select  ←/→ change  Enter edit/select  d download  q quit ",
        Color::DarkGrey,
        false,
        false,
    )?;

    out.flush()
}

fn draw_main<W: Write>(app: &App, out: &mut W, width: u16, height: u16) -> io::Result<()> {
    let url_label = if app.editing_url { "URL [editing]" } else { "URL [Enter to edit]" };
    let url_value = if app.url.is_empty() { "paste a video / playlist link" } else { &app.url };
    let rows = [
        format!("{url_label:<23} {url_value}"),
        format!("Download playlist      {}", on_off(app.settings.download_playlist)),
        format!("Embed cover            {}", on_off(app.settings.embed_cover)),
        format!("Media                  {}", app.settings.media_mode.label()),
        format!(
            "Audio quality          {}",
            AUDIO_QUALITIES[app.settings.audio_quality.min(AUDIO_QUALITIES.len() - 1)]
        ),
        format!(
            "Video quality          {}",
            VIDEO_QUALITIES[app.settings.video_quality.min(VIDEO_QUALITIES.len() - 1)]
        ),
        "▶  Start download".to_string(),
        "⚙  Settings".to_string(),
    ];

    for (index, row) in rows.iter().enumerate() {
        line(
            out,
            4 + index as u16,
            width,
            &format!(" {row}"),
            if index == 6 { Color::Green } else { Color::White },
            app.focus == index,
            false,
        )?;
    }

    let log_y = 14;
    line(
        out,
        log_y,
        width,
        if app.job.is_some() {
            " Download output · active"
        } else {
            " Recent output"
        },
        Color::Cyan,
        true,
        false,
    )?;

    let visible = height.saturating_sub(log_y + 4).min(7) as usize;
    let start = app.log.len().saturating_sub(visible);
    for (offset, entry) in app.log.iter().skip(start).enumerate() {
        line(
            out,
            log_y + 1 + offset as u16,
            width,
            &format!(" {}", clean_terminal_text(entry)),
            Color::DarkGrey,
            false,
            false,
        )?;
    }
    Ok(())
}

fn draw_settings<W: Write>(app: &App, out: &mut W, width: u16, _height: u16) -> io::Result<()> {
    let path_label = if app.editing_path {
        "Download folder [editing]"
    } else {
        "Download folder [Enter to edit]"
    };
    let rows = [
        format!("{path_label}: {}", app.settings.download_dir),
        "Save settings".to_string(),
        "Back to main".to_string(),
    ];

    for (index, row) in rows.iter().enumerate() {
        line(
            out,
            4 + index as u16,
            width,
            &format!(" {row}"),
            if index == 1 { Color::Green } else { Color::White },
            app.focus == index,
            false,
        )?;
    }

    line(
        out,
        10,
        width,
        " Coil expands ~/ and creates the folder when a download starts.",
        Color::DarkGrey,
        false,
        false,
    )?;
    Ok(())
}

fn line<W: Write>(
    out: &mut W,
    y: u16,
    width: u16,
    text: &str,
    color: Color,
    bold: bool,
    selected: bool,
) -> io::Result<()> {
    let content = truncate(text, width as usize);
    queue!(
        out,
        MoveTo(0, y),
        SetForegroundColor(color),
        SetAttribute(if bold { Attribute::Bold } else { Attribute::Reset }),
    )?;
    if selected {
        queue!(out, SetBackgroundColor(Color::DarkGrey))?;
    }
    queue!(out, Print(content), ResetColor, SetAttribute(Attribute::Reset), Clear(ClearType::UntilNewLine))?;
    Ok(())
}

fn truncate(value: &str, max_chars: usize) -> String {
    value.chars().take(max_chars.saturating_sub(1)).collect()
}

fn clean_terminal_text(value: &str) -> String {
    value
        .chars()
        .filter(|character| !character.is_control())
        .collect()
}

fn on_off(value: bool) -> &'static str {
    if value { "ON" } else { "OFF" }
}
