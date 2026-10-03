package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"time"
)

const (
	defaultGeocodeURL  = "https://geocoding-api.open-meteo.com/v1/search"
	defaultForecastURL = "https://api.open-meteo.com/v1/forecast"
	maxProviderBytes   = 16 << 10
	providerTimeout    = 2 * time.Second
	maxWeatherAge      = 2 * time.Hour
	maxFutureTime      = 5 * time.Minute
)

type weatherProvider struct {
	client   *http.Client
	geocode  *url.URL
	forecast *url.URL
	now      func() time.Time
}

type weatherResult struct {
	Source           string  `json:"source"`
	Location         string  `json:"location"`
	Country          string  `json:"country"`
	TemperatureC     float64 `json:"temperature_c"`
	WeatherCode      int     `json:"weather_code"`
	ObservedAt       string  `json:"observed_at"`
	SourceAgeSeconds int64   `json:"source_age_seconds"`
}

func newWeatherProvider(client *http.Client, geocodeURL, forecastURL string, now func() time.Time) (*weatherProvider, error) {
	geocode, err := providerURL(geocodeURL)
	if err != nil {
		return nil, fmt.Errorf("geocode-url: %w", err)
	}
	forecast, err := providerURL(forecastURL)
	if err != nil {
		return nil, fmt.Errorf("forecast-url: %w", err)
	}
	if client == nil || now == nil {
		return nil, errors.New("weather provider requires an HTTP client and clock")
	}
	copyClient := *client
	if client.Transport == nil {
		direct := http.DefaultTransport.(*http.Transport).Clone()
		direct.Proxy = nil
		copyClient.Transport = direct
	} else if transport, ok := client.Transport.(*http.Transport); ok {
		direct := transport.Clone()
		direct.Proxy = nil
		copyClient.Transport = direct
	}
	copyClient.Timeout = providerTimeout
	copyClient.Jar = nil
	copyClient.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return &weatherProvider{client: &copyClient, geocode: geocode, forecast: forecast, now: now}, nil
}

func providerURL(raw string) (*url.URL, error) {
	u, err := url.Parse(raw)
	if err != nil || u == nil || u.Hostname() == "" || u.Path == "" || u.User != nil ||
		u.Fragment != "" || u.RawQuery != "" || u.Opaque != "" {
		return nil, errors.New("expected an HTTPS endpoint URL without credentials, query, or fragment")
	}
	if u.Scheme != "https" {
		ip := net.ParseIP(u.Hostname())
		if u.Scheme != "http" || ip == nil || !ip.IsLoopback() {
			return nil, errors.New("provider endpoints must use HTTPS (HTTP is allowed only for loopback fixtures)")
		}
	}
	return u, nil
}

func (p *weatherProvider) lookup(ctx context.Context, place string) (weatherResult, int, *rpcError) {
	geoURL := *p.geocode
	query := geoURL.Query()
	query.Set("name", place)
	query.Set("count", "1")
	query.Set("language", "en")
	query.Set("format", "json")
	geoURL.RawQuery = query.Encode()
	body, status, failure := p.get(ctx, &geoURL)
	if failure != nil {
		return weatherResult{}, status, failure
	}
	var locations struct {
		Results json.RawMessage `json:"results"`
	}
	if json.Unmarshal(body, &locations) != nil {
		return weatherResult{}, http.StatusBadGateway, &rpcError{"provider_bad_response", "invalid geocoding response"}
	}
	if len(locations.Results) == 0 || string(locations.Results) == "null" {
		return weatherResult{}, http.StatusNotFound, &rpcError{"place_not_found", "place not found"}
	}
	var results []struct {
		Name      string   `json:"name"`
		Country   string   `json:"country"`
		Latitude  *float64 `json:"latitude"`
		Longitude *float64 `json:"longitude"`
	}
	if json.Unmarshal(locations.Results, &results) != nil {
		return weatherResult{}, http.StatusBadGateway, &rpcError{"provider_bad_response", "invalid geocoding response"}
	}
	if len(results) == 0 {
		return weatherResult{}, http.StatusNotFound, &rpcError{"place_not_found", "place not found"}
	}
	location := results[0]
	if !validText(location.Name, maxPlaceBytes) || !validText(location.Country, maxPlaceBytes) ||
		location.Latitude == nil || location.Longitude == nil ||
		math.IsNaN(*location.Latitude) || math.IsNaN(*location.Longitude) ||
		*location.Latitude < -90 || *location.Latitude > 90 ||
		*location.Longitude < -180 || *location.Longitude > 180 {
		return weatherResult{}, http.StatusBadGateway, &rpcError{"provider_bad_response", "invalid geocoding response"}
	}
	forecastURL := *p.forecast
	query = forecastURL.Query()
	query.Set("latitude", fmt.Sprintf("%g", *location.Latitude))
	query.Set("longitude", fmt.Sprintf("%g", *location.Longitude))
	query.Set("current", "temperature_2m,weather_code")
	query.Set("temperature_unit", "celsius")
	query.Set("timezone", "UTC")
	query.Set("forecast_days", "1")
	forecastURL.RawQuery = query.Encode()
	body, status, failure = p.get(ctx, &forecastURL)
	if failure != nil {
		return weatherResult{}, status, failure
	}
	var forecast struct {
		CurrentUnits struct {
			Temperature string `json:"temperature_2m"`
		} `json:"current_units"`
		Current struct {
			Time        string   `json:"time"`
			Temperature *float64 `json:"temperature_2m"`
			WeatherCode *int     `json:"weather_code"`
		} `json:"current"`
	}
	if json.Unmarshal(body, &forecast) != nil || forecast.CurrentUnits.Temperature != "°C" ||
		forecast.Current.Temperature == nil || forecast.Current.WeatherCode == nil ||
		*forecast.Current.WeatherCode < 0 || *forecast.Current.WeatherCode > 99 {
		return weatherResult{}, http.StatusBadGateway, &rpcError{"provider_bad_response", "invalid forecast response"}
	}
	observed, err := time.ParseInLocation("2006-01-02T15:04", forecast.Current.Time, time.UTC)
	if err != nil {
		return weatherResult{}, http.StatusBadGateway, &rpcError{"provider_bad_response", "invalid forecast timestamp"}
	}
	age := p.now().UTC().Sub(observed)
	if age > maxWeatherAge || age < -maxFutureTime {
		return weatherResult{}, http.StatusBadGateway, &rpcError{"provider_stale", "forecast observation is outside the allowed time window"}
	}
	return weatherResult{
		Source:           "open-meteo",
		Location:         location.Name,
		Country:          location.Country,
		TemperatureC:     *forecast.Current.Temperature,
		WeatherCode:      *forecast.Current.WeatherCode,
		ObservedAt:       observed.Format(time.RFC3339),
		SourceAgeSeconds: max(0, int64(age.Seconds())),
	}, http.StatusOK, nil
}

func (p *weatherProvider) get(ctx context.Context, endpoint *url.URL) ([]byte, int, *rpcError) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return nil, http.StatusBadGateway, &rpcError{"provider_bad_response", "invalid provider request"}
	}
	response, err := p.client.Do(req)
	if err != nil {
		status, failure := providerFailure(ctx, err)
		return nil, status, failure
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		if response.StatusCode == http.StatusTooManyRequests {
			return nil, http.StatusServiceUnavailable, &rpcError{"provider_rate_limited", "weather provider is rate limiting requests"}
		}
		if response.StatusCode == http.StatusRequestTimeout {
			return nil, http.StatusGatewayTimeout, &rpcError{"provider_timeout", "weather provider timed out"}
		}
		if response.StatusCode >= 500 {
			return nil, http.StatusServiceUnavailable, &rpcError{"provider_unavailable", "weather provider is unavailable"}
		}
		return nil, http.StatusBadGateway, &rpcError{"provider_bad_response", "weather provider returned an unexpected status"}
	}
	body, err := io.ReadAll(io.LimitReader(response.Body, maxProviderBytes+1))
	if err != nil {
		status, failure := providerFailure(ctx, err)
		return nil, status, failure
	}
	if len(body) > maxProviderBytes {
		return nil, http.StatusBadGateway, &rpcError{"provider_bad_response", "weather provider response is too large"}
	}
	return body, http.StatusOK, nil
}

func providerFailure(ctx context.Context, err error) (int, *rpcError) {
	var netErr net.Error
	if errors.Is(ctx.Err(), context.DeadlineExceeded) || errors.Is(err, context.DeadlineExceeded) ||
		(errors.As(err, &netErr) && netErr.Timeout()) {
		return http.StatusGatewayTimeout, &rpcError{"provider_timeout", "weather provider timed out"}
	}
	if errors.Is(ctx.Err(), context.Canceled) || errors.Is(err, context.Canceled) {
		return http.StatusServiceUnavailable, &rpcError{"provider_interrupted", "weather request was interrupted"}
	}
	return http.StatusServiceUnavailable, &rpcError{"provider_unavailable", "weather provider is unavailable"}
}
