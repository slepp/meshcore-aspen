package main

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

var fixtureTime = time.Date(2026, 9, 27, 17, 30, 0, 0, time.UTC)

type transportFunc func(*http.Request) (*http.Response, error)

func (f transportFunc) RoundTrip(req *http.Request) (*http.Response, error) { return f(req) }

func fixtureWeather(t *testing.T, handler http.HandlerFunc) (*weatherProvider, *httptest.Server) {
	t.Helper()
	fixture := httptest.NewServer(handler)
	provider, err := newWeatherProvider(fixture.Client(), fixture.URL+"/geo", fixture.URL+"/forecast", func() time.Time { return fixtureTime })
	if err != nil {
		fixture.Close()
		t.Fatal(err)
	}
	return provider, fixture
}

func geoJSON() string {
	return `{"results":[{"id":12345,"name":"Fixture City","latitude":40.1,"longitude":-105.2,"elevation":1630,"feature_code":"PPL","country_code":"TL","timezone":"Etc/UTC","country_id":1,"country":"Testland","admin1":"Example State"}],"generationtime_ms":0.42}`
}

func forecastJSON() string {
	return `{"latitude":40.1,"longitude":-105.2,"generationtime_ms":0.21,"utc_offset_seconds":0,"timezone":"GMT","timezone_abbreviation":"GMT","elevation":1630,"current_units":{"time":"iso8601","interval":"seconds","temperature_2m":"°C","weather_code":"wmo code"},"current":{"time":"2026-09-27T17:15","interval":900,"temperature_2m":12.5,"weather_code":3}}`
}

func TestWeatherProviderSuccessAndNoIdentityLeak(t *testing.T) {
	const secret = "only-a-fixture-token-0123456789-abcdef"
	var calls atomic.Int32
	provider, fixture := fixtureWeather(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.Method != http.MethodGet || r.Header.Get("Authorization") != "" ||
			r.Header.Get("X-Local-Identity") != "" || strings.Contains(r.URL.String(), secret) {
			t.Errorf("unwanted provider request headers or method: %s %+v", r.Method, r.Header)
		}
		switch r.URL.Path {
		case "/geo":
			if r.URL.Query().Get("name") != "Fixture City" ||
				r.URL.Query().Get("count") != "1" || r.URL.Query().Get("format") != "json" {
				t.Errorf("unexpected geocoding query: %s", r.URL.RawQuery)
			}
			fmt.Fprint(w, geoJSON())
		case "/forecast":
			if r.URL.Query().Get("latitude") != "40.1" || r.URL.Query().Get("longitude") != "-105.2" ||
				r.URL.Query().Get("current") != "temperature_2m,weather_code" ||
				r.URL.Query().Get("timezone") != "UTC" {
				t.Errorf("unexpected forecast query: %s", r.URL.RawQuery)
			}
			fmt.Fprint(w, forecastJSON())
		default:
			t.Errorf("unexpected request path: %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer fixture.Close()
	host := httptest.NewServer(newRPCServer(secret, true, provider))
	defer host.Close()
	status, result, raw := rpcCall(t, host.Client(), host.URL,
		`{"operation":"weather","request_id":"weather-1","args":{"place":"Fixture City"}}`, secret)
	if status != 200 || !result.OK || calls.Load() != 2 ||
		string(raw) != `{"ok":true,"result":{"source":"open-meteo","location":"Fixture City","country":"Testland","temperature_c":12.5,"weather_code":3,"observed_at":"2026-09-27T17:15:00Z","source_age_seconds":900}}` {
		t.Fatalf("weather result: %d %s (%d calls)", status, raw, calls.Load())
	}
}

func TestWeatherProviderFailures(t *testing.T) {
	tests := []struct {
		name, geo, forecast       string
		geoStatus, forecastStatus int
		wantStatus                int
		wantCode                  string
	}{
		{"no results field", `{"generationtime_ms":0.23}`, "", 200, 200, 404, "place_not_found"},
		{"empty results", `{"results":[],"generationtime_ms":0.23}`, "", 200, 200, 404, "place_not_found"},
		{"null results", `{"results":null}`, "", 200, 200, 404, "place_not_found"},
		{"offline", "", "", 503, 200, 503, "provider_unavailable"},
		{"malformed geocoding", `{"results":{}}`, "", 200, 200, 502, "provider_bad_response"},
		{"large geocoding", strings.Repeat("a", maxProviderBytes+1), "", 200, 200, 502, "provider_bad_response"},
		{"bad location", `{"results":[{"name":"x","country":"y","latitude":91,"longitude":0}]}`, "", 200, 200, 502, "provider_bad_response"},
		{"geocoding rate limited", "", "", 429, 200, 503, "provider_rate_limited"},
		{"geocoding timed out", "", "", 408, 200, 504, "provider_timeout"},
		{"forecast down", geoJSON(), "", 200, 503, 503, "provider_unavailable"},
		{"forecast rate limited", geoJSON(), "", 200, 429, 503, "provider_rate_limited"},
		{"forecast timed out", geoJSON(), "", 200, 408, 504, "provider_timeout"},
		{"forecast missing fields", geoJSON(), `{"current":{}}`, 200, 200, 502, "provider_bad_response"},
		{"forecast stale", geoJSON(), strings.Replace(forecastJSON(), "17:15", "14:15", 1), 200, 200, 502, "provider_stale"},
		{"forecast too far ahead", geoJSON(), strings.Replace(forecastJSON(), "17:15", "17:40", 1), 200, 200, 502, "provider_stale"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			provider, fixture := fixtureWeather(t, func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/geo" {
					w.WriteHeader(tc.geoStatus)
					fmt.Fprint(w, tc.geo)
				} else {
					w.WriteHeader(tc.forecastStatus)
					fmt.Fprint(w, tc.forecast)
				}
			})
			defer fixture.Close()
			host := httptest.NewServer(newRPCServer("", true, provider))
			defer host.Close()
			status, result, _ := rpcCall(t, host.Client(), host.URL,
				`{"operation":"weather","request_id":"1","args":{"place":"Fixture City"}}`, "")
			if status != tc.wantStatus || result.Error == nil || result.Error.Code != tc.wantCode {
				t.Fatalf("unexpected response: %d %+v", status, result)
			}
		})
	}
}

func TestProviderTimeoutAndOffline(t *testing.T) {
	for _, tc := range []struct {
		name      string
		transport transportFunc
		code      string
		status    int
	}{
		{"timeout", func(req *http.Request) (*http.Response, error) {
			<-req.Context().Done()
			return nil, req.Context().Err()
		}, "provider_timeout", 504},
		{"offline", func(*http.Request) (*http.Response, error) {
			return nil, errors.New("fixture-only offline detail that must not leak")
		}, "provider_unavailable", 503},
	} {
		t.Run(tc.name, func(t *testing.T) {
			provider, err := newWeatherProvider(&http.Client{Transport: tc.transport},
				defaultGeocodeURL, defaultForecastURL, func() time.Time { return fixtureTime })
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
			defer cancel()
			_, status, failure := provider.lookup(ctx, "Fixture City")
			if status != tc.status || failure == nil || failure.Code != tc.code ||
				strings.Contains(failure.Message, "fixture-only") {
				t.Fatalf("%s: status %d failure %+v", tc.name, status, failure)
			}
		})
	}
}

func TestProviderHTTPTimeoutAndOfflineFixtures(t *testing.T) {
	timeoutProvider, timeoutFixture := fixtureWeather(t, func(w http.ResponseWriter, r *http.Request) {
		<-r.Context().Done()
	})
	defer timeoutFixture.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	_, status, failure := timeoutProvider.lookup(ctx, "Fixture City")
	if status != http.StatusGatewayTimeout || failure == nil || failure.Code != "provider_timeout" {
		t.Fatalf("HTTP timeout: %d %+v", status, failure)
	}

	bodyProvider, bodyFixture := fixtureWeather(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		w.(http.Flusher).Flush()
		<-r.Context().Done()
	})
	defer bodyFixture.Close()
	bodyCtx, bodyCancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer bodyCancel()
	_, status, failure = bodyProvider.lookup(bodyCtx, "Fixture City")
	if status != http.StatusGatewayTimeout || failure == nil || failure.Code != "provider_timeout" {
		t.Fatalf("HTTP body timeout: %d %+v", status, failure)
	}

	offlineProvider, offlineFixture := fixtureWeather(t, func(w http.ResponseWriter, r *http.Request) {
		t.Error("closed fixture must not receive a request")
	})
	offlineFixture.Close()
	_, status, failure = offlineProvider.lookup(context.Background(), "Fixture City")
	if status != http.StatusServiceUnavailable || failure == nil || failure.Code != "provider_unavailable" {
		t.Fatalf("HTTP offline: %d %+v", status, failure)
	}
}

func TestProviderRejectsRedirectAndUnsafeEndpoints(t *testing.T) {
	var redirected atomic.Int32
	fixture := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/geo" {
			http.Redirect(w, r, "/target", http.StatusTemporaryRedirect)
			return
		}
		redirected.Add(1)
		fmt.Fprint(w, geoJSON())
	}))
	defer fixture.Close()
	provider, err := newWeatherProvider(fixture.Client(), fixture.URL+"/geo", fixture.URL+"/forecast", func() time.Time { return fixtureTime })
	if err != nil {
		t.Fatal(err)
	}
	_, status, failure := provider.lookup(context.Background(), "Fixture City")
	if status != http.StatusBadGateway || failure == nil || failure.Code != "provider_bad_response" || redirected.Load() != 0 {
		t.Fatalf("redirect followed: status %d failure %+v count %d", status, failure, redirected.Load())
	}
	for _, raw := range []string{
		"http://example.org/v1/search", "http://192.168.1.2/v1/search",
		"https://user@example.org/search", "https://example.org/search?key=secret",
		"https://example.org/search#fragment", "http://localhost/search",
	} {
		if _, err := providerURL(raw); err == nil {
			t.Errorf("accepted unsafe provider URL: %s", raw)
		}
	}
}

func TestProviderFailureDoesNotExposeUpstreamBody(t *testing.T) {
	const upstreamSecret = "fixture-private-upstream-detail-0000"
	provider, fixture := fixtureWeather(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprint(w, upstreamSecret)
	})
	defer fixture.Close()
	host := httptest.NewServer(newRPCServer("", true, provider))
	defer host.Close()
	status, result, raw := rpcCall(t, host.Client(), host.URL,
		`{"operation":"weather","request_id":"1","args":{"place":"Fixture City"}}`, "")
	if status != http.StatusServiceUnavailable || result.Error == nil ||
		result.Error.Code != "provider_unavailable" || strings.Contains(string(raw), upstreamSecret) {
		t.Fatalf("upstream failure exposed details: %d %s", status, raw)
	}
}

func TestProviderIgnoresAmbientAndInjectedProxies(t *testing.T) {
	var proxyCalls atomic.Int32
	proxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		proxyCalls.Add(1)
		w.WriteHeader(http.StatusBadGateway)
	}))
	defer proxy.Close()
	t.Setenv("HTTP_PROXY", proxy.URL)
	t.Setenv("HTTPS_PROXY", proxy.URL)
	t.Setenv("http_proxy", proxy.URL)
	t.Setenv("https_proxy", proxy.URL)

	defaultProvider, err := newWeatherProvider(&http.Client{}, defaultGeocodeURL, defaultForecastURL, func() time.Time { return fixtureTime })
	if err != nil {
		t.Fatal(err)
	}
	if transport, ok := defaultProvider.client.Transport.(*http.Transport); !ok || transport.Proxy != nil {
		t.Fatal("default provider transport still uses an ambient proxy")
	}

	fixture := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Proxy-Authorization") != "" {
			t.Error("proxy credentials reached provider fixture")
		}
		switch r.URL.Path {
		case "/geo":
			fmt.Fprint(w, geoJSON())
		case "/forecast":
			fmt.Fprint(w, forecastJSON())
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer fixture.Close()
	proxyURL, err := url.Parse(proxy.URL)
	if err != nil {
		t.Fatal(err)
	}
	injected := fixture.Client().Transport.(*http.Transport).Clone()
	injected.Proxy = http.ProxyURL(proxyURL)
	provider, err := newWeatherProvider(&http.Client{Transport: injected},
		fixture.URL+"/geo", fixture.URL+"/forecast", func() time.Time { return fixtureTime })
	if err != nil {
		t.Fatal(err)
	}
	_, status, failure := provider.lookup(context.Background(), "Fixture City")
	if status != http.StatusOK || failure != nil || proxyCalls.Load() != 0 {
		t.Fatalf("provider used proxy: status %d failure %+v proxy calls %d", status, failure, proxyCalls.Load())
	}
	if injected.Proxy == nil {
		t.Fatal("provider mutated the injected client's transport")
	}
}
