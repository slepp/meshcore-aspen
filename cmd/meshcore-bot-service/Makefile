GO ?= go
ROOT := ../..
TMPDIR := $(abspath $(ROOT)/.tmp)
BIN := $(CURDIR)/bin/meshcore-bot-service
LISTEN ?= 127.0.0.1:8787
WEATHER ?= false
COMPUTE ?= false
TLS_CERT ?=
TLS_KEY ?=
SERVICE_DIRECTORY ?=
UNIT := $(HOME)/.config/systemd/user/meshcore-bot-service.service

.PHONY: build test run service-install service-status

build:
	mkdir -p bin "$(TMPDIR)"
	cd $(ROOT) && TMPDIR="$(TMPDIR)" $(GO) build -trimpath -buildvcs=false -o "$(BIN)" ./cmd/meshcore-bot-service

test:
	mkdir -p "$(TMPDIR)"
	cd $(ROOT) && TMPDIR="$(TMPDIR)" $(GO) test -race ./cmd/meshcore-bot-service

run: build
	"$(BIN)" -listen "$(LISTEN)" -weather=$(WEATHER) -compute=$(COMPUTE) -tls-cert "$(TLS_CERT)" -tls-key "$(TLS_KEY)"

service-install: build
	@test -n "$(SERVICE_DIRECTORY)" && test -f "$(SERVICE_DIRECTORY)/service.env"
	@mkdir -p "$(dir $(UNIT))"
	@umask 077; sed -e 's|@BIN@|$(BIN)|g' -e 's|@SERVICE_DIRECTORY@|$(SERVICE_DIRECTORY)|g' \
		-e 's|@LISTEN@|$(LISTEN)|g' -e 's|@COMPUTE@|$(COMPUTE)|g' meshcore-bot-service.service > "$(UNIT)"
	systemctl --user daemon-reload
	systemctl --user enable meshcore-bot-service.service
	systemctl --user restart meshcore-bot-service.service
	systemctl --user is-active meshcore-bot-service.service

service-status:
	systemctl --user --no-pager status meshcore-bot-service.service
