PYTHON ?= python3
ROOT := $(abspath ../..)
TMPDIR := $(ROOT)/.tmp
export TMPDIR
export PYTHONDONTWRITEBYTECODE := 1

.PHONY: test run wait-reply

test:
	@mkdir -p "$(TMPDIR)"
	@$(PYTHON) -m unittest -v test_meshcore_chat.py

run:
	@mkdir -p "$(TMPDIR)"
	@$(PYTHON) -u meshcore_chat.py $(ARGS)

wait-reply:
	@$(PYTHON) -u meshcore_chat.py wait-reply $(ARGS)
