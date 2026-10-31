// SoundFetch backend: a small JSON-lines service that coordinates yt-dlp.
// The extractor remains replaceable; the UI talks only to this stable protocol.
package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

const defaultWorkers = 3

// Request is one command from the C++ frontend.
type Request struct {
	Cmd          string  `json:"cmd"`
	RequestID    string  `json:"requestId,omitempty"`
	JobID        string  `json:"jobId,omitempty"`
	URL          string  `json:"url,omitempty"`
	Items        []Track `json:"items,omitempty"`
	Quality      string  `json:"quality,omitempty"`
	OutputDir    string  `json:"outputDir,omitempty"`
	ProxyEnabled bool    `json:"proxyEnabled,omitempty"`
	ProxyURL     string  `json:"proxyUrl,omitempty"`
	Concurrency  int     `json:"concurrency,omitempty"`
}

type Track struct {
	ID        string  `json:"id,omitempty"`
	URL       string  `json:"url"`
	Title     string  `json:"title,omitempty"`
	Artist    string  `json:"artist,omitempty"`
	Duration  float64 `json:"duration,omitempty"`
	Thumbnail string  `json:"thumbnail,omitempty"`
}

type Event map[string]any

type Service struct {
	outMu  sync.Mutex
	jobsMu sync.Mutex
	jobs   map[string]context.CancelFunc
	ytdlp  string
}

func NewService() (*Service, error) {
	binary := strings.TrimSpace(os.Getenv("YTDLP"))
	if binary == "" {
		binary = "yt-dlp"
	}
	path, err := exec.LookPath(binary)
	if err != nil {
		return nil, fmt.Errorf("yt-dlp was not found; install yt-dlp and make it available in PATH")
	}
	return &Service{jobs: make(map[string]context.CancelFunc), ytdlp: path}, nil
}

func (s *Service) emit(event Event) {
	s.outMu.Lock()
	defer s.outMu.Unlock()
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(event)
}

func (s *Service) run() error {
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 64*1024), 8*1024*1024)
	for scanner.Scan() {
		line := bytes.TrimSpace(scanner.Bytes())
		if len(line) == 0 {
			continue
		}
		var req Request
		if err := json.Unmarshal(line, &req); err != nil {
			s.emit(Event{"event": "protocol_error", "message": "Invalid request JSON: " + err.Error()})
			continue
		}
		switch req.Cmd {
		case "fetch":
			go s.fetch(req)
		case "download":
			go s.download(req)
		case "cancel":
			s.cancel(req.JobID)
		case "shutdown":
			s.cancelAll()
			return nil
		default:
			s.emit(Event{"event": "protocol_error", "message": "Unknown command"})
		}
	}
	s.cancelAll()
	return scanner.Err()
}

func (s *Service) cancel(id string) {
	s.jobsMu.Lock()
	cancel := s.jobs[id]
	s.jobsMu.Unlock()
	if cancel != nil {
		cancel()
	}
}

func (s *Service) cancelAll() {
	s.jobsMu.Lock()
	cancels := make([]context.CancelFunc, 0, len(s.jobs))
	for _, cancel := range s.jobs {
		cancels = append(cancels, cancel)
	}
	s.jobsMu.Unlock()
	for _, cancel := range cancels {
		cancel()
	}
}

func (s *Service) registerJob(id string, cancel context.CancelFunc) {
	s.jobsMu.Lock()
	s.jobs[id] = cancel
	s.jobsMu.Unlock()
}

func (s *Service) unregisterJob(id string) {
	s.jobsMu.Lock()
	delete(s.jobs, id)
	s.jobsMu.Unlock()
}

func (s *Service) proxyArgs(enabled bool, raw string) ([]string, error) {
	if !enabled {
		return nil, nil
	}
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || u == nil {
		return nil, errors.New("proxy must be a complete URL, such as socks5h://127.0.0.1:9050")
	}
	if u.Host == "" || strings.ContainsAny(u.Host, "\r\n \t") {
		return nil, errors.New("proxy must be a complete URL, such as socks5h://127.0.0.1:9050")
	}
	switch strings.ToLower(u.Scheme) {
	case "http", "https", "socks4", "socks4a", "socks5", "socks5h":
	default:
		return nil, errors.New("unsupported proxy scheme; use http, https, socks4, socks4a, socks5 or socks5h")
	}
	return []string{"--proxy", u.String()}, nil
}

func validateSoundCloudURL(raw string) error {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || u.Host == "" || (u.Scheme != "https" && u.Scheme != "http") {
		return errors.New("paste a complete http(s) SoundCloud URL")
	}
	host := strings.ToLower(u.Hostname())
	if !(host == "soundcloud.com" || strings.HasSuffix(host, ".soundcloud.com") || host == "sndcdn.com" || strings.HasSuffix(host, ".sndcdn.com")) {
		return errors.New("only SoundCloud links are supported")
	}
	return nil
}

func (s *Service) fetch(req Request) {
	base := Event{"requestId": req.RequestID}
	if err := validateSoundCloudURL(req.URL); err != nil {
		base["event"] = "fetch_error"
		base["message"] = err.Error()
		s.emit(base)
		return
	}
	proxy, err := s.proxyArgs(req.ProxyEnabled, req.ProxyURL)
	if err != nil {
		base["event"], base["message"] = "fetch_error", err.Error()
		s.emit(base)
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Minute)
	defer cancel()
	args := []string{"--dump-single-json", "--no-warnings", "--no-call-home", "--socket-timeout", "20", "--skip-download", "--flat-playlist", "--yes-playlist", "--no-progress"}
	args = append(args, proxy...)
	args = append(args, req.URL)
	cmd := exec.CommandContext(ctx, s.ytdlp, args...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	stdout, runErr := cmd.Output()
	if runErr != nil {
		message := strings.TrimSpace(stderr.String())
		if message == "" {
			message = runErr.Error()
		}
		base["event"], base["message"] = "fetch_error", concise(message)
		s.emit(base)
		return
	}
	var raw map[string]any
	if err := json.Unmarshal(stdout, &raw); err != nil {
		base["event"], base["message"] = "fetch_error", "Could not read playlist metadata returned by yt-dlp: "+err.Error()
		s.emit(base)
		return
	}

	tracks := make([]Track, 0, 32)
	if entries, ok := raw["entries"].([]any); ok {
		for _, value := range entries {
			entry, ok := value.(map[string]any)
			if !ok {
				continue
			}
			if track, ok := trackFromMap(entry); ok {
				tracks = append(tracks, track)
			}
		}
	} else if track, ok := trackFromMap(raw); ok {
		tracks = append(tracks, track)
	}
	if len(tracks) == 0 {
		base["event"], base["message"] = "fetch_error", "No downloadable public tracks were found at this URL."
		s.emit(base)
		return
	}
	playlistTitle := stringField(raw, "title")
	if playlistTitle == "" {
		playlistTitle = tracks[0].Title
	}
	base["event"] = "playlist"
	base["title"] = playlistTitle
	base["uploader"] = firstNonEmpty(stringField(raw, "uploader"), stringField(raw, "channel"))
	base["trackCount"] = len(tracks)
	base["tracks"] = tracks
	s.emit(base)
}

func trackFromMap(m map[string]any) (Track, bool) {
	t := Track{
		ID:        stringField(m, "id"),
		Title:     firstNonEmpty(stringField(m, "title"), stringField(m, "fulltitle"), "Untitled track"),
		Artist:    firstNonEmpty(stringField(m, "artist"), firstString(m["artists"]), stringField(m, "creator"), stringField(m, "uploader"), stringField(m, "channel")),
		Thumbnail: stringField(m, "thumbnail"),
	}
	if value, ok := m["duration"].(float64); ok {
		t.Duration = value
	}
	for _, key := range []string{"webpage_url", "original_url", "url"} {
		candidate := stringField(m, key)
		if strings.HasPrefix(candidate, "https://") || strings.HasPrefix(candidate, "http://") {
			t.URL = candidate
			break
		}
	}
	if t.URL == "" && t.ID != "" {
		t.URL = "https://api.soundcloud.com/tracks/" + url.PathEscape(t.ID)
	}
	if t.URL == "" {
		return Track{}, false
	}
	return t, true
}

func stringField(m map[string]any, key string) string {
	value, _ := m[key].(string)
	return strings.TrimSpace(value)
}

func firstString(value any) string {
	if values, ok := value.([]any); ok {
		for _, item := range values {
			if candidate, ok := item.(string); ok && strings.TrimSpace(candidate) != "" {
				return strings.TrimSpace(candidate)
			}
		}
	}
	return ""
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return strings.TrimSpace(value)
		}
	}
	return ""
}

func concise(message string) string {
	message = strings.TrimSpace(message)
	if len(message) > 700 {
		message = message[len(message)-700:]
	}
	return message
}

func (s *Service) download(req Request) {
	if req.JobID == "" {
		req.JobID = strconv.FormatInt(time.Now().UnixNano(), 36)
	}
	if len(req.Items) == 0 {
		s.emit(Event{"event": "download_error", "jobId": req.JobID, "message": "There are no tracks selected."})
		return
	}
	outputDir := strings.TrimSpace(req.OutputDir)
	if outputDir == "" {
		home, _ := os.UserHomeDir()
		outputDir = filepath.Join(home, "Music", "SoundFetch")
	}
	if err := os.MkdirAll(outputDir, 0o755); err != nil {
		s.emit(Event{"event": "download_error", "jobId": req.JobID, "message": "Cannot create output folder: " + err.Error()})
		return
	}
	proxy, err := s.proxyArgs(req.ProxyEnabled, req.ProxyURL)
	if err != nil {
		s.emit(Event{"event": "download_error", "jobId": req.JobID, "message": err.Error()})
		return
	}
	workers := req.Concurrency
	if workers < 1 || workers > 5 {
		workers = defaultWorkers
	}
	ctx, cancel := context.WithCancel(context.Background())
	s.registerJob(req.JobID, cancel)
	defer func() {
		cancel()
		s.unregisterJob(req.JobID)
	}()

	s.emit(Event{"event": "download_started", "jobId": req.JobID, "total": len(req.Items), "message": fmt.Sprintf("Downloading %d track(s)…", len(req.Items))})
	jobs := make(chan Track)
	var wg sync.WaitGroup
	var resultMu sync.Mutex
	completed, failed, cancelled := 0, 0, 0
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for track := range jobs {
				if ctx.Err() != nil {
					resultMu.Lock()
					cancelled++
					resultMu.Unlock()
					continue
				}
				if err := validateSoundCloudURL(track.URL); err != nil {
					resultMu.Lock()
					failed++
					resultMu.Unlock()
					s.emit(Event{"event": "track_error", "jobId": req.JobID, "trackId": track.ID, "title": track.Title, "message": err.Error()})
					continue
				}
				if s.downloadOne(ctx, req.JobID, track, outputDir, req.Quality, proxy) {
					resultMu.Lock()
					completed++
					resultMu.Unlock()
				} else {
					resultMu.Lock()
					if ctx.Err() != nil {
						cancelled++
					} else {
						failed++
					}
					resultMu.Unlock()
				}
			}
		}()
	}
	skipped := 0
sendLoop:
	for i, track := range req.Items {
		select {
		case jobs <- track:
		case <-ctx.Done():
			skipped = len(req.Items) - i
			break sendLoop
		}
	}
	close(jobs)
	wg.Wait()
	resultMu.Lock()
	c, f, x := completed, failed, cancelled+skipped
	resultMu.Unlock()
	s.emit(Event{"event": "download_finished", "jobId": req.JobID, "completed": c, "failed": f, "cancelled": x, "total": len(req.Items)})
}

func (s *Service) downloadOne(ctx context.Context, jobID string, track Track, outputDir, quality string, proxy []string) bool {
	args := []string{
		"--no-warnings", "--no-call-home", "--newline", "--no-color", "--no-playlist",
		"--socket-timeout", "20", "--retries", "3", "--fragment-retries", "3", "--extractor-retries", "3", "--file-access-retries", "2",
		"--progress", "--progress-delta", "1",
		"--progress-template", "download:SF_PROGRESS:%(progress._percent_str)s|%(progress._speed_str)s|%(progress._eta_str)s",
		"--print", "after_move:SF_DONE:%(filepath)s",
		"--embed-metadata", "--embed-thumbnail", "--convert-thumbnails", "jpg",
		"--output", filepath.Join(outputDir, "%(artist,uploader,creator)s - %(title)s [%(id)s].%(ext)s"),
	}
	quality = strings.ToLower(strings.TrimSpace(quality))
	switch quality {
	case "mp3-320", "mp3-256", "mp3-192", "mp3-128":
		bitrate := strings.TrimPrefix(quality, "mp3-") + "K"
		args = append(args, "--extract-audio", "--audio-format", "mp3", "--audio-quality", bitrate)
	default:
		// Preserve the best source stream instead of re-encoding it.
		args = append(args, "--format", "bestaudio/best", "--extract-audio", "--audio-format", "best")
	}
	args = append(args, proxy...)
	args = append(args, track.URL)
	cmd := exec.CommandContext(ctx, s.ytdlp, args...)
	writer := &lineWriter{onLine: func(line string) {
		if strings.HasPrefix(line, "SF_PROGRESS:") {
			parts := strings.SplitN(strings.TrimPrefix(line, "SF_PROGRESS:"), "|", 3)
			percent := 0.0
			if len(parts) > 0 {
				p := strings.TrimSpace(strings.TrimSuffix(parts[0], "%"))
				percent, _ = strconv.ParseFloat(p, 64)
			}
			speed, eta := "", ""
			if len(parts) > 1 {
				speed = strings.TrimSpace(parts[1])
			}
			if len(parts) > 2 {
				eta = strings.TrimSpace(parts[2])
			}
			s.emit(Event{"event": "progress", "jobId": jobID, "trackId": track.ID, "title": track.Title, "percent": percent, "speed": speed, "eta": eta})
		} else if strings.HasPrefix(line, "SF_DONE:") {
			s.emit(Event{"event": "track_saved", "jobId": jobID, "trackId": track.ID, "title": track.Title, "path": strings.TrimSpace(strings.TrimPrefix(line, "SF_DONE:"))})
		}
	}}
	cmd.Stdout = writer
	cmd.Stderr = writer
	s.emit(Event{"event": "track_started", "jobId": jobID, "trackId": track.ID, "title": track.Title, "artist": track.Artist})
	err := cmd.Run()
	writer.Flush()
	if err != nil {
		if ctx.Err() != nil {
			s.emit(Event{"event": "track_cancelled", "jobId": jobID, "trackId": track.ID, "title": track.Title})
			return false
		}
		message := writer.Tail()
		if message == "" {
			message = err.Error()
		}
		s.emit(Event{"event": "track_error", "jobId": jobID, "trackId": track.ID, "title": track.Title, "message": concise(message)})
		return false
	}
	return true
}

// lineWriter safely merges yt-dlp stdout/stderr, emitting complete progress lines.
type lineWriter struct {
	mu     sync.Mutex
	buf    []byte
	tail   []byte
	onLine func(string)
}

func (w *lineWriter) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.tail = append(w.tail, p...)
	if len(w.tail) > 4096 {
		w.tail = append([]byte(nil), w.tail[len(w.tail)-4096:]...)
	}
	w.buf = append(w.buf, p...)
	for {
		i := bytes.IndexByte(w.buf, '\n')
		if i < 0 {
			break
		}
		line := strings.TrimSpace(string(w.buf[:i]))
		w.buf = append(w.buf[:0], w.buf[i+1:]...)
		if line != "" && w.onLine != nil {
			w.onLine(line)
		}
	}
	return len(p), nil
}

func (w *lineWriter) Flush() {
	w.mu.Lock()
	defer w.mu.Unlock()
	line := strings.TrimSpace(string(w.buf))
	w.buf = nil
	if line != "" && w.onLine != nil {
		w.onLine(line)
	}
}

func (w *lineWriter) Tail() string {
	w.mu.Lock()
	defer w.mu.Unlock()
	return strings.TrimSpace(string(w.tail))
}

func main() {
	service, err := NewService()
	if err != nil {
		_ = json.NewEncoder(os.Stdout).Encode(Event{"event": "fatal", "message": err.Error()})
		os.Exit(1)
	}
	if err := service.run(); err != nil && !errors.Is(err, io.EOF) {
		service.emit(Event{"event": "fatal", "message": err.Error()})
		os.Exit(1)
	}
}
