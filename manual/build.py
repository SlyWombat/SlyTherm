#!/usr/bin/env python3
"""manual/build.py — Dettson manual build pipeline (issue #42).

Builds the User Manual and Installation Manual from split markdown
sources, with mermaid diagram rendering and print-ready PDF output.

Pipeline (per manual):
    1. Assemble  manual/src/<manual>/*.md  in filename order.
    2. Render diagrams:
         - every ``` mermaid fenced block in the sources  -> SVG
         - every manual/diagrams/*.mmd                    -> SVG
         - hand-authored manual/diagrams/*.svg are used as-is
       Backends: local mmdc, falling back to kroki.io over HTTP. If a
       mermaid render fails on both backends a visible placeholder box
       is substituted (build still succeeds). A markdown reference to
       diagrams/NAME.svg with no .svg and no .mmd source is a hard
       error (exit nonzero).
    3. pandoc -> standalone HTML (--toc --toc-depth=2, CSS + images
       embedded, callout blockquotes classified, TOC relocated after
       the title page).
    4. Playwright Chromium page.pdf(): Letter, 0.75 in margins,
       backgrounds on, footer with page numbers + draft banner +
       product name, no page header.

Usage:
    python3 build.py [--manual user|install|all] [--format html|pdf|all]

Outputs:
    manual/build/<manual>.html
    docs/manuals/USER_MANUAL.pdf / INSTALLATION_MANUAL.pdf

Prerequisites: python3, pandoc >= 2.17, playwright (+ chromium),
mermaid-cli (mmdc) and/or outbound HTTPS to kroki.io.
"""
from __future__ import annotations

import argparse
import base64
import html as htmllib
import logging
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
import zlib
from pathlib import Path

# ── Paths / constants ────────────────────────────────────────────────

ROOT = Path(__file__).resolve().parent          # .../Dettson/manual
PROJECT = ROOT.parent                           # .../Dettson
SRC = ROOT / 'src'
DIAGRAMS = ROOT / 'diagrams'                    # .mmd sources + hand-authored .svg
THEME_CSS = ROOT / 'theme' / 'slytherm.css'
BUILD = ROOT / 'build'                          # gitignored
BUILD_DIAGRAMS = BUILD / 'diagrams'
PDF_DIR = PROJECT / 'docs' / 'manuals'

PRODUCT = 'ElectricRV Dual-Fuel Smart Thermostat'
MODEL = 'DT-1 (Dettson/Gree edition)'
DRAFT = 'DRAFT v0.3'

MANUALS = {
    'user': {'title': 'User Manual', 'pdf': 'USER_MANUAL.pdf'},
    'install': {'title': 'Installation Manual', 'pdf': 'INSTALLATION_MANUAL.pdf'},
}

KROKI_URL = 'https://kroki.io'
KROKI_TIMEOUT_S = 30

logging.basicConfig(level=logging.INFO, format='%(levelname)s: %(message)s')
log = logging.getLogger('slytherm.manual')

# Collected across a run; missing hand-authored SVGs -> exit nonzero.
HARD_ERRORS: list[str] = []


# ── Diagram rendering ────────────────────────────────────────────────

def _render_mmdc(mmd: Path, out: Path) -> None:
    cmd = ['mmdc', '-i', str(mmd), '-o', str(out),
           '-t', 'neutral', '-b', 'white']
    # Optional puppeteer config (executablePath + --no-sandbox) for hosts where
    # mmdc can't download/launch its own chromium: MMDC_PUPPETEER_CONFIG=<json>.
    _pcfg = os.environ.get('MMDC_PUPPETEER_CONFIG')
    if _pcfg:
        cmd += ['-p', _pcfg]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0 or not out.exists():
        raise RuntimeError(f'mmdc failed on {mmd.name}: '
                           f'{(r.stderr or r.stdout)[-300:]!r}')


def _render_kroki(mmd: Path, out: Path) -> None:
    src = mmd.read_bytes()
    encoded = base64.urlsafe_b64encode(zlib.compress(src, 9)).decode('ascii')
    url = f'{KROKI_URL}/mermaid/svg/{encoded}'
    with urllib.request.urlopen(url, timeout=KROKI_TIMEOUT_S) as resp:
        out.write_bytes(resp.read())


def _placeholder_svg(name: str) -> str:
    label = htmllib.escape(name)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="600" height="160" '
        'viewBox="0 0 600 160">'
        '<rect x="4" y="4" width="592" height="152" fill="#fef2f2" '
        'stroke="#b91c1c" stroke-width="3" stroke-dasharray="10 6"/>'
        '<text x="300" y="70" text-anchor="middle" font-family="sans-serif" '
        'font-size="20" fill="#b91c1c" font-weight="bold">'
        'DIAGRAM UNAVAILABLE</text>'
        f'<text x="300" y="105" text-anchor="middle" font-family="monospace" '
        f'font-size="14" fill="#7f1d1d">{label}</text>'
        '</svg>'
    )


def render_mermaid(mmd: Path, out: Path) -> bool:
    """Render one .mmd to SVG. mmdc first, kroki fallback, placeholder
    last. Returns True on a real render, False when the placeholder was
    substituted (build continues; not a hard error)."""
    out.parent.mkdir(parents=True, exist_ok=True)
    errors = []
    if shutil.which('mmdc'):
        try:
            _render_mmdc(mmd, out)
            log.info('diagram [mmdc]  %s -> %s', mmd.name, out.name)
            return True
        except Exception as e:                      # noqa: BLE001
            errors.append(f'mmdc: {e}')
    else:
        errors.append('mmdc: not on PATH')
    try:
        _render_kroki(mmd, out)
        log.info('diagram [kroki] %s -> %s', mmd.name, out.name)
        return True
    except Exception as e:                          # noqa: BLE001
        errors.append(f'kroki: {e}')
    log.warning('diagram %s unrenderable (%s) — placeholder substituted',
                mmd.name, '; '.join(errors))
    out.write_text(_placeholder_svg(mmd.stem), encoding='utf-8')
    return False


def render_diagram_sources() -> None:
    """Render every manual/diagrams/*.mmd into build/diagrams/."""
    BUILD_DIAGRAMS.mkdir(parents=True, exist_ok=True)
    # Purge stale placeholders so a previous run's substitutes never
    # mask a still-missing diagram on incremental rebuilds.
    for svg in BUILD_DIAGRAMS.glob('*.svg'):
        try:
            if 'DIAGRAM UNAVAILABLE' in svg.read_text(encoding='utf-8',
                                                      errors='replace'):
                svg.unlink()
        except OSError:
            pass
    for mmd in sorted(DIAGRAMS.glob('*.mmd')) if DIAGRAMS.is_dir() else []:
        render_mermaid(mmd, BUILD_DIAGRAMS / (mmd.stem + '.svg'))


# ── Source assembly ──────────────────────────────────────────────────

MERMAID_FENCE = re.compile(r'^```\s*mermaid[ \t]*\n(.*?)^```[ \t]*$',
                           re.M | re.S)
IMG_REF = re.compile(r'!\[[^\]]*\]\(\s*(?:\./)?diagrams/([^)\s]+\.svg)\s*\)')


def assemble(manual: str) -> Path:
    """Concatenate src/<manual>/*.md (filename order), extract inline
    mermaid fences to SVGs, and verify every diagrams/*.svg reference
    resolves. Writes build/<manual>-assembled.md."""
    src_dir = SRC / manual
    files = sorted(src_dir.glob('*.md'))
    if not files:
        raise SystemExit(f'No sources found in {src_dir}')
    parts = []
    for f in files:
        txt = f.read_text(encoding='utf-8')
        txt = txt.replace('\r\n', '\n').replace('\r', '\n')
        parts.append(txt.rstrip('\n') + '\n')
    text = '\n'.join(parts)

    # Inline ```mermaid fences -> SVG files + image references.
    counter = 0

    def _fence(m: re.Match) -> str:
        nonlocal counter
        counter += 1
        name = f'{manual}-inline-{counter:02d}'
        mmd = BUILD_DIAGRAMS / (name + '.mmd')
        mmd.parent.mkdir(parents=True, exist_ok=True)
        mmd.write_text(m.group(1), encoding='utf-8')
        render_mermaid(mmd, BUILD_DIAGRAMS / (name + '.svg'))
        return f'![](diagrams/{name}.svg)'

    text = MERMAID_FENCE.sub(_fence, text)

    # Verify every diagrams/NAME.svg reference resolves to a rendered
    # SVG (build/diagrams/) or a hand-authored one (manual/diagrams/).
    for name in sorted(set(IMG_REF.findall(text))):
        if (BUILD_DIAGRAMS / name).exists() or (DIAGRAMS / name).exists():
            continue
        mmd = DIAGRAMS / (Path(name).stem + '.mmd')
        if mmd.exists():
            # .mmd exists but wasn't rendered for some reason — render now.
            render_mermaid(mmd, BUILD_DIAGRAMS / name)
            continue
        HARD_ERRORS.append(
            f'{manual}: missing hand-authored diagram diagrams/{name} '
            f'(no .svg and no .mmd source)')
        # Substitute a placeholder so the build output is still inspectable.
        BUILD_DIAGRAMS.mkdir(parents=True, exist_ok=True)
        (BUILD_DIAGRAMS / name).write_text(
            _placeholder_svg(name), encoding='utf-8')

    out = BUILD / f'{manual}-assembled.md'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding='utf-8', newline='\n')
    log.info('assembled [%s]: %d sections -> %s', manual, len(files), out.name)
    return out


# ── HTML post-processing ─────────────────────────────────────────────

BQ_OPEN = re.compile(r'<blockquote>(\s*<p>)(.*?)(</p>)', re.S)
TAGS = re.compile(r'<[^>]+>')


def _classify_callouts(html: str) -> str:
    """Add callout classes to blockquotes based on their lead text.

    tbd      blue dashed   'TBD'
    info     blue          pending-verification / note / info / tip leads
    danger   red           gas / danger / shock / fire / CO leads
    warning  amber         warning / caution / damage / draft-banner leads
    """
    def repl(m: re.Match) -> str:
        lead = TAGS.sub('', m.group(2))[:160].lower()
        if 'tbd' in lead:
            cls = 'callout callout-tbd'
        elif 'pending verification' in lead:
            cls = 'callout callout-info'
        elif any(k in lead for k in ('danger', 'gas', 'shock', 'fire',
                                     'explosion', 'carbon monoxide')):
            cls = 'callout callout-danger'
        elif any(k in lead for k in ('warning', 'caution', 'damage', '⚠',
                                     'draft', 'pre-release')):
            cls = 'callout callout-warning'
        elif any(k in lead for k in ('note', 'info', 'tip')):
            cls = 'callout callout-info'
        else:
            return m.group(0)
        return f'<blockquote class="{cls}">{m.group(1)}{m.group(2)}{m.group(3)}'

    return BQ_OPEN.sub(repl, html)


def _relocate_toc(html: str) -> str:
    """Move pandoc's <nav id="TOC"> so the print order is:
    front matter / title page, contents, body sections.

    Preferred anchor: the closing of a <div class="title-page"> fenced
    div (the rich 00-front-matter contract; no nested divs allowed, so
    the first </div> closes it). Fallback for plain-markdown front
    matter: insert before the second <h1> in the document (the first
    H1 is the front-matter title, the second starts the body)."""
    toc = re.search(r'<nav id="TOC"[^>]*>.*?</nav>\s*', html, re.S)
    if not toc:
        return html
    nav = toc.group(0)
    html = html[:toc.start()] + html[toc.end():]

    tp_open = re.search(r'<div class="title-page">', html)
    if tp_open:
        tp_close = html.find('</div>', tp_open.end())
        if tp_close != -1:
            at = tp_close + len('</div>')
            return html[:at] + '\n' + nav + html[at:]

    h1s = [m.start() for m in re.finditer(r'<h1[ >]', html)]
    if len(h1s) >= 2:
        at = h1s[1]
        return html[:at] + nav + '\n' + html[at:]
    # No usable anchor — put the TOC back where pandoc had it.
    return html[:toc.start()] + nav + html[toc.start():]


# ── Format backends ──────────────────────────────────────────────────

def build_html(manual: str) -> Path:
    if not shutil.which('pandoc'):
        raise SystemExit('pandoc not found on PATH')
    assembled = assemble(manual)
    out = BUILD / f'{manual}.html'
    title = f'{PRODUCT} — {MANUALS[manual]["title"]}'
    cmd = ['pandoc', str(assembled), '-o', str(out),
           '--standalone', '--embed-resources',
           '--toc', '--toc-depth=2',
           '--css', str(THEME_CSS),
           '--metadata', f'pagetitle={title}',
           # build/ first so rendered SVGs win, then manual/ for
           # hand-authored diagrams/*.svg.
           f'--resource-path={BUILD}:{ROOT}']
    subprocess.run(cmd, check=True)
    html = out.read_text(encoding='utf-8')
    html = _classify_callouts(html)
    html = _relocate_toc(html)
    out.write_text(html, encoding='utf-8')
    log.info('html [%s]: %s (%d bytes)', manual, out.name, out.stat().st_size)
    return out


FOOTER_TEMPLATE = (
    '<div style="width:100%; font-size:8px; '
    "font-family:'Segoe UI',Roboto,Arial,sans-serif; color:#475569; "
    'padding:0 0.75in; display:flex; justify-content:space-between;">'
    f'<span>{PRODUCT} &mdash; {{TITLE}} &mdash; '
    f'<span style="color:#b45309; font-weight:bold;">{DRAFT}</span></span>'
    '<span>Page <span class="pageNumber"></span> of '
    '<span class="totalPages"></span></span></div>'
)


def build_pdf(manual: str) -> Path:
    html = build_html(manual)
    PDF_DIR.mkdir(parents=True, exist_ok=True)
    out = PDF_DIR / MANUALS[manual]['pdf']
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        raise SystemExit('playwright not installed; cannot produce PDF')
    footer = FOOTER_TEMPLATE.replace('{TITLE}', MANUALS[manual]['title'])
    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        page = browser.new_page()
        page.goto(html.as_uri(), wait_until='load')
        page.pdf(path=str(out), format='Letter',
                 margin={'top': '0.75in', 'bottom': '0.75in',
                         'left': '0.75in', 'right': '0.75in'},
                 print_background=True,
                 display_header_footer=True,
                 header_template='<span></span>',   # no page header anywhere
                 footer_template=footer)
        browser.close()
    log.info('pdf [%s]: %s (%.0f kB)', manual,
             out.relative_to(PROJECT), out.stat().st_size / 1e3)
    return out


# ── Orchestration ────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description='Dettson manual builder')
    p.add_argument('--manual', choices=('user', 'install', 'all'),
                   default='all')
    p.add_argument('--format', choices=('html', 'pdf', 'all'), default='all')
    args = p.parse_args(argv)

    manuals = list(MANUALS) if args.manual == 'all' else [args.manual]
    render_diagram_sources()
    for manual in manuals:
        if args.format == 'html':
            build_html(manual)
        else:  # pdf and all both end at the PDF (which builds HTML first)
            build_pdf(manual)

    if HARD_ERRORS:
        for e in HARD_ERRORS:
            log.error(e)
        log.error('build FAILED: %d missing hand-authored diagram(s)',
                  len(HARD_ERRORS))
        return 1
    log.info('manual build complete.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
