package main

import "testing"

func TestValidateSoundCloudURL(t *testing.T) {
	valid := []string{
		"https://soundcloud.com/artist/track",
		"https://soundcloud.com/artist/sets/album",
		"https://on.soundcloud.com/abc123",
		"https://api-v2.soundcloud.com/tracks/123",
	}
	for _, value := range valid {
		if err := validateSoundCloudURL(value); err != nil {
			t.Errorf("expected %q to be accepted: %v", value, err)
		}
	}
	invalid := []string{
		"soundcloud.com/artist/track",
		"file:///etc/passwd",
		"https://example.com/audio",
		"https://soundcloud.com.evil.example/track",
	}
	for _, value := range invalid {
		if err := validateSoundCloudURL(value); err == nil {
			t.Errorf("expected %q to be rejected", value)
		}
	}
}

func TestProxyArgs(t *testing.T) {
	service := &Service{}
	if args, err := service.proxyArgs(false, ""); err != nil || len(args) != 0 {
		t.Fatalf("disabled proxy should not add arguments: args=%v err=%v", args, err)
	}
	args, err := service.proxyArgs(true, "socks5h://127.0.0.1:9050")
	if err != nil || len(args) != 2 || args[0] != "--proxy" {
		t.Fatalf("valid SOCKS proxy rejected: args=%v err=%v", args, err)
	}
	if _, err := service.proxyArgs(true, "127.0.0.1:9050"); err == nil {
		t.Fatal("proxy without a scheme should be rejected")
	}
}

func TestTrackFromMap(t *testing.T) {
	track, ok := trackFromMap(map[string]any{
		"id": "123", "title": "Track title", "uploader": "Artist",
		"webpage_url": "https://soundcloud.com/artist/track", "duration": float64(91),
	})
	if !ok {
		t.Fatal("expected a playlist entry to be parsed")
	}
	if track.Title != "Track title" || track.Artist != "Artist" || track.Duration != 91 {
		t.Fatalf("unexpected parsed metadata: %#v", track)
	}
	if track.URL != "https://soundcloud.com/artist/track" {
		t.Fatalf("unexpected track URL: %s", track.URL)
	}
}
