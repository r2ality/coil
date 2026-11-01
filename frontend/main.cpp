#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr int kWindowWidth = 1020;
constexpr int kWindowHeight = 760;
const Fl_Color kBackground = fl_rgb_color(20, 21, 25);
const Fl_Color kPanel = fl_rgb_color(29, 31, 37);
const Fl_Color kText = fl_rgb_color(235, 237, 242);
const Fl_Color kMuted = fl_rgb_color(157, 163, 174);
const Fl_Color kAccent = fl_rgb_color(255, 85, 35);

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string configDirectory() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return (fs::path(xdg) / "soundfetch").string();
    if (const char* home = std::getenv("HOME"); home && *home)
        return (fs::path(home) / ".config" / "soundfetch").string();
    return (fs::current_path() / ".soundfetch").string();
}

std::string defaultMusicDirectory() {
    if (const char* home = std::getenv("HOME"); home && *home)
        return (fs::path(home) / "Music" / "SoundFetch").string();
    return (fs::current_path() / "SoundFetch").string();
}

std::string durationText(double seconds) {
    if (seconds <= 0) return "";
    const auto total = static_cast<long long>(seconds);
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto secs = total % 60;
    char buffer[32];
    if (hours > 0)
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", hours, minutes, secs);
    else
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes, secs);
    return buffer;
}

class BackendProcess {
public:
    using EventHandler = std::function<void(json)>;

    explicit BackendProcess(EventHandler handler) : handler_(std::move(handler)) {}
    BackendProcess(const BackendProcess&) = delete;
    BackendProcess& operator=(const BackendProcess&) = delete;

    bool start() {
        int toChild[2] = {-1, -1};
        int fromChild[2] = {-1, -1};
        if (::pipe(toChild) != 0 || ::pipe(fromChild) != 0) {
            closePipes(toChild, fromChild);
            return false;
        }

        const pid_t child = ::fork();
        if (child < 0) {
            closePipes(toChild, fromChild);
            return false;
        }
        if (child == 0) {
            ::dup2(toChild[0], STDIN_FILENO);
            ::dup2(fromChild[1], STDOUT_FILENO);
            ::close(toChild[0]); ::close(toChild[1]);
            ::close(fromChild[0]); ::close(fromChild[1]);
            const std::string executable = backendPath();
            if (!executable.empty()) {
                char* const args[] = {const_cast<char*>(executable.c_str()), nullptr};
                ::execv(executable.c_str(), args);
            }
            char* const fallbackArgs[] = {const_cast<char*>("soundfetch-backend"), nullptr};
            ::execvp("soundfetch-backend", fallbackArgs);
            const char* message = "SoundFetch: could not launch soundfetch-backend\n";
            (void)::write(STDOUT_FILENO, message, std::char_traits<char>::length(message));
            _exit(127);
        }

        pid_ = child;
        ::close(toChild[0]);
        ::close(fromChild[1]);
        inputFd_ = toChild[1];
        outputFd_ = fromChild[0];
        reader_ = std::thread([this] { readLoop(); });
        return true;
    }

    bool send(const json& request) {
        const std::string payload = request.dump() + "\n";
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (inputFd_ < 0) return false;
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const ssize_t written = ::write(inputFd_, payload.data() + offset, payload.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    ~BackendProcess() {
        if (inputFd_ >= 0) {
            send({{"cmd", "shutdown"}});
            ::close(inputFd_);
            inputFd_ = -1;
        }
        if (reader_.joinable()) reader_.join();
        if (pid_ > 0) {
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
        if (outputFd_ >= 0) ::close(outputFd_);
    }

private:
    static void closePipes(int (&a)[2], int (&b)[2]) {
        for (int fd : a) if (fd >= 0) ::close(fd);
        for (int fd : b) if (fd >= 0) ::close(fd);
    }

    static std::string backendPath() {
        if (const char* configured = std::getenv("SOUNDFETCH_BACKEND"); configured && *configured)
            return configured;
        char executable[4096];
        const auto length = ::readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length > 0) {
            executable[length] = '\0';
            const fs::path candidate = fs::path(executable).parent_path() / "soundfetch-backend";
            if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
        }
        return {};
    }

    void readLoop() {
        std::string pending;
        char buffer[8192];
        while (true) {
            const ssize_t count = ::read(outputFd_, buffer, sizeof(buffer));
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                break;
            }
            pending.append(buffer, static_cast<std::size_t>(count));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = trim(pending.substr(0, newline));
                pending.erase(0, newline + 1);
                if (line.empty()) continue;
                try {
                    if (handler_) handler_(json::parse(line));
                } catch (const std::exception& error) {
                    if (handler_) handler_(json{{"event", "protocol_error"}, {"message", error.what()}});
                }
            }
            if (pending.size() > 4 * 1024 * 1024) pending.clear();
        }
    }

    EventHandler handler_;
    pid_t pid_ = -1;
    int inputFd_ = -1;
    int outputFd_ = -1;
    std::mutex writeMutex_;
    std::thread reader_;
};

struct TrackItem {
    std::string id;
    std::string url;
    std::string title;
    std::string artist;
    std::string thumbnail;
    double duration = 0;
    Fl_Check_Button* check = nullptr;
};

class SoundFetchApp {
public:
    SoundFetchApp() : configPath_(fs::path(configDirectory()) / "settings.json") {
        loadSettings();
        buildUI();
        backend_ = std::make_unique<BackendProcess>([this](json event) {
            std::lock_guard<std::mutex> lock(eventMutex_);
            events_.push_back(std::move(event));
        });
        if (!backend_->start()) {
            log("Could not start the Go backend. Check that soundfetch-backend is installed beside the GUI.");
            status_->label("Backend failed to start");
        } else {
            log("Ready. Paste a public SoundCloud track or playlist URL.");
        }
        Fl::add_timeout(0.10, timerCallback, this);
    }

    ~SoundFetchApp() {
        Fl::remove_timeout(timerCallback, this);
        backend_.reset();
        // The text display references its buffer, so destroy the window first.
        delete window_;
        window_ = nullptr;
        delete logBuffer_;
        logBuffer_ = nullptr;
    }

    int run() {
        window_->show();
        return Fl::run();
    }

private:
    static void timerCallback(void* data) {
        auto* app = static_cast<SoundFetchApp*>(data);
        app->processEvents();
        Fl::repeat_timeout(0.10, timerCallback, data);
    }

    static void fetchCallback(Fl_Widget*, void* data) {
        static_cast<SoundFetchApp*>(data)->fetch();
    }
    static void selectedCallback(Fl_Widget*, void* data) {
        static_cast<SoundFetchApp*>(data)->startDownload(false);
    }
    static void allCallback(Fl_Widget*, void* data) {
        static_cast<SoundFetchApp*>(data)->startDownload(true);
    }
    static void cancelCallback(Fl_Widget*, void* data) {
        static_cast<SoundFetchApp*>(data)->cancelDownload();
    }
    static void browseCallback(Fl_Widget*, void* data) {
        static_cast<SoundFetchApp*>(data)->browseFolder();
    }
    static void saveSettingsCallback(Fl_Widget*, void* data) {
        static_cast<SoundFetchApp*>(data)->saveSettingsFromUI();
    }

    void buildUI() {
        Fl::scheme("gtk+");
        Fl::background(20, 21, 25);
        Fl::background2(34, 36, 43);
        Fl::foreground(235, 237, 242);
        Fl::set_color(FL_SELECTION_COLOR, kAccent);

        window_ = new Fl_Window(kWindowWidth, kWindowHeight, "SoundFetch");
        window_->color(kBackground);
        window_->begin();

        auto* brand = new Fl_Box(24, 16, 360, 38, "SOUNDFETCH");
        brand->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        brand->labelfont(FL_BOLD);
        brand->labelsize(25);
        brand->labelcolor(kAccent);

        auto* tagline = new Fl_Box(385, 20, 570, 30, "A lightweight SoundCloud downloader");
        tagline->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        tagline->labelcolor(kMuted);
        tagline->labelsize(14);

        tabs_ = new Fl_Tabs(18, 64, kWindowWidth - 36, kWindowHeight - 84);
        tabs_->selection_color(kPanel);
        tabs_->begin();

        mainTab_ = new Fl_Group(18, 91, kWindowWidth - 36, kWindowHeight - 112, "Main");
        mainTab_->color(kBackground);
        mainTab_->begin();

        auto* linkLabel = new Fl_Box(40, 112, 300, 24, "PASTE SOUNDCLOUD LINK");
        leftLabel(linkLabel);
        urlInput_ = new Fl_Input(40, 139, 760, 38);
        urlInput_->tooltip("A public SoundCloud track, playlist, album or artist tracks URL");
        styleInput(urlInput_);
        fetchButton_ = new Fl_Button(814, 139, 164, 38, "Fetch tracks");
        styleButton(fetchButton_, true);
        fetchButton_->callback(fetchCallback, this);

        auto* qualityLabel = new Fl_Box(40, 192, 300, 22, "AUDIO QUALITY");
        leftLabel(qualityLabel);
        qualityChoice_ = new Fl_Choice(40, 218, 300, 36);
        qualityChoice_->add("Best available (keep source)");
        qualityChoice_->add("MP3 - 320 kbps");
        qualityChoice_->add("MP3 - 256 kbps");
        qualityChoice_->add("MP3 - 192 kbps");
        qualityChoice_->add("MP3 - 128 kbps");
        qualityChoice_->value(0);
        styleChoice(qualityChoice_);

        auto* listTitle = new Fl_Box(40, 270, 550, 24, "TRACKS");
        leftLabel(listTitle);
        trackCount_ = new Fl_Box(740, 270, 238, 24, "No tracks loaded");
        trackCount_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        trackCount_->labelcolor(kMuted);
        trackCount_->labelsize(12);

        listScroll_ = new Fl_Scroll(40, 297, 938, 250);
        listScroll_->box(FL_DOWN_FRAME);
        listScroll_->color(kPanel);
        listScroll_->begin();
        auto* empty = new Fl_Box(56, 309, 900, 32, "Paste a SoundCloud link and select Fetch tracks.");
        empty->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        empty->labelcolor(kMuted);
        empty->labelsize(13);
        listScroll_->end();

        selectedButton_ = new Fl_Button(40, 565, 205, 40, "Download selected");
        styleButton(selectedButton_, true);
        selectedButton_->callback(selectedCallback, this);
        allButton_ = new Fl_Button(257, 565, 180, 40, "Download all");
        styleButton(allButton_, false);
        allButton_->callback(allCallback, this);
        cancelButton_ = new Fl_Button(449, 565, 145, 40, "Cancel");
        styleButton(cancelButton_, false);
        cancelButton_->callback(cancelCallback, this);
        cancelButton_->deactivate();

        progress_ = new Fl_Progress(40, 620, 938, 18);
        progress_->minimum(0);
        progress_->maximum(100);
        progress_->value(0);
        progress_->color(kPanel);
        progress_->selection_color(kAccent);
        progress_->box(FL_DOWN_FRAME);

        status_ = new Fl_Box(40, 644, 938, 24, "Idle");
        status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        status_->labelcolor(kMuted);
        status_->labelsize(12);

        logBuffer_ = new Fl_Text_Buffer();
        logOutput_ = new Fl_Text_Display(40, 674, 938, 64);
        logOutput_->buffer(logBuffer_);
        logOutput_->color(kPanel);
        logOutput_->textcolor(kText);
        logOutput_->textsize(11);
        logOutput_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        logOutput_->scrollbar_width(10);
        logOutput_->box(FL_DOWN_FRAME);
        logOutput_->show_cursor(0);

        mainTab_->end();

        settingsTab_ = new Fl_Group(18, 91, kWindowWidth - 36, kWindowHeight - 112, "Settings");
        settingsTab_->color(kBackground);
        settingsTab_->begin();

        auto* settingsTitle = new Fl_Box(40, 112, 500, 28, "DOWNLOAD SETTINGS");
        settingsTitle->labelfont(FL_BOLD);
        settingsTitle->labelsize(17);
        settingsTitle->labelcolor(kText);
        settingsTitle->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        proxyToggle_ = new Fl_Check_Button(40, 163, 280, 30, "Use proxy");
        proxyToggle_->labelcolor(kText);
        proxyToggle_->labelsize(14);
        proxyToggle_->value(proxyEnabled_ ? 1 : 0);

        auto* proxyLabel = new Fl_Box(40, 201, 260, 22, "PROXY URL");
        leftLabel(proxyLabel);
        proxyInput_ = new Fl_Input(40, 226, 650, 36);
        styleInput(proxyInput_);
        proxyInput_->value(proxyURL_.c_str());
        proxyInput_->tooltip("Example: socks5h://127.0.0.1:9050");

        auto* pathLabel = new Fl_Box(40, 291, 500, 22, "DOWNLOAD FOLDER");
        leftLabel(pathLabel);
        pathInput_ = new Fl_Input(40, 316, 790, 36);
        styleInput(pathInput_);
        pathInput_->value(outputDir_.c_str());
        browseButton_ = new Fl_Button(844, 316, 134, 36, "Browse…");
        styleButton(browseButton_, false);
        browseButton_->callback(browseCallback, this);

        auto* note = new Fl_Box(40, 382, 900, 68,
            "SoundFetch uses yt-dlp for SoundCloud extraction and FFmpeg for tagging / format conversion.\n"
            "Proxy settings affect network requests only; they do not unlock private, blocked or paid tracks.");
        note->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_TOP);
        note->labelcolor(kMuted);
        note->labelsize(13);

        saveSettingsButton_ = new Fl_Button(40, 478, 180, 40, "Save settings");
        styleButton(saveSettingsButton_, true);
        saveSettingsButton_->callback(saveSettingsCallback, this);
        settingsTab_->end();

        tabs_->end();
        window_->end();
        window_->resizable(tabs_);
        mainTab_->resizable(listScroll_);
        window_->size_range(850, 650);
    }

    static void leftLabel(Fl_Box* box) {
        box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        box->labelcolor(kMuted);
        box->labelfont(FL_BOLD);
        box->labelsize(11);
    }

    static void styleInput(Fl_Input* widget) {
        widget->color(kPanel);
        widget->selection_color(kAccent);
        widget->labelcolor(kText);
        widget->textsize(14);
    }

    static void styleChoice(Fl_Choice* widget) {
        widget->color(kPanel);
        widget->selection_color(kAccent);
        widget->labelcolor(kText);
        widget->textsize(14);
    }

    static void styleButton(Fl_Button* button, bool primary) {
        button->color(primary ? kAccent : fl_rgb_color(48, 51, 60));
        button->selection_color(primary ? fl_rgb_color(225, 70, 25) : fl_rgb_color(67, 71, 82));
        button->labelcolor(FL_WHITE);
        button->labelfont(FL_BOLD);
        button->labelsize(13);
        button->box(FL_ROUND_UP_BOX);
    }

    void loadSettings() {
        outputDir_ = defaultMusicDirectory();
        try {
            std::ifstream input(configPath_);
            if (!input) return;
            json settings;
            input >> settings;
            outputDir_ = settings.value("outputDir", outputDir_);
            proxyEnabled_ = settings.value("proxyEnabled", false);
            proxyURL_ = settings.value("proxyUrl", std::string{});
        } catch (...) {
            // Keep defaults when an old or incomplete settings file cannot be read.
        }
    }

    void saveSettingsFromUI() {
        proxyEnabled_ = proxyToggle_->value() != 0;
        proxyURL_ = trim(proxyInput_->value());
        outputDir_ = trim(pathInput_->value());
        if (outputDir_.empty()) outputDir_ = defaultMusicDirectory();
        std::error_code ec;
        fs::create_directories(configPath_.parent_path(), ec);
        if (!ec) fs::permissions(configPath_.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
        std::ofstream output(configPath_);
        if (!output) {
            log("Could not write settings file: " + configPath_.string());
            return;
        }
        output << json{{"outputDir", outputDir_}, {"proxyEnabled", proxyEnabled_}, {"proxyUrl", proxyURL_}}.dump(2);
        output.flush();
        output.close();
        fs::permissions(configPath_, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        pathInput_->value(outputDir_.c_str());
        log("Settings saved.");
        status_->label("Settings saved");
    }

    void browseFolder() {
        Fl_Native_File_Chooser chooser;
        chooser.title("Choose SoundFetch download folder");
        chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
        chooser.directory(pathInput_->value());
        if (chooser.show() == 0 && chooser.filename())
            pathInput_->value(chooser.filename());
    }

    json settingsPayload() {
        proxyEnabled_ = proxyToggle_->value() != 0;
        proxyURL_ = trim(proxyInput_->value());
        outputDir_ = trim(pathInput_->value());
        if (outputDir_.empty()) outputDir_ = defaultMusicDirectory();
        return {{"proxyEnabled", proxyEnabled_}, {"proxyUrl", proxyURL_}, {"outputDir", outputDir_}};
    }

    void fetch() {
        if (!backend_) {
            log("The backend is not running.");
            return;
        }
        const std::string url = trim(urlInput_->value());
        if (url.empty()) {
            status_->label("Paste a SoundCloud link first");
            return;
        }
        json request = settingsPayload();
        request["cmd"] = "fetch";
        request["requestId"] = "fetch-" + std::to_string(++requestCounter_);
        request["url"] = url;
        currentFetchId_ = request["requestId"].get<std::string>();
        fetchButton_->deactivate();
        status_->label("Fetching track information…");
        log("Fetching " + url);
        if (!backend_->send(request)) {
            fetchButton_->activate();
            log("Could not send the request to the Go backend.");
        }
    }

    void startDownload(bool all) {
        if (!backend_) return;
        std::vector<TrackItem> chosen;
        for (const auto& track : tracks_) {
            if (all || (track.check && track.check->value())) chosen.push_back(track);
        }
        if (chosen.empty()) {
            status_->label("Select at least one track to download");
            return;
        }
        json request = settingsPayload();
        request["cmd"] = "download";
        activeJobId_ = "job-" + std::to_string(++jobCounter_);
        request["jobId"] = activeJobId_;
        request["quality"] = selectedQuality();
        request["concurrency"] = 3;
        request["items"] = json::array();
        for (const auto& track : chosen) {
            request["items"].push_back({
                {"id", track.id}, {"url", track.url}, {"title", track.title},
                {"artist", track.artist}, {"thumbnail", track.thumbnail}, {"duration", track.duration}
            });
        }
        activeTotal_ = static_cast<int>(chosen.size());
        activeCompleted_ = 0;
        progress_->value(0);
        selectedButton_->deactivate();
        allButton_->deactivate();
        cancelButton_->activate();
        status_->copy_label(("Starting " + std::to_string(activeTotal_) + " download(s)…").c_str());
        log("Download queue started: " + std::to_string(activeTotal_) + " track(s).");
        if (!backend_->send(request)) {
            selectedButton_->activate();
            allButton_->activate();
            cancelButton_->deactivate();
            log("Could not send the download request.");
        }
    }

    std::string selectedQuality() const {
        switch (qualityChoice_->value()) {
            case 1: return "mp3-320";
            case 2: return "mp3-256";
            case 3: return "mp3-192";
            case 4: return "mp3-128";
            default: return "source";
        }
    }

    void cancelDownload() {
        if (!backend_ || activeJobId_.empty()) return;
        backend_->send({{"cmd", "cancel"}, {"jobId", activeJobId_}});
        status_->label("Cancelling downloads…");
        log("Cancellation requested.");
        cancelButton_->deactivate();
    }

    void clearTrackRows() {
        listScroll_->clear();
        tracks_.clear();
    }

    void showTracks(const json& event) {
        clearTrackRows();
        const auto list = event.value("tracks", json::array());
        int y = 306;
        for (const auto& entry : list) {
            TrackItem track;
            track.id = entry.value("id", std::string{});
            track.url = entry.value("url", std::string{});
            track.title = trim(entry.value("title", std::string{}));
            if (track.title.empty()) {
                track.title = track.id.empty() ? "Unknown track" : "Track " + track.id;
            }
            track.artist = trim(entry.value("artist", std::string{}));
            track.thumbnail = entry.value("thumbnail", std::string{});
            track.duration = entry.value("duration", 0.0);
            const std::string duration = durationText(track.duration);
            track.check = new Fl_Check_Button(50, y + 3, 28, 28);
            track.check->value(1);
            track.check->labelcolor(kText);
            track.check->color(kPanel);
            std::string label = track.title;
            if (!track.artist.empty()) label += "  —  " + track.artist;
            if (!duration.empty()) label += "   ·   " + duration;
            // FLTK widget labels are not copied by the constructor. The old
            // local std::string died at the end of this loop iteration, leaving
            // a dangling pointer that rendered as random glyphs in the track list.
            auto* text = new Fl_Box(82, y, 865, 34);
            text->copy_label(label.c_str());
            text->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            text->labelcolor(kText);
            text->labelsize(13);
            if (tracks_.size() % 2 == 1) text->color(fl_rgb_color(33, 35, 42));
            listScroll_->add(track.check);
            listScroll_->add(text);
            tracks_.push_back(std::move(track));
            y += 38;
        }
        if (tracks_.empty()) {
            auto* empty = new Fl_Box(56, 309, 900, 32, "No tracks were returned for this link.");
            empty->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            empty->labelcolor(kMuted);
            listScroll_->add(empty);
        }
        listScroll_->redraw();
        const std::string title = event.value("title", std::string{"SoundCloud list"});
        const std::string count = std::to_string(tracks_.size()) + (tracks_.size() == 1 ? " track" : " tracks");
        trackCount_->copy_label((title + "  ·  " + count).c_str());
        status_->label("Tracks loaded. Choose quality and download.");
        fetchButton_->activate();
        log("Loaded " + count + ": " + title);
    }

    void log(const std::string& message) {
        logs_ += message + "\n";
        if (logs_.size() > 7000) {
            const std::size_t trimAt = logs_.size() - 6000;
            const std::size_t nextLine = logs_.find('\n', trimAt);
            logs_.erase(0, nextLine == std::string::npos ? trimAt : nextLine + 1);
        }
        if (logBuffer_ && logOutput_) {
            logBuffer_->text(logs_.c_str());
            // Keep the newest event visible while retaining a real scrollbar
            // so users can scroll back through previous activity.
            const int lastLine = logOutput_->count_lines(0, logBuffer_->length(), false);
            const int lineHeight = std::max(1, static_cast<int>(logOutput_->textsize()) + 2);
            const int visibleLines = std::max(1, (logOutput_->h() - 4) / lineHeight);
            logOutput_->scroll(std::max(0, lastLine - visibleLines + 1), 0);
            logOutput_->insert_position(logBuffer_->length());
            logOutput_->redraw();
        }
    }

    void processEvents() {
        std::vector<json> pending;
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            pending.swap(events_);
        }
        for (const auto& event : pending) handleEvent(event);
    }

    void handleEvent(const json& event) {
        const std::string type = event.value("event", std::string{});
        if (type == "playlist") {
            showTracks(event);
        } else if (type == "fetch_error") {
            fetchButton_->activate();
            const std::string message = event.value("message", std::string{"Could not fetch link"});
            status_->copy_label(message.c_str());
            log("Fetch failed: " + message);
        } else if (type == "fatal") {
            const std::string message = event.value("message", std::string{"Backend error"});
            status_->copy_label(message.c_str());
            log("Backend: " + message);
        } else if (type == "download_started") {
            const std::string message = event.value("message", std::string{"Download queue started"});
            status_->copy_label(message.c_str());
            log(message);
        } else if (type == "track_started") {
            const std::string title = event.value("title", std::string{"Unknown track"});
            const std::string artist = event.value("artist", std::string{});
            const std::string displayName = artist.empty() ? title : title + " — " + artist;
            status_->copy_label(("Downloading: " + displayName).c_str());
            log("Downloading: " + displayName);
        } else if (type == "progress") {
            const double percent = std::clamp(event.value("percent", 0.0), 0.0, 100.0);
            progress_->value(static_cast<float>(percent));
            const std::string title = event.value("title", std::string{"Track"});
            const std::string speed = event.value("speed", std::string{});
            const std::string eta = event.value("eta", std::string{});
            std::string label = title + " — " + std::to_string(static_cast<int>(std::round(percent))) + "%";
            if (!speed.empty() && speed != "NA") label += " · " + speed;
            if (!eta.empty() && eta != "NA") label += " · ETA " + eta;
            status_->copy_label(label.c_str());
        } else if (type == "track_saved") {
            const std::string title = event.value("title", std::string{"Track"});
            const std::string path = event.value("path", std::string{});
            log("Saved: " + title + (path.empty() ? std::string{} : " → " + path));
        } else if (type == "track_error") {
            const std::string title = event.value("title", std::string{"Track"});
            const std::string message = event.value("message", std::string{"Download failed"});
            log("Failed: " + title + " — " + message);
        } else if (type == "track_cancelled") {
            log("Cancelled: " + event.value("title", std::string{"Track"}));
        } else if (type == "download_finished") {
            const int done = event.value("completed", 0);
            const int failed = event.value("failed", 0);
            const int cancelled = event.value("cancelled", 0);
            selectedButton_->activate();
            allButton_->activate();
            cancelButton_->deactivate();
            progress_->value(100);
            status_->copy_label(("Finished — " + std::to_string(done) + " saved, " + std::to_string(failed) + " failed, " + std::to_string(cancelled) + " cancelled").c_str());
            log("Queue finished: " + std::to_string(done) + " saved, " + std::to_string(failed) + " failed, " + std::to_string(cancelled) + " cancelled.");
            activeJobId_.clear();
        } else if (type == "download_error") {
            selectedButton_->activate();
            allButton_->activate();
            cancelButton_->deactivate();
            const std::string message = event.value("message", std::string{"Download failed"});
            status_->copy_label(message.c_str());
            log("Download error: " + message);
        } else if (type == "protocol_error") {
            log("Protocol: " + event.value("message", std::string{"Unknown error"}));
        }
    }

    Fl_Window* window_ = nullptr;
    Fl_Tabs* tabs_ = nullptr;
    Fl_Group* mainTab_ = nullptr;
    Fl_Group* settingsTab_ = nullptr;
    Fl_Input* urlInput_ = nullptr;
    Fl_Choice* qualityChoice_ = nullptr;
    Fl_Button* fetchButton_ = nullptr;
    Fl_Button* selectedButton_ = nullptr;
    Fl_Button* allButton_ = nullptr;
    Fl_Button* cancelButton_ = nullptr;
    Fl_Scroll* listScroll_ = nullptr;
    Fl_Box* trackCount_ = nullptr;
    Fl_Progress* progress_ = nullptr;
    Fl_Box* status_ = nullptr;
    Fl_Text_Display* logOutput_ = nullptr;
    Fl_Text_Buffer* logBuffer_ = nullptr;
    Fl_Check_Button* proxyToggle_ = nullptr;
    Fl_Input* proxyInput_ = nullptr;
    Fl_Input* pathInput_ = nullptr;
    Fl_Button* browseButton_ = nullptr;
    Fl_Button* saveSettingsButton_ = nullptr;

    std::unique_ptr<BackendProcess> backend_;
    std::vector<TrackItem> tracks_;
    std::mutex eventMutex_;
    std::vector<json> events_;
    std::string logs_;
    fs::path configPath_;
    std::string outputDir_;
    std::string proxyURL_;
    std::string currentFetchId_;
    std::string activeJobId_;
    bool proxyEnabled_ = false;
    int requestCounter_ = 0;
    int jobCounter_ = 0;
    int activeTotal_ = 0;
    int activeCompleted_ = 0;
};
} // namespace

int main() {
    SoundFetchApp app;
    return app.run();
}
