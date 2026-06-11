# Dettson manual build pipeline

Builds the **User Manual** and **Installation Manual** from split
markdown sources into themed standalone HTML and print-ready PDF.

## Usage

```sh
cd manual
python3 build.py                          # both manuals, HTML + PDF
python3 build.py --manual user            # one manual
python3 build.py --manual install --format html
python3 build.py --format pdf             # pdf implies the html step
```

Prerequisites (all preinstalled on the dev box): `python3`, `pandoc`
(>= 2.17), `playwright` + Chromium, and `mmdc` (mermaid-cli) and/or
outbound HTTPS to kroki.io for diagram rendering.

## Layout

| Path | Purpose |
|------|---------|
| `src/user/NN-section-name.md` | User Manual sources, assembled in filename order |
| `src/install/NN-section-name.md` | Installation Manual sources |
| `diagrams/*.mmd` | Mermaid diagram sources (rendered to SVG at build time) |
| `diagrams/*.svg` | Hand-authored SVG diagrams (used as-is) |
| `theme/dettson.css` | Print-first theme (inlined into the HTML) |
| `build/` | **Gitignored** build output: `<manual>.html`, assembled markdown, rendered SVGs |
| `../docs/manuals/USER_MANUAL.pdf`, `../docs/manuals/INSTALLATION_MANUAL.pdf` | Published PDFs |

## Pipeline

Per manual: assemble `src/<manual>/*.md` in filename order → render
diagrams → pandoc → standalone HTML (`--toc --toc-depth=2`, CSS and
images embedded, so the file is self-contained) → Playwright Chromium
`page.pdf()` (Letter, 0.75 in margins, backgrounds printed, footer with
page numbers + product name + `DRAFT v0.1`, no page header).

## Source contract (what authors must follow)

Shared project conventions apply (two-digit prefixes, one H1 per
section file, H1 only in `00-front-matter.md` is the title page). On
top of those, the pipeline imposes:

### 1. Title page — `00-front-matter.md` markup contract

Two forms are accepted.

**Form A — plain front matter (currently used).** `00-front-matter.md`
is ordinary markdown: a single H1 title, the draft-banner blockquote
(auto-styled as an amber warning callout), metadata/revision tables,
and any "About this manual" prose. The build inserts the table of
contents **after** all front-matter content, immediately before the
second H1 in the document (the first body section). No special markup
is required.

**Form B — styled full-page title page (optional upgrade).** The
front-matter file contains exactly one pandoc fenced div with class
`title-page`, with **no nested fenced divs** inside it (the build
relocates the table of contents to just after the first `</div>` that
closes this block). The div renders as a centered, full-page title
with a boxed draft banner and forces a page break. Structure:

```markdown
::: {.title-page}

# ElectricRV Dual-Fuel Smart Thermostat {.unlisted .unnumbered}

[User Manual]{.doc-title}

[Model DT-1 (Dettson/Gree edition)]{.model}

[ElectricRV]{.manufacturer}

[DRAFT v0.1 — PRE-RELEASE, NOT FOR FIELD USE]{.draft-banner}

| Rev | Date       | Description   |
|-----|------------|---------------|
| 0.1 | 2026-06-11 | Initial draft |

:::
```

For Form B:

- The H1 carries `{.unlisted .unnumbered}` so it stays out of the TOC.
- The bracketed spans (`.doc-title`, `.model`, `.manufacturer`,
  `.draft-banner`) must each sit in their own paragraph (blank line
  above and below).
- The revision-history table is the last element inside the div.
- Anything after the closing `:::` lands after the TOC, at the top of
  the body.

In both forms the TOC gets its own page(s). The PDF has no page
header; the footer (product name, manual name, `DRAFT v0.1`, page
numbers) appears on every page including the first — Chromium's
`page.pdf()` cannot suppress it per-page.

### 2. Diagrams

- Reference rendered diagrams as `![Caption](diagrams/NAME.svg)`.
  Resolution order: a rendered `diagrams/NAME.mmd`, then a
  hand-authored `diagrams/NAME.svg`. A reference with **neither** is a
  hard error (build exits nonzero).
- Fenced ```` ```mermaid ```` blocks inside section files are also
  rendered (extracted to `build/diagrams/<manual>-inline-NN.svg`).
- Rendering tries local `mmdc`, then kroki.io. If both fail, a visible
  red "DIAGRAM UNAVAILABLE" placeholder box is substituted and the
  build still succeeds (warning logged) — only missing hand-authored
  SVGs fail the build.
- Hand-authored SVGs must be self-contained: inline styles, no
  external fonts beyond generic families, explicit `width`/`height`
  plus `viewBox`.

### 3. Callout boxes

Blockquotes are auto-classified by the lead text of their first
paragraph (case-insensitive, first ~160 characters) and styled:

| Class | Trigger keywords | Style |
|-------|------------------|-------|
| danger | `danger`, `gas`, `shock`, `fire`, `explosion`, `carbon monoxide` | red border |
| TBD | `TBD` (checked first, so `⚠ TBD (Phase 0)` is TBD, not warning) | blue **dashed** border |
| warning | `warning`, `caution`, `damage`, `⚠` | amber border |
| info | `note`, `info`, `tip`, `pending verification` | blue border |

Write callouts with a bold lead-in so the keyword is in the first
sentence, e.g.:

```markdown
> **⚠ Warning:** Never connect 24 VAC to the V/W2 terminal …

> **⚠ TBD (Phase 0):** Demand payload offsets are pending bus capture.

> **Danger — gas appliance:** …
```

Blockquotes matching none of the keywords render as a plain neutral
quote.

### 4. Headings and page breaks

Every H1 (section) starts a new PDF page. Tables, figures, code blocks
and callouts avoid page breaks internally. Keep H1s to one per file.

## Output locations

- `manual/build/user.html`, `manual/build/install.html` — standalone,
  fully embedded HTML (open directly in a browser).
- `docs/manuals/USER_MANUAL.pdf`, `docs/manuals/INSTALLATION_MANUAL.pdf`
  — committed, print-ready PDFs.
