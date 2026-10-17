// Run with Node.js and Playwright; uses installed Edge unless MSWRITE_BROWSER_PATH is set.
// Exercises the same Lute hooks used by the editor and chat without loading a renderer.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright');
const web = path.resolve(__dirname, '../resources/web');
const server = http.createServer((request, response) => {
    if (request.url === '/') {
        response.setHeader('Content-Type', 'text/html');
        response.setHeader('Content-Security-Policy', "default-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'");
        response.end('<!doctype html><meta charset="utf-8"><title>Math parser regression</title>');
        return;
    }
    try {
        const filename = path.resolve(web, '.' + new URL(request.url, 'http://localhost').pathname);
        if (!filename.startsWith(web + path.sep)) { response.writeHead(403).end(); return; }
        response.setHeader('Content-Type', 'application/javascript');
        response.end(fs.readFileSync(filename));
    } catch (_) { response.writeHead(404).end(); }
});

async function main() {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    let browser;
    try {
        browser = await chromium.launch({ headless: true,
            ...(process.env.MSWRITE_BROWSER_PATH ? { executablePath: process.env.MSWRITE_BROWSER_PATH } : { channel: 'msedge' }) });
        const page = await browser.newPage();
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        const origin = 'http://127.0.0.1:' + server.address().port;
        await page.goto(origin);
        await page.addScriptTag({ url: origin + '/vditor/dist/js/lute/lute.min.js' });
        await page.addScriptTag({ url: origin + '/strong-spacing.js' });
        await page.addScriptTag({ url: origin + '/math-mode.js' });
        const results = await page.evaluate(() => {
            const results = [];
            function check(name, condition, diagnostic) {
                results.push({ name, ok: Boolean(condition), diagnostic: condition ? undefined : diagnostic });
            }
            function fragment(html) { const t = document.createElement('template'); t.innerHTML = html; return t.content; }
            const attributes = [
                { text: '![\\(x\\)](image.png)', selector: 'img', attribute: 'alt', value: '\\(x\\)' },
                { text: '![a](assets/\\(x\\).png)', selector: 'img', attribute: 'src', value: 'assets/\\(x\\).png' },
                { text: '[x](https://example.invalid/\\(x\\))', selector: 'a', attribute: 'href', value: 'https://example.invalid/\\(x\\)' },
                { text: '[x](https://example.invalid "title \\(x\\)")', selector: 'a', attribute: 'title', value: 'title \\(x\\)' },
                { text: '<a title="\\(x\\)">link</a>', selector: 'a', attribute: 'title', value: '\\(x\\)' },
                { text: '<img src="image.png" alt="\\[x\\]">', selector: 'img', attribute: 'alt', value: '\\[x\\]' },
                { text: '![$a **b**$](image.png)', selector: 'img', attribute: 'alt', value: '$a **b**$', mathjaxOnly: true },
                { text: '[x](https://example.invalid/$a$)', selector: 'a', attribute: 'href', value: 'https://example.invalid/$a$' }
            ];
            for (const mode of ['mathjax', 'markdown']) {
                msMathMode.setMode(mode);
                const lute = Lute.New();
                for (const item of attributes) {
                    if (item.mathjaxOnly && mode !== 'mathjax') continue;
                    const html = lute.Md2HTML(item.text);
                    const node = fragment(html).querySelector(item.selector);
                    const round = lute.HTML2Md(html);
                    check(mode + ' restores ' + item.attribute + ' for ' + item.text,
                        node && node.getAttribute(item.attribute) === item.value && !/MSMATHMODE\w+TOKEN\w+END/.test(html + round),
                        { html, round });
                }
                const commentHtml = lute.Md2HTML('<!-- \\(x\\) $y$ -->');
                const comment = Array.from(fragment(commentHtml).childNodes).find(node => node.nodeType === Node.COMMENT_NODE);
                check(mode + ' restores formula-like text inside HTML comments',
                    comment && comment.data === ' \\(x\\) $y$ ' && !/MSMATHMODE\w+TOKEN\w+END/.test(commentHtml), commentHtml);
            }
            msMathMode.setMode('mathjax');
            const lute = Lute.New();
            const codeCases = [
                '```text\n\\(x\\)\n$y$\n```',
                '- ```text\n  \\(x\\)\n  $y$\n  ```',
                '+ ~~~text\n  \\(x\\)\n  $y$\n  ~~~',
                '1. ```text\n   \\(x\\)\n   $y$\n   ```',
                '> - ```text\n>   \\(x\\)\n>   $y$\n>   ```',
                '> > ```text\n> > \\(x\\)\n> > $y$\n> > ```',
                '- outer\n  - ```text\n    \\(x\\)\n    $y$\n    ```',
                '- ```text\n  - ```\n  \\(x\\)\n  $y$\n  ```',
                '`\\(x\\) $y$`',
                '``tick ` \\(x\\) $y$``',
                '    \\(x\\) $y$'
            ];
            for (const source of codeCases) {
                const html = lute.Md2HTML(source), ir = lute.Md2VditorIRDOM(source);
                const tree = fragment(html), sourceTree = fragment(ir);
                const code = tree.querySelector('code');
                const spun = lute.SpinVditorIRDOM(ir);
                check('code stays literal: ' + source.split('\n')[0],
                    code && code.textContent.includes('\\(x\\)') && code.textContent.includes('$y$') &&
                    !tree.querySelector('.language-math,.ms-math-literal') &&
                    !sourceTree.querySelector('[data-ms-math-mode],[data-ms-math-literal]') &&
                    !fragment(spun).querySelector('[data-ms-math-mode],[data-ms-math-literal]'), { source, html, ir, spun });
            }
            const sourceCases = [
                '\\(x^2\\)', '**\\(P\\)**', '\\[\nx+1\n\\]',
                '> \\[\n> x+1\n> \\]', '> - \\[\n>   x+1\n>   \\]',
                '$a **b**$', '$$\n\\{x\\} \\_\n$$', '\\\\(escaped\\\\)',
                '\\(unclosed', '\\(x\\\\)', '\\[unclosed',
                '[x](https://example.invalid/\\(x\\))', '![\\(x\\)](image.png)',
                '<a title="\\(x\\)">link</a>', '文字 \\(\\alpha\\) Unicode α 😀',
                'MSMATHMODEliteralTOKEN0END 与 \\(x\\)'
            ];
            for (const source of sourceCases) {
                let ir = lute.Md2VditorIRDOM(source);
                const saved = lute.VditorIRDOM2Md(ir);
                for (let n = 0; n < 3; n++) ir = lute.SpinVditorIRDOM(ir);
                const spun = lute.VditorIRDOM2Md(ir);
                check('repeated editor conversions retain source: ' + source.split('\n')[0],
                    saved === source + '\n' && spun === saved, { source, saved, spun });
            }
            const disabled = '$\\{x\\}$ 与 $a **b**$\n\n$$\n\\(disabled\\)\n$$';
            const disabledTree = fragment(lute.Md2HTML(disabled));
            check('disabled dollar expressions remain a single literal even with backslash formulas',
                !disabledTree.querySelector('.language-math,strong') && disabledTree.textContent.includes('\\(disabled\\)'),
                disabledTree.textContent);
            const references = [
                { text: '[x][\\(r\\)]\n\n[\\(r\\)]: https://example.invalid', selector: 'a', ir: '[data-type="link-ref"]' },
                { text: '![' + '\\(x\\)' + '][\\(r\\)]\n\n[\\(r\\)]: image.png', selector: 'img', ir: '[data-type="img"]' },
                { text: '[x][$r$]\n\n[$r$]: https://example.invalid', selector: 'a', ir: '[data-type="link-ref"]' },
                // Lute canonicalizes a collapsed reference to a shortcut.
                { text: '[\\(r\\)][]\n\n[\\(r\\)]: https://example.invalid', saved: '[\\(r\\)]\n\n[\\(r\\)]: https://example.invalid\n', selector: 'a', ir: '[data-type="link-ref"]' },
                { text: '\\(r\\) [x][\\(r\\)]\n\n[\\(r\\)]: https://example.invalid', selector: 'a', ir: '[data-type="link-ref"]' }
            ];
            for (const item of references) {
                const html = lute.Md2HTML(item.text);
                let ir = lute.Md2VditorIRDOM(item.text);
                for (let n = 0; n < 3; n++) ir = lute.SpinVditorIRDOM(ir);
                const saved = lute.VditorIRDOM2Md(ir);
                check('reference label resolves and retains source: ' + item.text.split('\n')[0],
                    fragment(html).querySelector(item.selector) && fragment(ir).querySelector(item.ir) &&
                    saved === (item.saved || item.text + '\n') && !/MSMATHMODE\w+TOKEN\w+END/.test(html + saved), { html, ir, saved });
            }
            const caretSource = '[x](https://example.invalid/\\(x\\))';
            const caretTree = fragment(lute.Md2VditorIRDOM(caretSource));
            const caretLiteral = caretTree.querySelector('.vditor-ir__marker--link .ms-math-literal');
            caretLiteral.firstChild.splitText(3).before(document.createElement('wbr'));
            const caretHolder = document.createElement('div'); caretHolder.append(caretTree);
            const caretIR = lute.SpinVditorIRDOM(caretHolder.innerHTML);
            const caretSpun = fragment(caretIR);
            const bookmark = caretSpun.querySelector('.vditor-ir__marker--link .ms-math-literal wbr');
            let caretPrefix = '';
            if (bookmark) {
                const range = document.createRange(); range.selectNodeContents(bookmark.parentElement); range.setEndBefore(bookmark);
                caretPrefix = range.toString();
            }
            check('editing a protected link destination retains the exact caret bookmark',
                caretSpun.querySelectorAll('wbr').length === 1 && caretPrefix === '\\(x' &&
                lute.VditorIRDOM2Md(caretIR) === caretSource + '\n', { caretIR, caretPrefix });
            const inline = fragment(lute.Md2HTML('\\(x\\) 与 \\[y\\]'));
            check('selected delimiters remain active outside code and attributes',
                inline.querySelectorAll('.language-math[data-ms-math-mode="mathjax"]').length === 2, inline.textContent);
            msMathMode.setMode('markdown');
            const legacy = Lute.New();
            const legacyTree = fragment(legacy.Md2HTML('$x$ 与 \\(y\\)'));
            check('traditional mode renders dollars and leaves backslash source intact',
                legacyTree.querySelectorAll('.language-math').length === 1 && legacyTree.textContent.includes('\\(y\\)'), legacyTree.textContent);
            msMathMode.setMode('invalid-value');
            check('invalid setting falls back to traditional mode', msMathMode.getMode() === 'markdown');
            return results;
        });
        for (const result of results) {
            assert.ok(result.ok, result.name + '\n' + JSON.stringify(result.diagnostic));
            console.log('PASS ' + result.name);
        }
        assert.deepEqual(errors, [], 'Unhandled browser errors');
        console.log(results.length + ' math parser checks passed.');
    } finally {
        if (browser) await browser.close();
        await new Promise(resolve => server.close(resolve));
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
