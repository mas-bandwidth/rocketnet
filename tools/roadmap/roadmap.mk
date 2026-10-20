# ROADMAP.md is generated from docs/roadmap.sexp, and TESTING.md from
# docs/testing.sexp, by tools/roadmap/render.py.
# Edit the data, then `make roadmap` or `make testing`; never edit either page by hand.
# `make roadmap-check` and `make testing-check` exit 1 when the committed page is not what the data renders.
PYTHON3 ?= python3

.PHONY: roadmap roadmap-check testing testing-check
roadmap:
	$(PYTHON3) tools/roadmap/render.py docs/roadmap.sexp ROADMAP.md

roadmap-check:
	$(PYTHON3) tools/roadmap/render.py --check docs/roadmap.sexp ROADMAP.md

testing:
	$(PYTHON3) tools/roadmap/render.py docs/testing.sexp TESTING.md

testing-check:
	$(PYTHON3) tools/roadmap/render.py --check docs/testing.sexp TESTING.md
