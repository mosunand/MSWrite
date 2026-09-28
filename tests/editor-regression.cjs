// Run with Node and Playwright available through NODE_PATH.
// Uses the real bundled editor and trusted browser keyboard/mouse events.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');

const webRoot = path.resolve(__dirname, '../resources/web');
const root = '.vditor-ir pre.vditor-reset';
const math = `${root} > [data-type="math-block"]`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
    const server = http.createServer(async (req, res) => {
        try {
            const name = path.resolve(webRoot, '.' + decodeURIComponent(new URL(req.url, 'http://localhost').pathname));
            if (!name.startsWith(webRoot + path.sep)) throw Error('Invalid path');
            const data = await fs.readFile(name);
            res.setHeader('Content-Type', ({ '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
                '.svg': 'image/svg+xml', '.png': 'image/png', '.woff2': 'font/woff2' })[path.extname(name)] || 'application/octet-stream');
            res.end(data);
        } catch { res.writeHead(404).end(); }
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    let browser;
    try {
        const executablePath = process.env.MSWRITE_BROWSER || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
        browser = await chromium.launch({ executablePath, headless: true });
        const page = await browser.newPage({ viewport: { width: 1100, height: 760 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.stack));
        await page.addInitScript(() => {
            window.chrome.webview = { postMessage: message => (window.__messages ||= []).push(message) };
        });
        await page.goto(`http://127.0.0.1:${server.address().port}/editor.html`);
        await page.waitForFunction(() => window.__messages?.some(m => m.t === 'ready'));
        const load = async md => {
            await page.evaluate(value => window.msbridge.setContent(value), md);
            await pause(450);
        };
        const value = () => page.evaluate(() => window.mswValue());
        const caret = () => page.evaluate(() => {
            const selection = getSelection();
            const node = selection.anchorNode;
            const el = node.nodeType === 1 ? node : node.parentElement;
            const block = el.closest('[data-block="0"]');
            return { y: scrollY, text: block?.textContent, tag: block?.tagName,
                type: block?.getAttribute('data-type') };
        });
        let passed = 0;
        const test = async (name, run) => {
            if (process.env.MSWRITE_TEST_FILTER && !name.includes(process.env.MSWRITE_TEST_FILTER)) return;
            const errorCount = errors.length;
            await run();
            assert.deepEqual(errors.slice(errorCount), [], `Browser errors in ${name}`);
            passed++;
            console.log(`PASS ${name}`);
        };
        await test('Initial content, theme and line numbers render in the first editor instance', async () => {
            const boot = await browser.newPage();
            await boot.addInitScript(() => {
                window.chrome.webview = { postMessage: m => (window.__messages ||= []).push(m) };
                window.msInitialState = { content: '# Initial document\n\nFirst paint content.', theme: 'dark', fontSize: 18, lineNumbers: true };
            });
            await boot.goto(`http://127.0.0.1:${server.address().port}/editor.html`);
            await boot.waitForFunction(() => window.__messages?.some(m => m.t === 'ready'));
            assert.equal(await boot.locator('h1').innerText(), 'Initial document');
            assert.equal(await boot.evaluate(() => document.body.dataset.msTheme), 'dark');
            assert.equal(await boot.evaluate(() => document.body.classList.contains('ms-no-lineno')), false);
            assert.ok(!(await boot.evaluate(() => window.__messages)).some(m => m.t === 'changed'), 'Loading marks document dirty');
            await boot.close();
        });
        const lead = '$$\n\n$$\n\n' + Array.from({ length: 50 }, (_, i) => `Paragraph ${i}`).join('\n\n');
        const tail = Array.from({ length: 25 }, (_, i) => `Tail ${i}`).join('\n\n');

        for (let level = 1; level <= 6; level++) {
            for (const position of ['start', 'middle', 'end']) {
                await test(`H${level} Enter at ${position} with earlier empty formulas`, async () => {
                    await load(`${lead}\n\n${'#'.repeat(level)} Heading target\n\n${tail}`);
                    await page.locator(`h${level}`).click();
                    await page.keyboard.press(position === 'start' ? 'Home' : 'End');
                    if (position === 'middle') await page.keyboard.press('ArrowLeft');
                    const before = await caret();
                    await page.keyboard.press('Enter');
                    await page.keyboard.type('LOCAL');
                    await pause(250);
                    const after = await caret();
                    assert.ok(Math.abs(after.y - before.y) < 120, JSON.stringify({ before, after }));
                    assert.equal(after.type, null, 'Caret moved into an unrelated formula');
                    assert.ok(after.text.includes('LOCAL'));
                    assert.equal(await page.locator(`${root} wbr`).count(), 0);
                    assert.equal(await page.locator(`${math} code.language-math`).first().textContent(), '\n');
                });
            }
        }
        await test('Formatted heading, repeated Enter, Backspace and undo remain local', async () => {
            await load(`${lead}\n\n# **Heading target**\n\n${tail}`);
            await page.locator('h1').click();
            await page.keyboard.press('End');
            const before = await caret();
            await page.keyboard.press('Enter');
            await page.keyboard.press('Enter');
            await page.keyboard.type('LOCAL');
            await page.keyboard.press('Backspace');
            await pause(1000);
            await page.keyboard.press('Control+z');
            await pause(250);
            assert.ok(Math.abs((await caret()).y - before.y) < 180);
            assert.equal((await caret()).type, null);
        });
        await test('Empty line above a code block can be clicked and typed into', async () => {
            await load('# Heading\n\n```js\nconst x = 1;\n```\n\nAfter');
            await page.locator('h1').click();
            await page.keyboard.press('End');
            await page.keyboard.press('Enter');
            const empty = page.locator(`${root} > p`).first();
            assert.ok((await empty.boundingBox()).height >= 20);
            await page.locator(`${root} > p`).last().click();
            await empty.click();
            await page.keyboard.type('ABOVE');
            assert.ok((await value()).includes('ABOVE\n\n```js'));
        });
        for (const width of [1100, 520]) {
            await test(`Formula shortcut, typing, preview and reopen at width ${width}`, async () => {
                await page.setViewportSize({ width, height: 760 });
                await load('Before\n\nInsert here\n\nAfter');
                await page.locator(`${root} > p`).nth(1).click();
                await page.keyboard.press('End');
                await page.keyboard.press('Control+Shift+m');
                assert.equal(await page.locator(math).count(), 1);
                await page.keyboard.type('x^2+1');
                await pause(500);
                assert.ok((await value()).includes('$$\nx^2+1\n$$'));
                const editing = await page.locator(math).boundingBox();
                const preview = await page.locator(`${math} .vditor-ir__preview`).boundingBox();
                const source = await page.locator(`${math} pre.vditor-ir__marker--pre`).boundingBox();
                assert.ok(preview && preview.height > 10 && preview.y >= source.y + source.height);
                assert.ok(editing.height < 110, `Single-line editing height: ${editing.height}`);
                await page.locator(`${root} > p`).last().click();
                await page.waitForFunction(() => document.querySelector('[data-type="math-block"] .katex'));
                assert.ok((await page.locator(math).boundingBox()).height < 64);
                await page.locator(math).click();
                await page.keyboard.press('End');
                await page.keyboard.type('+2');
                await pause(500);
                assert.ok((await value()).includes('x^2+1+2'));
            });
        }
        await page.setViewportSize({ width: 1100, height: 760 });
        await test('Formula insertion does not steal focus after a later click', async () => {
            await load('Before\n\nInsert here\n\nAfter');
            await page.locator(`${root} > p`).nth(1).click();
            await page.keyboard.press('Control+Shift+m');
            await page.locator(`${root} > p`).last().click();
            await page.keyboard.press('End');
            await pause(600);
            await page.keyboard.type(' LOCAL');
            assert.ok((await value()).includes('After LOCAL'));
        });
        await test('Selected formula text is wrapped once and undo restores it', async () => {
            await load('Before\n\nx_1 < y_2\n\nAfter');
            await page.locator(`${root} > p`).nth(1).click();
            await page.keyboard.press('Home');
            await page.keyboard.press('Shift+End');
            await page.keyboard.press('Control+Shift+m');
            await pause(550);
            assert.equal((await value()).match(/x_1 < y_2/g)?.length, 1);
            assert.ok((await value()).includes('$$\nx_1 < y_2\n$$'));
            await page.keyboard.press('Control+z');
            assert.equal((await value()).trim(), 'Before\n\nx_1 < y_2\n\nAfter');
        });
        await test('Formula shortcut works in a new empty document', async () => {
            await load('');
            await page.locator(root).click({ position: { x: 180, y: 20 } });
            await page.keyboard.press('Control+Shift+m');
            await page.keyboard.type('a=b');
            assert.equal((await value()).trim(), '$$\na=b\n$$');
        });
        await test('Formula source-mode insertion preserves the selected text on reopening', async () => {
            await load('Before\n\nPick\n\nAfter');
            await page.evaluate(() => window.msbridge.toggleMode());
            const source = page.locator('.vditor-sv [data-type="text"]').filter({ hasText: 'Pick' });
            await source.click();
            await page.keyboard.press('Home');
            await page.keyboard.press('Shift+End');
            await page.keyboard.press('Control+Shift+m');
            await pause(300);
            assert.ok((await value()).includes('$$\nPick\n$$'));
            await page.evaluate(() => window.msbridge.toggleMode());
            await page.waitForSelector(math);
            assert.equal(await page.locator(math).count(), 1);
            assert.equal((await value()).match(/Pick/g)?.length, 1);
        });
        const sourceSelector = `${math} pre.vditor-ir__marker--pre code`;
        const newFormula = async () => {
            await load('Before\n\nAfter');
            await page.locator(`${root} > p`).first().click();
            await page.keyboard.press('End');
            await page.keyboard.press('Enter');
            await page.keyboard.press('Control+Shift+m');
        };
        const paste = text => page.evaluate(text => {
            const data = new DataTransfer();
            data.setData('text/plain', text);
            document.activeElement.dispatchEvent(new ClipboardEvent('paste', {
                bubbles: true, cancelable: true, clipboardData: data
            }));
        }, text);
        const expectFormula = async text => {
            await page.waitForFunction(() => !!document.querySelector('[data-block="0"][data-type="math-block"] .katex'));
            assert.equal((await page.locator(sourceSelector).textContent()).trim(), text.trim());
            assert.equal(await page.locator(`${math} .katex-error, ${math} .vditor-reset--error`).count(), 0);
            assert.ok(await page.locator(`${math} .vditor-ir__preview`).isVisible());
            assert.equal(/\$\$\n([\s\S]*?)\n\$\$/.exec(await value())?.[1].trim(), text.trim());
        };
        await test('Shortcut consumes the empty paragraph without moving surrounding text', async () => {
            await newFormula();
            assert.deepEqual(await page.locator(`${root} > p`).allTextContents(), ['Before', 'After']);
            assert.equal(await page.locator(math).count(), 1);
            assert.ok((await page.locator(math).boundingBox()).height < 55);
            await page.keyboard.type('x=1');
            await expectFormula('x=1');
        });
        await test('Shortcut replacement is one undoable action with redo', async () => {
            await newFormula();
            await pause(900);
            await page.keyboard.press('Control+z');
            assert.equal(await page.locator(math).count(), 0);
            assert.deepEqual(await page.locator(`${root} > p`).allTextContents(), ['Before', '', 'After']);
            await page.keyboard.press('Control+y');
            assert.equal(await page.locator(math).count(), 1);
            assert.deepEqual(await page.locator(`${root} > p`).allTextContents(), ['Before', 'After']);
        });
        await test('Manually typed dollar fences create the same block with a live preview', async () => {
            await load('Before\n\nAfter');
            await page.locator(`${root} > p`).first().click();
            await page.keyboard.press('End');
            await page.keyboard.press('Enter');
            await page.keyboard.type('$$ x^2 $$');
            await expectFormula('x^2');
            assert.equal(await page.locator(math).count(), 1);
            assert.equal(await page.locator(`${root} .ms-mi`).count(), 0);
        });
        await test('Typing single-dollar inline math still renders inline', async () => {
            await load('Before');
            await page.locator(`${root} > p`).click();
            await page.keyboard.press('End');
            await page.keyboard.type(' $x^2$ after');
            await page.waitForSelector(`${root} [data-type="inline-node"] .katex`);
            assert.equal(await page.locator(math).count(), 0);
            assert.equal((await value()).trim(), 'Before $x^2$ after');
        });
        const cases = String.raw`f(x)=\begin{cases}
x^2 & x < 0 \\
\frac{1}{x} & x > 0
\end{cases}`;
        const matrix = String.raw`A=\begin{pmatrix}
1 & 2 & 3 \\
4 & 5 & 6 \\
7 & 8 & 9
\end{pmatrix}`;
        const tall = '\\left\\{\\begin{aligned}\n' + Array.from({ length: 12 }, (_, i) =>
            `x_{${i}} &= \\frac{a_{${i}}}{b_{${i}}}`).join(' \\\\\n') + '\n\\end{aligned}\\right.';
        for (const [name, text] of [['cases', cases], ['matrix', matrix], ['tall aligned brace', tall]]) {
            await test(`Multiline ${name} paste preserves every character and survives reopening`, async () => {
                await newFormula();
                await paste(text);
                await expectFormula(text);
                const saved = await value();
                await load(saved);
                await page.locator(`${math} .vditor-ir__preview`).click();
                await expectFormula(text);
                assert.equal(await value(), saved);
            });
        }
        await test('Formula paste is one undoable action and redo restores all lines', async () => {
            await newFormula();
            const before = await value();
            await paste(cases);
            await expectFormula(cases);
            await pause(1000);
            await page.keyboard.press('Control+z');
            assert.equal(await value(), before);
            await page.keyboard.press('Control+y');
            await expectFormula(cases);
        });
        await test('Host paste accepts complete fences and replaces selected formula source', async () => {
            await newFormula();
            await page.evaluate(text => msbridge.pasteText('$$\r\n' + text.replace(/\n/g, '\r\n') + '\r\n$$'), cases);
            await expectFormula(cases);
            await page.keyboard.press('Control+a');
            await page.evaluate(text => msbridge.pasteText(text), matrix);
            await expectFormula(matrix);
            assert.deepEqual(await page.locator(`${root} > p`).allTextContents(), ['Before', 'After']);
        });
        await test('Multiline beforeinput preserves LaTeX and the following typing position', async () => {
            await newFormula();
            await page.keyboard.insertText(tall);
            await expectFormula(tall);
            await page.keyboard.type('+z');
            await expectFormula(tall + '+z');
        });
        await test('Incomplete formula keeps its source and renders after correction', async () => {
            await newFormula();
            await page.keyboard.type('\\frac{a}{');
            await page.waitForSelector(`${math} .katex-error, ${math} .vditor-reset--error`);
            assert.equal((await page.locator(sourceSelector).textContent()).trim(), '\\frac{a}{');
            await page.keyboard.type('b}');
            await expectFormula('\\frac{a}{b}');
            await page.keyboard.press('Control+a');
            await page.keyboard.press('Backspace');
            await page.waitForSelector(`${math}.ms-math-empty`);
            assert.equal(await page.locator(`${math} .vditor-ir__preview`).isVisible(), false);
        });
        for (const width of [1100, 520]) {
            await test(`Tall and wide formulas remain fully accessible at width ${width}`, async () => {
                await page.setViewportSize({ width, height: 760 });
                await newFormula();
                await paste(tall);
                await expectFormula(tall);
                const geometry = await page.locator(`${math} .vditor-ir__preview`).evaluate(el => {
                    const box = el.getBoundingClientRect();
                    return { height: box.height, bottom: box.bottom, top: box.top,
                        svg: [...el.querySelectorAll('svg')].map(s => {
                            const r = s.getBoundingClientRect(); return { top: r.top, bottom: r.bottom };
                        }) };
                });
                assert.ok(geometry.height > 450);
                assert.ok(geometry.svg.length >= 2);
                assert.ok(geometry.svg.every(s => s.top >= geometry.top - 1 && s.bottom <= geometry.bottom + 1));
                await page.keyboard.press('Control+a');
                const wide = Array.from({ length: 30 }, (_, i) => `x_{${i}}`).join('+');
                await paste(wide);
                await expectFormula(wide);
                const scroll = await page.locator(`${math} .vditor-ir__preview`).evaluate(el => {
                    const content = el.querySelector('.katex-html');
                    const left = content.getBoundingClientRect().left - el.getBoundingClientRect().left;
                    el.scrollLeft = el.scrollWidth;
                    return { left, moved: el.scrollLeft, end: content.getBoundingClientRect().right,
                        right: el.getBoundingClientRect().right,
                        documentWidth: document.documentElement.scrollWidth, windowWidth: innerWidth };
                });
                assert.ok(scroll.left >= -1, JSON.stringify(scroll));
                assert.ok(scroll.moved > 0);
                assert.ok(scroll.end <= scroll.right + 1, JSON.stringify(scroll));
                assert.ok(scroll.documentWidth <= scroll.windowWidth + 1);
            });
        }
        await page.setViewportSize({ width: 1100, height: 760 });
        const mathSource = () => page.locator(sourceSelector).first().textContent();
        const colorFormula = (text, color) => `\\textcolor{${color}}{\n${text}\n}`;
        const boldFormula = text => `\\pmb{\n${text}\n}`;
        const loadFormula = async text => load(`Before\n\n$$\n${text}\n$$\n\nAfter`);
        const contextOnFormula = async (index = 0) => {
            await page.locator(`${math} .vditor-ir__preview`).nth(index).click({ button: 'right' });
            return page.evaluate(() => window.__messages.filter(m => m.t === 'contextMenu').at(-1));
        };
        const waitMathSource = async text => {
            await page.waitForFunction(expected => {
                const el = document.querySelector('[data-block="0"][data-type="math-block"] .vditor-ir__preview .language-math');
                return el?.getAttribute('data-math')?.trim() === expected.trim() && el.querySelector('.katex');
            }, text);
            assert.equal((await mathSource()).trim(), text.trim());
            assert.equal(await page.locator(`${math} .vditor-reset--error`).count(), 0);
        };
        await test('Right-clicking a finished formula colors only that block without HTML in LaTeX', async () => {
            await loadFormula(cases);
            await page.locator(`${root} > p`).last().click();
            assert.deepEqual(await contextOnFormula(), { t: 'contextMenu', math: true, mathBold: false });
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await waitMathSource(colorFormula(cases, '#e74c3c'));
            const paint = await page.locator(`${math} .katex`).evaluate(el => {
                const symbol = el.querySelector('.mord');
                return getComputedStyle(symbol).color;
            });
            assert.equal(paint, 'rgb(231, 76, 60)');
            assert.deepEqual(await page.locator(`${root} > p`).allTextContents(), ['Before', 'After']);
            assert.equal(await page.locator(`${math}.vditor-ir__node--expand`).count(), 0);
            assert.ok(!(await mathSource()).includes('<font'));
        });
        await test('Formula recoloring replaces one wrapper; clearing restores the original source', async () => {
            await loadFormula(tall);
            await contextOnFormula();
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await page.evaluate(() => msbridge.applyColor('#3498db'));
            await waitMathSource(colorFormula(tall, '#3498db'));
            const before = await value();
            await page.evaluate(() => msbridge.applyColor('#3498db'));
            assert.equal(await value(), before);
            await page.evaluate(() => msbridge.applyColor('clear'));
            await waitMathSource(tall);
        });
        await test('Formula bold menu and Ctrl+B toggle all matrix cells without asterisks', async () => {
            await loadFormula(matrix);
            await contextOnFormula();
            await page.evaluate(() => msbridge.toggleMathBold());
            await waitMathSource(boldFormula(matrix));
            assert.equal((await contextOnFormula()).mathBold, true);
            assert.ok(await page.locator(`${math} [style*="text-shadow"]`).count() > 0);
            await page.keyboard.press('Control+b');
            await waitMathSource(matrix);
            assert.equal((await contextOnFormula()).mathBold, false);
            assert.equal(await page.locator(`${math} [style*="text-shadow"]`).count(), 0);
        });
        await test('Color and bold combine in either order and clear independently', async () => {
            for (const first of ['color', 'bold']) {
                await loadFormula(cases);
                await contextOnFormula();
                if (first === 'color') {
                    await page.evaluate(() => msbridge.applyColor('#e74c3c'));
                    await page.keyboard.press('Control+b');
                } else {
                    await page.evaluate(() => msbridge.toggleMathBold());
                    await page.evaluate(() => msbridge.applyColor('#e74c3c'));
                }
                await waitMathSource(colorFormula(boldFormula(cases), '#e74c3c'));
                await page.evaluate(() => msbridge.applyColor('clear'));
                await waitMathSource(boldFormula(cases));
                await page.keyboard.press('Control+b');
                await waitMathSource(cases);
            }
        });
        await test('Formatting selected source styles the whole formula and preserves the edit position', async () => {
            await loadFormula(cases);
            await page.locator(`${math} .vditor-ir__preview`).click();
            await page.locator(sourceSelector).evaluate(source => {
                const text = source.firstChild;
                const range = document.createRange();
                range.setStart(text, text.textContent.indexOf('x^2'));
                range.setEnd(text, range.startOffset + 3);
                getSelection().removeAllRanges(); getSelection().addRange(range);
            });
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await waitMathSource(colorFormula(cases, '#e74c3c'));
            assert.equal(await page.evaluate(() => getSelection().toString()), 'x^2');
            assert.ok(await page.locator(`${math} pre.vditor-ir__marker--pre`).isVisible());
            await page.keyboard.type('x^3');
            await waitMathSource(colorFormula(cases.replace('x^2', 'x^3'), '#e74c3c'));
        });
        await test('Each formula style change is undoable once after rendering, with redo', async () => {
            await loadFormula(tall);
            const plain = await value();
            await contextOnFormula();
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await waitMathSource(colorFormula(tall, '#e74c3c'));
            const colored = await value();
            await pause(1000);
            await page.keyboard.press('Control+z');
            assert.equal(await value(), plain);
            await page.keyboard.press('Control+y');
            await waitMathSource(colorFormula(tall, '#e74c3c'));
            assert.equal(await value(), colored);
            await contextOnFormula();
            await page.evaluate(() => msbridge.toggleMathBold());
            await waitMathSource(colorFormula(boldFormula(tall), '#e74c3c'));
            await pause(1000);
            await page.evaluate(() => msbridge.editUndo());
            assert.equal(await value(), colored);
            await page.evaluate(() => msbridge.editRedo());
            await waitMathSource(colorFormula(boldFormula(tall), '#e74c3c'));
        });
        await test('Formatting the second of two identical formulas keeps the first intact', async () => {
            await load(`$$\n${cases}\n$$\n\nBetween\n\n$$\n${cases}\n$$\n\nAfter`);
            await contextOnFormula(1);
            await page.evaluate(() => msbridge.applyColor('#3498db'));
            assert.equal((await page.locator(sourceSelector).nth(0).textContent()).trim(), cases);
            assert.equal((await page.locator(sourceSelector).nth(1).textContent()).trim(), colorFormula(cases, '#3498db'));
        });
        await test('Comments and escaped braces survive applying and removing formula styles', async () => {
            const tex = String.raw`\{x\} + \frac{a}{b} % comment with unmatched {`;
            await loadFormula(tex);
            await contextOnFormula();
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await page.keyboard.press('Control+b');
            await waitMathSource(colorFormula(boldFormula(tex), '#e74c3c'));
            await page.evaluate(() => msbridge.applyColor('clear'));
            await page.keyboard.press('Control+b');
            await waitMathSource(tex);
        });
        await test('Incomplete or cross-block formula selections cannot be corrupted by styling', async () => {
            await loadFormula('\\frac{a}{');
            const incomplete = await value();
            await contextOnFormula();
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            assert.equal(await value(), incomplete);
            await page.evaluate(() => msbridge.toggleMathBold());
            assert.equal(await value(), incomplete);
            await loadFormula(cases);
            const before = await value();
            await page.locator(root).evaluate(el => {
                const range = document.createRange(); range.selectNodeContents(el);
                getSelection().removeAllRanges(); getSelection().addRange(range);
            });
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await page.keyboard.press('Control+b');
            assert.equal(await value(), before);
        });
        await test('Formula context does not leak into a later paragraph, document or code block', async () => {
            await loadFormula(cases);
            await contextOnFormula();
            await page.locator(`${root} > p`).last().click();
            await page.keyboard.press('Home'); await page.keyboard.press('Shift+End');
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            assert.equal((await mathSource()).trim(), cases);
            assert.ok((await value()).includes('<font color="#e74c3c">After</font>'));
            await contextOnFormula();
            await load('New text');
            await page.locator(`${root} > p`).click();
            await page.keyboard.press('Control+b');
            assert.equal((await value()).trim(), '**New text**');
            await load('```js\nconst x = 1;\n```');
            await page.locator(`${root} > [data-type="code-block"]`).click();
            const code = await value();
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await page.keyboard.press('Control+b');
            assert.equal(await value(), code);
        });
        await test('Styled formulas retain their rendering on reopening, source-mode round trips and export', async () => {
            await loadFormula(tall);
            await contextOnFormula();
            await page.evaluate(() => msbridge.applyColor('#e74c3c'));
            await page.keyboard.press('Control+b');
            const styled = colorFormula(boldFormula(tall), '#e74c3c');
            await waitMathSource(styled);
            const saved = await value();
            await load(saved);
            await waitMathSource(styled);
            await page.evaluate(() => msbridge.toggleMode());
            await page.evaluate(() => msbridge.toggleMode());
            await waitMathSource(styled);
            assert.equal(await value(), saved);
            await page.evaluate(() => { window.__messages = []; msbridge.requestHtml(); });
            await page.waitForFunction(() => window.__messages.some(m => m.t === 'html'));
            const exported = await page.evaluate(() => {
                const el = document.createElement('div');
                el.innerHTML = window.__messages.find(m => m.t === 'html').html;
                return { source: el.querySelector('.language-math')?.getAttribute('data-math').trim(),
                    color: el.querySelector('[style*="color:"]')?.style.color,
                    bold: !!el.querySelector('[style*="text-shadow:"]'),
                    error: !!el.querySelector('.katex-error,.vditor-reset--error') };
            });
            assert.equal(exported.source, styled);
            assert.equal(exported.color, 'rgb(231, 76, 60)');
            assert.equal(exported.bold, true);
            assert.equal(exported.error, false);
        });
        await test('Complex formulas keep their source through source mode and export', async () => {
            await load([cases, matrix, tall].map(text => '$$\n' + text + '\n$$').join('\n\n'));
            const before = await value();
            await page.evaluate(() => msbridge.toggleMode());
            assert.equal((await value()).trimEnd(), before.trimEnd());
            await page.evaluate(() => msbridge.toggleMode());
            await page.waitForSelector(math);
            await page.evaluate(() => { window.__messages = []; msbridge.requestHtml(); });
            await page.waitForFunction(() => window.__messages.some(m => m.t === 'html'));
            const exported = await page.evaluate(() => {
                const el = document.createElement('div');
                el.innerHTML = window.__messages.find(m => m.t === 'html').html;
                return { sources: [...el.querySelectorAll('.language-math')].map(m => m.getAttribute('data-math').trim()),
                    count: el.querySelectorAll('.katex-display').length,
                    errors: el.querySelectorAll('.katex-error,.vditor-reset--error').length };
            });
            assert.deepEqual(exported.sources, [cases, matrix, tall]);
            assert.equal(exported.count, 3);
            assert.equal(exported.errors, 0);
            assert.equal((await value()).trimEnd(), before.trimEnd());
        });
        await test('Export contains rendered math, highlighted code and embedded local images', async () => {
            await load('# Title\n\nInline $x^2$\n\n$$\n\\frac{a}{b}\n$$\n\n```js\nconst x = 1;\n```\n\n![Local](vditor/dist/images/emoji/b3log.png)');
            const before = await value();
            await page.evaluate(() => { window.__messages = []; window.msbridge.requestHtml(); });
            await page.waitForFunction(() => window.__messages.some(m => m.t === 'html'));
            const exported = await page.evaluate(() => {
                const html = window.__messages.filter(m => m.t === 'html').at(-1).html;
                const holder = document.createElement('div');
                holder.innerHTML = html;
                return { math: holder.querySelectorAll('.katex').length,
                    display: holder.querySelectorAll('.katex-display').length,
                    syntax: holder.querySelectorAll('.hljs-keyword').length,
                    image: holder.querySelector('img').getAttribute('src'),
                    ui: holder.querySelectorAll('[contenteditable],wbr,.vditor-ir__marker').length };
            });
            assert.equal(exported.math, 2);
            assert.equal(exported.display, 1);
            assert.equal(exported.syntax, 1);
            assert.match(exported.image, /^data:image\/png;base64,/);
            assert.equal(exported.ui, 0);
            assert.equal(await value(), before, 'Export must not change the document');
        });
        await test('Caret movement cannot cancel pending title and outline updates', async () => {
            await load('# Opening title\n\nBody');
            await page.locator('h1').click();
            await page.keyboard.press('End');
            await page.evaluate(() => { window.__messages = []; });
            await page.keyboard.type(' revised');
            // Vditor debounces input delivery; move the caret after its final
            // value reaches the bridge but before our 300 ms content timer.
            await page.waitForFunction(() => window.mswFirstLine().includes('revised'));
            await page.evaluate(() => {
                document.dispatchEvent(new Event('selectionchange'));
            });
            await page.waitForFunction(() => window.__messages.some(m => m.t === 'firstLine' && m.text.includes('revised')));
            const messages = await page.evaluate(() => window.__messages);
            assert.ok(messages.some(m => m.t === 'firstLine' && m.text.includes('revised')), JSON.stringify(messages));
            assert.ok(messages.some(m => m.t === 'outline' && m.items.some(i => i.text.includes('revised'))));
        });
        await test('Unchanged active code reuses highlighting across layout refreshes', async () => {
            await load('```javascript\nconst performanceMarker = 42;\nconsole.log(performanceMarker);\n```\n\nTail');
            await page.locator(`${root} [data-type="code-block"]`).click();
            await pause(800);
            assert.ok(await page.locator('.ms-hi-col').count(), 'Fixture must enter highlighted code editing');
            await page.evaluate(() => {
                window.__highlightCalls = 0;
                window.__originalHighlight = window.hljs.highlight;
                window.hljs.highlight = function (...args) {
                    window.__highlightCalls++;
                    return window.__originalHighlight.apply(this, args);
                };
            });
            for (let i = 0; i < 8; i++) {
                await page.evaluate(() => window.msbridge.resyncDecor());
                await pause(80);
            }
            const calls = await page.evaluate(() => {
                window.hljs.highlight = window.__originalHighlight;
                return window.__highlightCalls;
            });
            console.log(`HIGHLIGHT_CALLS_FOR_8_LAYOUT_REFRESHES ${calls}`);
            assert.equal(calls, 0, 'Scrolling/layout must not rerun syntax highlighting for unchanged code');
        });
        await test('Title snapshot bounds large documents and export carries its request identity', async () => {
            await load('**Current title**\n\n' + 'Long body '.repeat(8000));
            assert.equal(await page.evaluate(() => window.mswTitleSource().length), 65536);
            await page.evaluate(() => { window.__messages = []; window.msbridge.requestHtml(913); });
            await page.waitForFunction(() => window.__messages.some(m => m.t === 'html' && m.request === 913));
            const snapshot = await page.evaluate(() => window.__messages.find(m => m.t === 'html'));
            assert.ok(snapshot.titleSource.startsWith('**Current title**'));
            assert.equal(snapshot.titleSource.length, 65536);
        });
        assert.deepEqual(errors, []);
        console.log(`${passed} browser regression checks passed.`);
    } finally {
        await browser?.close();
        await new Promise(resolve => server.close(resolve));
    }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
