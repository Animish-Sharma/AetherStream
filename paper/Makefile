PYTHON ?= python3
LATEXMK ?= latexmk
PDFLATEX ?= pdflatex
BIBTEX ?= bibtex

FIGURES := figures/fig_rate_distortion_theory.pdf \
           figures/fig_log_spectral_distance.pdf \
           figures/fig_random_access_scaling.pdf
SECTIONS := $(wildcard sections/*.tex)

.PHONY: all figures clean distclean

all: paper.pdf

figures: $(FIGURES)

$(FIGURES): ../benchmarks/generate_paper_plots.py \
            ../benchmarks/theoretical_bounds.py \
            ../benchmarks/spectral_metrics.py
	cd .. && $(PYTHON) benchmarks/generate_paper_plots.py

paper.pdf: main.tex references.bib $(SECTIONS) $(FIGURES)
	@if command -v $(LATEXMK) >/dev/null 2>&1; then \
		$(LATEXMK) -pdf -jobname=paper -interaction=nonstopmode -halt-on-error main.tex; \
	else \
		$(PDFLATEX) -jobname=paper -interaction=nonstopmode -halt-on-error main.tex && \
		$(BIBTEX) paper && \
		$(PDFLATEX) -jobname=paper -interaction=nonstopmode -halt-on-error main.tex && \
		$(PDFLATEX) -jobname=paper -interaction=nonstopmode -halt-on-error main.tex; \
	fi

clean:
	@if command -v $(LATEXMK) >/dev/null 2>&1; then \
		$(LATEXMK) -C -jobname=paper main.tex; \
	fi
	rm -f main.aux main.bbl main.blg main.log main.pdf main.out
	rm -f paper.aux paper.bbl paper.blg paper.fdb_latexmk paper.fls paper.log paper.out paper.pdf

# Generated publication figures are reproducible and intentionally untracked.
distclean: clean
	rm -rf figures
