const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');
globalThis.katex = require('../resources/web/vditor/dist/js/katex/katex.min.js');
const { normalize } = require('../resources/web/latex-preference.js');

(async () => {
    const cases = [
        [String.raw`\$f(x)\$`, '$f(x)$'],
        [String.raw`\\$f(x)\\$`, '$f(x)$'],
        [String.raw`\$x$`, String.raw`\$x$`],
        [String.raw`$x\$`, String.raw`$x\$`],
        [String.raw`\$x`, String.raw`\$x`],
        [String.raw`\$a\$ + \$b\$`, '$a$ + $b$'],
        [String.raw`\$\frac{a}{b}\$`, String.raw`$\frac{a}{b}$`],
        ['`\\$x\\$`', '`\\$x\\$`'],
        ['``a ` \\$x\\$``', '``a ` \\$x\\$``'],
        ['`$$x^2$$`', '\n\n$$\nx^2\n$$\n\n'],
        ['`value = $$x^2$$`', '`value = $$x^2$$`'],
        ['    `$$x^2$$`','    `$$x^2$$`'],
        ['    \\$x\\$', '    \\$x\\$'],
        ['```js\nconst s = "\\$x\\$";\n```', '```js\nconst s = "\\$x\\$";\n```'],
        ['```tex\n$$f(x)=x^2$$\n```', '$$\nf(x)=x^2\n$$'],
        ['~~~~tex\n$$x^2$$\n~~~~~', '$$\nx^2\n$$'],
        ['```\nBefore $$x$$ after\n```', '```\nBefore $$x$$ after\n```'],
        ['```\n$$x$$\n$$y$$\n```', '```\n$$x$$\n$$y$$\n```'],
        ['```\n\\$x\\$\n', '```\n\\$x\\$\n'],
        [String.raw`Price \$5 and \$10`, String.raw`Price \$5 and \$10`],
        [String.raw`$\text{\$x\$}$`, String.raw`$\text{\$x\$}$`],
        [String.raw`\$\unknowncommand{x}\$`, String.raw`\$\unknowncommand{x}\$`]
    ];
    for (const [input, expected] of cases) assert.equal(normalize(input), expected, input);
    assert.equal(normalize(String.raw`old \$x\$ new`, String.raw`old \$x\$`), String.raw`old \$x\$ new`);
    console.log(`${cases.length + 1} normalization boundary checks passed.`);
    const web = path.resolve(__dirname, '../resources/web');
    const server = http.createServer(async (req, res) => {
        try {
            const file = path.resolve(web, '.' + new URL(req.url, 'http://localhost').pathname);
            if (!file.startsWith(web + path.sep)) throw Error('path');
            res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.html': 'text/html', '.css': 'text/css' })[path.extname(file)] || 'application/octet-stream');
            res.end(await fs.readFile(file));
        } catch { res.writeHead(404).end(); }
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const browser = await chromium.launch({ executablePath: process.env.MSWRITE_BROWSER || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe' });
    try {
        const page = await browser.newPage();
        const errors = []; page.on('pageerror', e => errors.push(e.message));
        await page.addInitScript(() => { window.chrome.webview = { postMessage: m => (window.__messages ||= []).push(m) }; });
        await page.goto(`http://127.0.0.1:${server.address().port}/editor.html`);
        await page.waitForFunction(() => window.__messages?.some(m => m.t === 'ready'));
        const wait = () => page.waitForTimeout(650);
        const value = () => page.evaluate(() => window.mswValue());
        const load = async (content = '') => { await page.evaluate(md => window.msbridge.setContent(md), content); await wait(); await page.locator('.vditor-ir pre.vditor-reset').click(); await page.keyboard.press('Control+End'); };
        await load(String.raw`old \$f(x)\$`);
        const old = await value();
        await page.evaluate(() => window.msbridge.setPreferLatex(true)); await wait();
        assert.equal(await value(), old, 'Enabling must not rewrite existing content');
        await page.keyboard.press('Enter');
        await page.evaluate(() => window.msbridge.pasteText(String.raw`\$g(x)\$`)); await wait();
        assert.match(await value(), /\$g\(x\)\$/);
        assert.ok((await value()).includes(String.raw`\$f(x)\$`), 'Old escaped formula changed');
        await page.evaluate(() => window.msbridge.setPreferLatex(false));
        assert.match(await value(), /\$g\(x\)\$/);
        await load(); await page.evaluate(() => window.msbridge.pasteText(String.raw`\$z\$`)); await wait();
        assert.ok((await value()).includes(String.raw`\$z\$`), 'Disabled preference converted input');
        await page.evaluate(() => window.msbridge.setPreferLatex(true));
        await load();
        await page.evaluate(() => window.msbridge.pasteText('```tex\n$$f(x)=x^2$$\n```')); await wait();
        assert.equal(await page.locator('[data-type="math-block"][data-block="0"]').count(), 1);
        assert.equal(await page.locator('[data-type="code-block"][data-block="0"]').count(), 0);
        await page.keyboard.press('Control+z'); await wait();
        assert.ok(!(await value()).includes('f(x)'), `Conversion cannot be undone: ${await value()}`);
        await load(); await page.keyboard.type(String.raw`\$f(x)\$`, { delay: 25 }); await wait();
        assert.ok((await value()).includes('$f(x)$') && !(await value()).includes(String.raw`\$f(x)`), `Typed formula: ${await value()}`);
        await page.keyboard.type(' tail'); await wait();
        assert.match(await value(), /\$f\(x\)\$ tail/, 'Caret lost after typed conversion');
        await load('```text\n\n```');
        await page.locator('[data-type="code-block"] .vditor-ir__preview').click();
        await page.locator('[data-type="code-block"] pre.vditor-ir__marker--pre code').click();
        await page.keyboard.type('$$x^2$$', { delay: 25 }); await wait();
        assert.equal(await page.locator('[data-type="math-block"][data-block="0"]').count(), 1, `Typed code: ${await value()}`);
        await page.keyboard.type('after');await wait();
        assert.match(await value(), /\$\$\s+after/, 'Code conversion caret must follow formula');
        await load('```text\n\n```');
        await page.locator('[data-type="code-block"] .vditor-ir__preview').click();
        await page.locator('[data-type="code-block"] pre.vditor-ir__marker--pre code').click();
        await page.evaluate(() => window.msbridge.pasteText('$$a^2+b^2=c^2$$'));await wait();
        assert.equal(await page.locator('[data-type="math-block"][data-block="0"]').count(),1, `Code paste: ${await value()}`);
        await load();await page.evaluate(() => window.msbridge.insertText(String.raw`\$q^2\$`));await wait();
        assert.equal((await value()).trim(),'$q^2$','AI insertion follows preference');
        const taylor = String.raw`设函数 $f(x)$ 在点 $a$ 处具有任意阶导数。`+'\n\n'+
            '`'+String.raw`$$f(x) = f(a) + \frac{f^{(2)}(a)}{2!}(x-a)^2$$`+'`\n\n'+
            '```latex\n'+String.raw`$$f(x) = \sum_{n=0}^{\infty} \frac{f^{(n)}(a)}{n!}(x-a)^n$$`+'\n```';
        await load();await page.evaluate(md => window.msbridge.insertText(md), taylor);await wait();
        assert.equal(await page.locator('.katex').count(),4, `Full AI Markdown did not render: ${await value()}`);
        await load();
        await page.locator('.vditor-ir pre.vditor-reset').evaluate(el => {
            const data=new DataTransfer();
            data.setData('text/plain', '设函数 $f(x)$ 在点 $a$ 处。\n$$f(x)=x^2$$');
            data.setData('text/html', '<p>设函数 $f(x)$ 在点 $a$ 处。</p><p><code>$$f(x)=x^2$$</code></p>');
            el.dispatchEvent(new ClipboardEvent('paste',{bubbles:true,cancelable:true,clipboardData:data}));
        });await wait();
        assert.equal(await page.locator('.katex').count(),3,`Rich clipboard math did not render: ${await value()}`);
        await load(String.raw`已有 \$f(x)\$`+'\n\n`$$x^2$$`');
        const unrepaired=await value();
        await page.evaluate(()=>window.msbridge.repairLatex());await wait();
        assert.equal(await page.locator('.katex').count(),2,'Explicit repair renders existing formulas');
        await page.keyboard.press('Control+z');await wait();
        assert.equal(await value(),unrepaired,'Explicit repair is undoable');
        await load('Source');
        await page.evaluate(() => window.msbridge.toggleMode());
        await page.locator('.vditor-sv').click();await page.keyboard.press('Control+End');
        await page.keyboard.type(String.raw` \$y\$`, {delay:25});await wait();
        assert.ok((await value()).includes('$y$') && !(await value()).includes(String.raw`\$y`), `Source typed: ${await value()}`);
        await page.keyboard.type(' tail');await wait();
        assert.match(await value(), /\$y\$ tail/);
        await page.evaluate(() => window.msbridge.pasteText('\n```tex\n$$z^2$$\n```'));await wait();
        assert.ok(!(await value()).includes('```') && (await value()).includes('z^2'), `Source paste: ${await value()}`);
        await page.keyboard.press('Control+End');
        await page.keyboard.type('\n```tex\n$$w^2$$', {delay:25});await wait();
        assert.ok(!(await value()).includes('```') && !(await value()).includes('mswCaret') && (await value()).includes('w^2'), `Source typed fence: ${await value()}`);
        await page.evaluate(() => window.msbridge.toggleMode());await wait();
        assert.equal(await page.locator('[data-type="math-block"][data-block="0"]').count(),2);
        assert.deepEqual(errors, []);
        console.log('18 LaTeX editor workflow checks passed.');
    } finally { await browser.close(); await new Promise(resolve => server.close(resolve)); }
})().catch(error => { console.error(error); process.exitCode = 1; });
