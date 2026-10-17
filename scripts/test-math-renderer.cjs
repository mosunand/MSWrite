// Run with Node.js, Playwright and installed Edge; MSWRITE_BROWSER_PATH may override Edge.
const fs = require('fs');
const http = require('http');
const path = require('path');
const assert = require('assert');
const { chromium } = require('playwright');
const web = path.resolve(__dirname, '../resources/web');
const csp = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' https: http: data: blob:; font-src 'self' data:; connect-src 'self' https: http:; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'";
const requests = [];
const server = http.createServer((req, res) => {
    requests.push(req.url);
    if (req.url === '/' || req.url === '/delay') {
        res.setHeader('Content-Type', 'text/html');
        res.setHeader('Content-Security-Policy', csp);
        res.end('<!doctype html><html><head><meta charset="utf-8"></head><body><div id="content"></div></body></html>');
        return;
    }
    const filename = path.join(web, decodeURIComponent(req.url.split('?')[0].slice(1)));
    if (!filename.startsWith(web + path.sep)) { res.writeHead(403).end(); return; }
    const send = () => {
        try { res.setHeader('Content-Type', 'application/javascript'); res.end(fs.readFileSync(filename)); }
        catch (_) { res.writeHead(404).end(); }
    };
    if (req.url.endsWith('tex-svg-full.js') && req.headers.referer?.endsWith('/delay')) setTimeout(send, 300);
    else send();
});
(async () => {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const browser = await chromium.launch({ headless: true,
        ...(process.env.MSWRITE_BROWSER_PATH ? { executablePath: process.env.MSWRITE_BROWSER_PATH }
            : { channel: 'msedge' }) });
    let checks = 0;
    try {
        const page = await browser.newPage();
        const errors = [], external = [];
        page.on('pageerror', error => errors.push(error.message));
        page.on('request', request => { if (!request.url().startsWith('http://127.0.0.1:')) external.push(request.url()); });
        const origin = 'http://127.0.0.1:' + server.address().port;
        await page.goto(origin);
        await page.evaluate(() => { window.msMathMode = { getMode: () => 'mathjax' }; });
        await page.addScriptTag({ url: origin + '/math-renderer.js' });
        const tests = await page.evaluate(async () => {
            const tests = [];
            const check = (name, ok) => tests.push({ name, ok: !!ok });
            const root = document.getElementById('content');
            const formula = (source, display = false) => {
                const element = document.createElement('span');
                element.className = 'language-math';
                element.dataset.math = source;
                element.dataset.msMathDisplay = String(display);
                root.append(element); return element;
            };
            const inline = formula('\\frac{a}{b}+\\sqrt{x^2+1}');
            const display = formula('\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}', true);
            await msMathRenderer.render(root);
            check('inline SVG renders', inline.querySelector('svg'));
            check('display SVG renders', display.querySelector('mjx-container[display="true"]'));
            check('inline remains inline', !inline.querySelector('mjx-container[display="true"]'));
            check('runtime version correct', MathJax.version === '3.1.2');
            check('export stylesheet captured', msMathRenderer.exportStyle().includes('MJX-SVG-styles'));
            const ids = new Set(Array.from(inline.querySelectorAll('[id]')).map(el => el.id));
            check('self contained font references', Array.from(inline.querySelectorAll('use')).every(el => ids.has(el.getAttributeNS('http://www.w3.org/1999/xlink', 'href')?.slice(1))));
            inline.dataset.msMathDisplay = 'true';
            await msMathRenderer.render(inline);
            check('display-only change rerenders', inline.querySelector('mjx-container[display="true"]'));
            for (const source of ['\\href{javascript:alert(1)}{x}', '\\require{html}x', '\\style{position:fixed}{x}', '\\class{evil}{x}']) {
                const element = formula(source);
                await msMathRenderer.render(element);
                check('dangerous formula stays source: ' + source, !element.querySelector('svg') && element.textContent === '\\(' + source + '\\)');
            }
            for (const source of [String.raw`\mmlToken{mglyph}[src="https://bad.invalid/token-image"]{x}`,
                String.raw`\let\imageToken=\mmlToken\imageToken{mglyph}[src="https://bad.invalid/alias-image"]{x}`,
                String.raw`\def\imageToken#1{\mmlToken{mglyph}[src=#1]{x}}\imageToken{https://bad.invalid/macro-image}`,
                String.raw`\mmlToken{mi}[style="background-image:url(https://bad.invalid/token-style)"]{x}`,
                String.raw`\mmlToken{mi}[href="https://bad.invalid/token-link"]{x}`,
                String.raw`\mmlToken{mi}[mathcolor="url(https://bad.invalid/token-paint)"]{x}`]) {
                const element = formula(source);
                await msMathRenderer.render(element);
                check('unsafe MathML token rejected before output: ' + source,
                    !element.querySelector('svg') && element.textContent === '\\(' + source + '\\)');
            }
            const normalToken = formula(String.raw`\mmlToken{mo}[lspace="thickmathspace" rspace="thickmathspace"]{mod}`);
            const modCommands = formula(String.raw`x \bmod y \pmod n`);
            const coloredToken = formula(String.raw`\mmlToken{mi}[mathcolor="#369"]{x}`);
            await msMathRenderer.render(root);
            check('ordinary MathML token remains supported', normalToken.querySelector('svg'));
            check('built-in mod commands remain supported', modCommands.querySelector('svg'));
            check('safe MathML token color retained', coloredToken.querySelector('[fill="#369"]'));
            const definition = formula('\\newcommand{\\tempMacro}{y}\\tempMacro');
            const leaked = formula('\\tempMacro');
            await msMathRenderer.render(root);
            check('same formula macros supported', definition.querySelector('svg'));
            check('macros isolated between formulas', !leaked.querySelector('svg'));
            const escapedStyle = formula(String.raw`\unicode[serif; background-image:u\72l(https://bad.invalid/x);position:fixed;top:0]{65}`);
            await msMathRenderer.render(escapedStyle);
            check('unicode formula still renders', escapedStyle.querySelector('svg'));
            check('CSS escaped URL and positions removed', !Array.from(escapedStyle.querySelectorAll('[style]')).some(el => /url|position|background|top:/i.test(el.getAttribute('style'))));
            check('safe font family retained', escapedStyle.querySelector('g[style="font-family: serif;"]'));
            const unicodeDefault = await msMathRenderer.convert(String.raw`\unicode{65}`, false, false);
            check('unicode font does not leak between formulas', !unicodeDefault.querySelector('[style*="font-family"]'));
            const unicodeShared = await msMathRenderer.convert(String.raw`\unicode[monospace]{66}+\unicode{66}`, false, false);
            check('unicode font reused within the same formula', unicodeShared.querySelectorAll('[style*="font-family: monospace"]').length === 2);
            const unicodeHex = await msMathRenderer.convert(String.raw`\unicode[1.0,0.2][serif]{0x41}`, false, false);
            check('unicode dimensions and hex remain supported', unicodeHex.querySelector('[style*="font-family: serif"]'));
            for (const source of [String.raw`\unicode{1114112}`, String.raw`\unicode{55296}`, String.raw`\unicode{not-a-number}`]) {
                const element = formula(source);
                await msMathRenderer.render(element);
                check('invalid unicode stays source: ' + source, !element.querySelector('svg'));
            }
            for (const source of [String.raw`\color{url(https://bad.invalid/paint)}x`,
                String.raw`\color{u\72l(https://bad.invalid/paint)}x`]) {
                const element = formula(source);
                await msMathRenderer.render(element);
                check('URL paint formula still renders: ' + source, element.querySelector('svg'));
                check('SVG paint URL removed: ' + source, !Array.from(element.querySelectorAll('[fill],[stroke]'))
                    .some(el => /url|\\/i.test((el.getAttribute('fill') || '') + (el.getAttribute('stroke') || ''))));
            }
            const colored = formula(String.raw`\textcolor{#369}{x}`);
            await msMathRenderer.render(colored);
            check('legitimate formula color retained', colored.querySelector('g[fill="#369"][stroke="#369"]'));
            const bboxStyle = formula(String.raw`\bbox[background-image:u\72l(https://bad.invalid/bg)]{x}`);
            await msMathRenderer.render(bboxStyle);
            check('bbox CSS escaped background removed', bboxStyle.querySelector('svg') &&
                !Array.from(bboxStyle.querySelectorAll('[style]')).some(el => /background|url/i.test(el.getAttribute('style'))));
            const mathjaxFactory = MathJax._.mathjax.mathjax.document;
            let cleaned;
            try {
                MathJax._.mathjax.mathjax.document = () => ({ convert() {
                    const node = document.createElement('mjx-container');
                    node.innerHTML = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 1000">' +
                        '<defs><clipPath id="safe"><rect width="1" height="1"/></clipPath></defs>' +
                        '<g id="reference-test" fill="#369" stroke="currentColor" filter="url(https://bad.invalid/f)" ' +
                        'mask="u\\72l(https://bad.invalid/m)" clip-path="url(#safe)" ' +
                        'marker-start="url(https://bad.invalid/k)" cursor="url(https://bad.invalid/c),auto"/>' +
                        '<use href=" #missing reference " onload="alert(1)"/>' +
                        '<image/><foreignObject><iframe src="about:blank"/></foreignObject></svg>';
                    return node;
                } });
                cleaned = await msMathRenderer.convert('x', false, false);
            } finally { MathJax._.mathjax.mathjax.document = mathjaxFactory; }
            const referenceTest = cleaned.querySelector('#reference-test');
            check('external SVG reference attributes removed', ['filter', 'mask', 'marker-start', 'cursor']
                .every(name => !referenceTest.hasAttribute(name)));
            check('local SVG clip reference retained', referenceTest.getAttribute('clip-path') === 'url(#safe)');
            check('ordinary SVG paint retained', referenceTest.getAttribute('fill') === '#369' && referenceTest.getAttribute('stroke') === 'currentColor');
            check('SVG hyperlink and event attributes removed', !cleaned.querySelector('use').hasAttribute('href') &&
                !cleaned.querySelector('use').hasAttribute('onload'));
            check('foreign SVG content removed', !cleaned.querySelector('image,foreignObject,iframe'));
            const invalid = formula('\\invalidCommand');
            await msMathRenderer.render(invalid);
            check('invalid formula shows error title', invalid.title && invalid.classList.contains('vditor-reset--error'));
            invalid.dataset.math = 'x+1';
            await msMathRenderer.render(invalid);
            check('corrected formula renders', invalid.querySelector('svg'));
            check('corrected formula clears title', !invalid.hasAttribute('title') && !invalid.classList.contains('vditor-reset--error'));
            const huge = formula('\\rule{9999999em}{9999999em}');
            await msMathRenderer.render(huge);
            check('huge output stays source', !huge.querySelector('svg'));
            const recursive = formula('\\def\\x{\\x}\\x');
            await msMathRenderer.render(recursive);
            check('recursive macros rejected', !recursive.querySelector('svg'));
            let oversizedRejected = false;
            try { await msMathRenderer.convert('x'.repeat(16385), false, false); } catch (_) { oversizedRejected = true; }
            check('oversized source rejected', oversizedRejected);
            const afterErrors = formula('x^2');
            await msMathRenderer.render(afterErrors);
            check('queue recovers after rejected conversion', afterErrors.querySelector('svg'));
            const detachedRoot = document.createElement('div');
            const detached = document.createElement('span');
            detached.className = 'language-math'; detached.dataset.math = 'z'; detachedRoot.append(detached);
            const detachedJob = msMathRenderer.render(detachedRoot, true);
            msMathRenderer.invalidate(); await detachedJob;
            check('detached export retains captured generation', detached.querySelector('svg'));
            let conversionCount = 0;
            const previousMode = window.msMathMode.getMode;
            try {
                window.msMathMode.getMode = () => 'markdown';
                MathJax._.mathjax.mathjax.document = (...args) => {
                    conversionCount++;
                    return mathjaxFactory(...args);
                };
                const staleRoot = document.createElement('div');
                document.body.append(staleRoot);
                const staleJobs = [];
                for (let index = 0; index < 100; index++) {
                    const element = document.createElement('span');
                    element.className = 'language-math'; element.textContent = 'x'.repeat(1000);
                    staleRoot.append(element); staleJobs.push(msMathRenderer.render(element));
                }
                msMathRenderer.invalidate(); await Promise.all(staleJobs);
                check('invalidated queue does not perform conversions', conversionCount === 0);
                check('invalidated queue clears every pending flag', !msMathRenderer.pending(staleRoot));
                const latest = document.createElement('span');
                latest.className = 'language-math'; latest.dataset.math = 'old'; staleRoot.append(latest);
                const oldJob = msMathRenderer.render(latest);
                latest.dataset.math = 'new';
                const newJob = msMathRenderer.render(latest);
                await Promise.all([oldJob, newJob]);
                check('superseded source only converts once', conversionCount === 1);
                check('superseded source renders latest value', latest.querySelector('svg') && latest.dataset.math === 'new');
                const removed = document.createElement('span');
                removed.className = 'language-math'; removed.textContent = 'x'; staleRoot.append(removed);
                const removedJob = msMathRenderer.render(removed); removed.remove(); await removedJob;
                check('removed preview does not perform a conversion', conversionCount === 1 && !removed.querySelector('svg'));
                staleRoot.append(removed); await msMathRenderer.render(removed);
                check('reattached preview recovers after cancellation', conversionCount === 2 && removed.querySelector('svg'));
                conversionCount = 0;
                MathJax._.mathjax.mathjax.document = (...args) => {
                    conversionCount++;
                    const deadline = performance.now() + 8;
                    while (performance.now() < deadline) {}
                    return mathjaxFactory(...args);
                };
                const responsiveJobs = [];
                for (let index = 0; index < 20; index++) {
                    const element = document.createElement('span');
                    element.className = 'language-math'; element.textContent = 'x';
                    staleRoot.append(element); responsiveJobs.push(msMathRenderer.render(element));
                }
                let inputProcessed = false;
                setTimeout(() => { inputProcessed = true; msMathRenderer.invalidate(); }, 0);
                await Promise.all(responsiveJobs);
                check('large queue yields to browser input', inputProcessed && conversionCount < 20);
                check('cancelled responsive queue clears pending flags', !msMathRenderer.pending(staleRoot));
                staleRoot.remove();
            } finally {
                MathJax._.mathjax.mathjax.document = mathjaxFactory;
                window.msMathMode.getMode = previousMode;
            }
            return tests;
        });
        for (const test of tests) { assert(test.ok, test.name); checks++; console.log('PASS ' + test.name); }
        await page.goto(origin + '/delay');
        await page.evaluate(() => { window.msMathMode = { getMode: () => 'mathjax' }; });
        await page.addScriptTag({ url: origin + '/math-renderer.js' });
        const staleTests = await page.evaluate(async () => {
            const element = document.createElement('span'); element.className = 'language-math'; element.dataset.math = 'x'; element.textContent = 'x'; document.body.append(element);
            const job = msMathRenderer.render(element);
            const pendingRoot = msMathRenderer.pending(element);
            await Promise.resolve();
            const loadStarted = !!document.getElementById('msMathJaxScript');
            window.msMathMode.getMode = () => 'markdown';
            msMathRenderer.invalidate(); await job;
            return { pendingRoot, loadStarted, staleNotApplied: !element.querySelector('svg') && element.textContent === 'x', pendingCleared: !msMathRenderer.pending(element) };
        });
        for (const [name, ok] of Object.entries(staleTests)) { assert(ok, name); checks++; console.log('PASS ' + name); }
        await page.goto(origin);
        await page.evaluate(() => { window.msMathMode = { getMode: () => 'mathjax' }; });
        await page.addScriptTag({ url: origin + '/math-renderer.js' });
        let assetAttempts = 0;
        await page.route('**/tex-svg-full.js', route => {
            assetAttempts++;
            return assetAttempts === 1 ? route.abort() : route.continue();
        });
        const unavailableTests = await page.evaluate(async () => {
            const element = document.createElement('span');
            element.className = 'language-math'; element.textContent = String.raw`\frac{x}{y}`;
            document.body.append(element);
            await msMathRenderer.render(element);
            const sourcePreserved = element.textContent === String.raw`\(\frac{x}{y}\)` &&
                element.classList.contains('vditor-reset--error') && !msMathRenderer.pending(element);
            element.dataset.math = 'x+1'; await msMathRenderer.render(element);
            const failedGenerationCached = !element.querySelector('svg');
            msMathRenderer.invalidate(); await msMathRenderer.render(element);
            return { sourcePreserved, failedGenerationCached,
                newGenerationRetries: !!element.querySelector('svg'),
                recoveryClearsError: !element.classList.contains('vditor-reset--error') && !element.title,
                recoveryPendingCleared: !msMathRenderer.pending(element) };
        });
        for (const [name, ok] of Object.entries(unavailableTests)) { assert(ok, name); checks++; console.log('PASS ' + name); }
        assert.strictEqual(assetAttempts, 2, 'failed asset retries once per new generation'); checks++;
        await page.unroute('**/tex-svg-full.js');
        assert.deepStrictEqual(errors, [], 'no page errors'); checks++;
        assert.deepStrictEqual(external, [], 'no external requests'); checks++;
        assert(requests.filter(url => url.endsWith('tex-svg-full.js')).length === 3, 'one successful MathJax script load per document'); checks++;
        console.log(JSON.stringify({ checks, errors, external, requests }, null, 2));
    } finally { await browser.close(); server.close(); }
})().catch(error => { console.error(error); server.close(); process.exitCode = 1; });
