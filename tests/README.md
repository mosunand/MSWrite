# Editor regression checks

Run against the bundled editor with Node.js and Playwright installed:

```powershell
node tests/editor-regression.cjs
```

Set `NODE_PATH` if Playwright is supplied by an external runtime. Set
`MSWRITE_BROWSER` to override the default Windows Edge executable path.
The suite starts a temporary localhost server and closes its browser on exit.
It does not launch Mswrite, modify user Markdown files, or use saved profiles.

The checks use real browser key and mouse events, including:

- H1-H6 Enter at the beginning, middle, and end of a long document with earlier empty formulas.
- Repeated Enter, Backspace, and undo after Vditor's undo debounce has completed.
- Clicking and typing in the empty paragraph immediately above a code block.
- Formula insertion, immediate typing, compact preview/edit layout, and reopening at 1100px and 520px widths.
- Clicking away before the previous formula focus timers would have run.
- Selected formula text, undo, a new empty document, and source-mode round trips.
- Replacing an empty paragraph with a formula without adding a blank line, including undo/redo.
- Typed `$$` fences and ordinary `$x$` inline formulas.
- Live source/preview layout, error recovery, and clearing a formula.
- Multiline cases, matrices and tall braces via clipboard events, host paste and beforeinput.
- Literal LaTeX preservation, complete fenced paste, selection replacement and one-step paste undo/redo.
- Full-height braces and both ends of wide equations at desktop and narrow widths.
- Complex formulas through source-mode switching, reopening and rendered HTML export.
- Right-clicked formula previews receive whole-block color and bold without HTML in LaTeX.
- Recoloring, clearing color, toggling bold, combining styles in either order, and one-step undo/redo.
- Source selections keep their edit position; comments and escaped braces survive style removal.
- Identical neighboring formulas, canceled context targets, ordinary text/code formatting, and invalid selections.
- Styled formulas through source-mode switching, reopening and rendered HTML export.
- Exported KaTeX, code highlighting, embedded local images, and unchanged Markdown.

The critical heading reproduction is an empty formula before a heading. Lute
emits a temporary `wbr` inside an empty formula even on document load. Vditor
restores the first bookmark in the document on Enter, which used to move the
caret into that formula. The initial undo snapshot must also be taken after
bookmark cleanup and updated to the actual first edit position.

`fixtures/editor-smoke.md` is a manual rendering sample. A successful browser
suite does not replace testing WebView2's native save dialogs and PDF menu.
`fixtures/math-editing.md` contains cases, matrices, tall braces and a wide equation
for checking editing and export with the same document in other Markdown editors.

## Legacy painter and PDF reader regression checks

Build and run separately from the application with the same Qt/MinGW toolchain:

```powershell
cmake -S tests -B build/tests -G Ninja -DCMAKE_PREFIX_PATH="D:/Users/qt/6.8.3/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests --parallel
ctest --test-dir build/tests --output-on-failure
```

The Qt and MinGW `bin` directories must be on `PATH`, as for a development build.
The tests use the Windows Qt platform and installed fonts without showing any
windows. They do not load saved sessions, contact AI providers, or modify editor
documents. CTest imposes a 45-second timeout to catch a stalled Markdown scanner.

The 30 checks cover code fences, inline code, escaped dollars, incomplete formulas,
every prefix of a streamed message, light/dark and streaming/final rendering,
painter state, nested scripts, math whitespace, and formula image boundaries.
To generate a formula sheet for visual inspection, pass an output PNG path:

```powershell
build/tests/chat-regression.exe build/tests/formula-review.png
```

The application now uses the browser chat renderer. `chat-regression` retains
coverage of the old painter; it is not a substitute for the new chat tests below.
`pdf-regression` exercises the real Qt Quick PDF component: dragging over glyphs,
partial selection, Ctrl+C, select-all, search, navigation, bookmarks and contrast.
It creates temporary PDFs and restores the clipboard when finished.

## Browser chat and desktop integration

```powershell
node tests/chat-browser-regression.cjs
cmake -S tests -B build/tests -DMSWRITE_NATIVE_SMOKE=ON
cmake --build build/tests --parallel
ctest --test-dir build/tests --output-on-failure
```

The browser suite covers Markdown, KaTeX matrices/cases, code highlighting and
copy, HTML filtering, text selection during streaming, scroll retention, unchanged
message DOM, themes and narrow windows. It writes `build/chat-review.png`.
The optional desktop suite additionally verifies MainWindow's PDF menu routing,
PDF reopening and the real WebView2 model/clipboard bridge. It uses an isolated
settings directory and no external AI requests. Run the desktop suite in a normal Windows
session that permits WebView2 child processes; CTest enforces a 90-second timeout.

Desktop checks also cover explicit-file startup without a duplicate tab, immediate
read-only content before WebView2 starts, initialization of content/theme before
page scripts, and replacement of the preview by the ready editor. The test prints
preview/editor/PDF timings for its temporary sample documents. PDF checks verify
actual rendered outline pixels, fit-width overflow and the page's top position.

PDF coverage also checks title-dependent outline widths, manual resizing,
confirmed page-number entry, a 460px toolbar, actual raster resolution and the
large-zoom memory limit. CTest repeats the PDF suite with QT_SCALE_FACTOR=1.5
to verify selection coordinates and rendering at fractional display scaling.

The desktop suite checks welcome-page reflow at 883x1096 and 460x520. Its
welcome fixture uses a temporary APPDATA directory so real AI sessions are
never loaded or overwritten. Pass --screenshots for light/dark/narrow images.
The WebView2 fixture exercises closing an editor and then opening a conversation,
including recovery when the browser process is still shutting down.

## LaTeX preference and document-aware AI

`node tests/latex-regression.cjs` checks paired escaped delimiters, invalid and
unfinished formulas, literal code and currency, unchanged old content, preference
toggles, typing, paste, AI insertion, undo, caret placement, code-to-math conversion
and source-mode round trips. It also checks inline-code formulas, rich HTML clipboard
content, full Taylor-series Markdown insertion, and explicit document repair with undo.
It uses the bundled KaTeX parser and real browser input.

Native integration now also checks IME preedit/commit, the left attachment icon,
shortcut filtering, nested skill loading and refresh, missing-key selection actions,
draft preservation, live Markdown reads, file identity and PDF text/page images.
A local injected transport verifies the ReadDocument tool loop, image serialization,
skill inclusion and write-off enforcement without network or real credentials.
PDF tests execute the selection context menu and validate exact text/page capture.

A localhost HTTP SSE fixture exercises real input submission, HTTP parsing, worker
signals and WebView2 rendering for both user and assistant messages. It verifies
per-message LaTeX settings, first-run session persistence, restoration after disabling
the preference, renderer-failure text fallback and recovery. Injected non-streaming
responses also have to reach the final visible reply. These fixtures use temporary
settings and a fake test key; no configured provider or paid request is used.

The native suites check Windows clipboard availability first. When OpenClipboard
is denied, clipboard assertions are printed as SKIP, and the original clipboard is
not overwritten. Such a run does not verify OS clipboard integration; the remaining
UI, selection and document tests still run.
