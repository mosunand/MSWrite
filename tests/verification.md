# Chat visibility and LaTeX verification - 2026-09-24

Release: Qt 6.8.3 / MinGW 13.1, Windows.
Installed executable: `dist/Mswrite/Mswrite.exe`.
SHA-256: `682BF938AA3C9CE181582B9301650FB8710BCAEAAC2B4EAA09D4601C368A035D`.

The build and installed executable hashes match. Packaging updates the existing
release, including bridge version 135 and chat page version 2. No running Mswrite
process was present at deployment. User documents, settings, sessions and existing
skill files were preserved.

## Results

- LaTeX normalization: 23 boundary checks passed.
- LaTeX editor workflows: 18 passed.
- Existing editor browser regression: 53 passed.
- Browser chat rendering: 18 passed.
- Legacy chat painter: 30 passed.
- PDF: 36 passed at each of 100% and 150% display scaling.
- Native desktop integration: 66 passed, zero failed.

Windows clipboard access was available in the final native runs. PDF select-all
copy, dragged selection/Ctrl+C, MainWindow menu copy and browser-to-native chat copy
all passed. The suites restore the original clipboard after testing.

## Reproductions and fixes

The Taylor-series example exposed missing conversion of a display formula wrapped
in inline code. Rich HTML paste could bypass normalization, and AI Markdown passed
through literal insertion even when it needed parsing. The browser tests now verify
four rendered formulas after a complete AI Markdown insertion, three after rich HTML
paste, and explicit repair of existing escaped/code formulas with one-step undo.
Preference toggles still leave existing document content unchanged.

The reported all-blank chat was not reliably reproduced with the old welcome/chat
fixture. Saved session text and logs showed completed user/assistant content.
The implementation now creates the browser under its final stack parent, retains
native ancestors and rebinds its controller when the native window changes. Message
updates use a result callback and render acknowledgement. Loading or failed updates
keep a readable native preview of recent messages and allow a full resynchronization.

A localhost HTTP SSE server exercises real input submission, network parsing,
worker signals, message models and the WebView2 DOM. Both user and assistant text
appear, the assistant renders two preference-enabled formulas, and window reopening
preserves browser visibility. Deliberately removing the page renderer shows the
native text fallback in the visible conversation area; restoring the renderer
recovers all messages. Visual review inspected that fallback and the browser chat
layout. A separate mock verifies one-shot non-streaming response text delivery.

First-run session persistence initially failed because its parent directory was
missing. Saving now creates that directory and atomically replaces the session file.
The full integration test verifies the stored per-message LaTeX flag and restores
the rendered formulas after the global preference is disabled.

No real provider, API key or paid request was used. These checks validate the local
display and conversion pipeline, not remote model output quality or every machine's
graphics-driver behavior.

## Existing behavior retained

Native checks also cover live Markdown/document identity, PDF text and page images,
read-only PDF selection actions, missing API configuration, draft preservation,
ReadDocument tool execution, skill loading, IME preedit/commit and welcome reflow.
Editor checks retain heading/caret, undo/redo, math editing, formatting and export
coverage. PDF checks include glyph selection, zoom resolution, memory limits,
search, bookmarks, outline sizing and fractional display scaling.

The final native run measured 757 ms to the sample startup preview, 3087 ms to
editor content, 581 ms to a PDF page and 2518 ms to the standalone chat page after
repeated browser teardown. Transient controller-creation retries recovered in the
run. These are sample observations, not startup guarantees.

## Release hygiene

Only one distributable is retained at `dist/Mswrite`. Temporary build/test binaries,
browser profiles and generated screenshots are removed after verification. Existing
review images, test sources and fixtures remain available for reproduction.
