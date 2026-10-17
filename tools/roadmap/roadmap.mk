# ROADMAP.md is generated from docs/roadmap.sexp by tools/roadmap/render.py.
# Edit the data, then `make roadmap`; never edit ROADMAP.md by hand.
# `make roadmap-check` exits 1 when the committed page is not what the data renders.
PYTHON3 ?= python3

.PHONY: roadmap roadmap-check
roadmap:
	$(PYTHON3) tools/roadmap/render.py docs/roadmap.sexp ROADMAP.md

roadmap-check:
	$(PYTHON3) tools/roadmap/render.py --check docs/roadmap.sexp ROADMAP.md
