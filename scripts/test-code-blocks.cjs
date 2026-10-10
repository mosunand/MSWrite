// Run with Node.js and Playwright installed (or available through NODE_PATH):
// node scripts/test-code-blocks.cjs
// Uses installed Edge by default. MSWRITE_BROWSER_PATH can select another Chromium executable.
// Test-only hooks are injected by the local server; production assets stay unchanged.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright');

const webRoot = path.resolve(__dirname, '../resources/web');
const hooks = `window.codeTests = {
    editLangInfo, removeEmptyCodeBlock, focusLangInfo, openLangPicker, resetLangPicker, createEditor,
    placeCaretInBlock, recordMathUndo, sendImage,
    get root() { return editorRoot(); }, get editor() { return vd; },
    get ready() { return vdReady; }, get pickerOpen() { return langPickerOpen; }
};`;
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
    '.json': 'application/json', '.svg': 'image/svg+xml', '.woff2': 'font/woff2' };
const server = http.createServer((req, res) => {
    try {
        const pathname = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
        const file = path.resolve(webRoot, '.' + pathname);
        if (!file.startsWith(webRoot + path.sep)) { res.writeHead(403).end(); return; }
        let body = fs.readFileSync(file);
        if (path.basename(file) === 'bridge.js') {
            const source = body.toString('utf8');
            const index = source.lastIndexOf('})();');
            assert.ok(index >= 0, 'Bridge closure missing');
            body = Buffer.from(source.slice(0, index) + hooks + source.slice(index));
        }
        res.writeHead(200, { 'Content-Type': mime[path.extname(file)] || 'application/octet-stream' });
        res.end(body);
    } catch (error) { res.writeHead(404).end(); }
});

async function main() {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const browser = await chromium.launch({ headless: true,
        ...(process.env.MSWRITE_BROWSER_PATH ? { executablePath: process.env.MSWRITE_BROWSER_PATH }
            : { channel: 'msedge' }) });
    const errors = [];
    let passed = 0;
    async function fixture(markdown = '测试\n\n和搞\n\n```python\n\n```\n\n之后') {
        const page = await browser.newPage({ viewport: { width: 900, height: 600 } });
        page.on('pageerror', error => errors.push(error.message));
        await page.addInitScript(content => {
            window.msInitialState = { theme: 'light', fontSize: 16,
                ...(typeof content === 'string' ? { content } : content) };
            window.hostMessages = [];
            window.chrome.webview = { postMessage: msg => window.hostMessages.push(msg) };
        }, markdown);
        await page.goto(`http://127.0.0.1:${server.address().port}/editor.html`);
        await page.waitForFunction(() => window.codeTests && codeTests.ready);
        return page;
    }
    async function language(page, start, end = start, text) {
        await page.evaluate(({ start, end, text }) => {
            const block = codeTests.root.querySelector('[data-type="code-block"]');
            block.classList.add('vditor-ir__node--expand');
            codeTests.openLangPicker(block);
            const info = block.querySelector('[data-type="code-block-info"]');
            if (text !== undefined) info.textContent = text;
            const node = info.firstChild || info.appendChild(document.createTextNode(''));
            const prefix = node.textContent.startsWith('\u200b') ? 1 : 0;
            const range = document.createRange();
            range.setStart(node, prefix + start); range.setEnd(node, prefix + end);
            getSelection().removeAllRanges(); getSelection().addRange(range);
            codeTests.root.focus({ preventScroll: true });
        }, { start, end, text });
    }
    async function codeCaret(page, index = 0) {
        await page.evaluate(index => {
            codeTests.resetLangPicker();
            const block = codeTests.root.querySelectorAll('[data-type="code-block"]')[index];
            block.classList.add('vditor-ir__node--expand');
            const source = block.querySelector('pre.vditor-ir__marker--pre > code');
            const range = document.createRange(); range.selectNodeContents(source); range.collapse(true);
            getSelection().removeAllRanges(); getSelection().addRange(range);
            codeTests.root.focus({ preventScroll: true });
        }, index);
    }
    async function state(page) {
        return page.evaluate(() => {
            const range = getSelection().rangeCount ? getSelection().getRangeAt(0) : null;
            const paragraph = range && (range.startContainer.nodeType === 1
                ? range.startContainer : range.startContainer.parentElement).closest('p');
            const prefix = document.createRange();
            if (paragraph) { prefix.selectNodeContents(paragraph); prefix.setEnd(range.startContainer, range.startOffset); }
            return {
                lang: (codeTests.root.querySelector('[data-type="code-block-info"]')?.textContent || '').replace(/\u200b/g, ''),
                blocks: codeTests.root.querySelectorAll('[data-type="code-block"]').length,
                beforeCaret: paragraph ? prefix.toString().replace(/\u200b/g, '') : null,
                md: window.mswValue(), scroll: window.scrollY
            };
        });
    }
    async function test(name, fn, markdown) {
        const page = await fixture(markdown);
        try { await fn(page); passed++; console.log('PASS ' + name); }
        catch (error) {
            console.error(name, await page.evaluate(() => ({
                errors: Array.from(document.querySelectorAll('.vditor-reset--error')).map(node => node.textContent),
                canvases: document.querySelectorAll('canvas').length,
                charts: Array.from(document.querySelectorAll('[class*="echarts"]')).map(node => ({
                    tag: node.tagName, class: node.className, html: node.innerHTML.slice(0, 700) }))
            })));
            throw error;
        }
        finally { await page.close(); }
    }
    async function testChat(name, text, verify) {
        const page = await browser.newPage();
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.addInitScript(() => {
                window.hostMessages = [];
                window.chrome.webview = { postMessage: msg => window.hostMessages.push(msg) };
            });
            await page.goto(`http://127.0.0.1:${server.address().port}/chat.html`);
            await page.waitForFunction(() => !!window.chatView);
            await page.evaluate(text => chatView.update({ reset: true, count: 1, sequence: 7,
                rows: [{ id: 0, kind: 1, text, finalized: true }], light: true, busy: false }), text);
            await page.waitForFunction(() => hostMessages.some(msg => msg.t === 'chatRendered' && msg.sequence === 7));
            await verify(page); passed++; console.log('PASS ' + name);
        } finally { await page.close(); }
    }
    try {
        await testChat('chat formatting and math work without dynamic script evaluation',
            '**正文**\n\n$x^2$\n\n```javascript\nconst value = 1;\n```', async page => {
                assert.equal(await page.locator('.content strong').innerText(), '正文');
                assert.ok(await page.locator('.content .katex').count());
                assert.ok(await page.locator('.content .hljs-keyword').count());
            });
        await testChat('chat model HTML cannot inject host-capable handlers or frames',
            '<img src="data:image/png;base64,broken" onerror="window.chatInjected=1">\n\n<iframe srcdoc="bad"></iframe>\n\n<a href="javascript:window.chatInjected=1" onclick="window.chatInjected=1">unsafe</a>', async page => {
                assert.equal(await page.evaluate(() => window.chatInjected), undefined);
                assert.equal(await page.locator('.content [onerror],.content [onclick],.content iframe').count(), 0);
                assert.equal(await page.locator('.content a[href^="javascript:"]').count(), 0);
            });
        await testChat('safe chat links still reach the native external-link handler',
            '[reference](https://example.com/paper)', async page => {
                await page.locator('.content a').click();
                assert.ok(await page.evaluate(() => hostMessages.some(msg => msg.t === 'chatLink' && msg.url === 'https://example.com/paper')));
            });
        await test('mode rebuilding queues content commands until ready', async page => {
            await page.evaluate(() => {
                msbridge.toggleMode(); msbridge.insertText(' queued text'); msbridge.requestContent(71);
            });
            await page.waitForFunction(() => hostMessages.some(msg => msg.t === 'content' && msg.request === 71));
            const content = await page.evaluate(() => hostMessages.find(msg => msg.t === 'content' && msg.request === 71).md);
            assert.ok(content.includes('original')); assert.ok(content.includes('queued text'));
        }, 'original');
        await test('failed content extraction preserves the active editor', async page => {
            const preserved = await page.evaluate(() => {
                const editor = codeTests.editor, getter = editor.getValue;
                editor.getValue = () => { throw new Error('test extraction failure'); };
                msbridge.toggleMode(); editor.getValue = getter;
                return codeTests.editor === editor && codeTests.ready;
            });
            assert.equal(preserved, true); assert.ok((await state(page)).md.includes('original'));
        }, 'original');
        await test('immediate native insertion after loading is marked changed', async page => {
            await page.evaluate(() => { msbridge.setContent('loaded'); msbridge.insertText(' immediate'); });
            await page.waitForFunction(() => hostMessages.some(msg => msg.t === 'changed' && msg.md.includes('immediate')));
            assert.ok((await state(page)).md.includes('loaded'));
        }, 'original');
        await test('real input ends the load suppression window', async page => {
            await page.evaluate(() => {
                msbridge.setContent('loaded'); codeTests.root.focus();
                codeTests.placeCaretInBlock(codeTests.root.querySelector('p'), true);
                codeTests.root.dispatchEvent(new InputEvent('beforeinput', { inputType: 'insertText', data: 'X', bubbles: true, cancelable: true }));
                document.execCommand('insertText', false, 'X');
            });
            await page.waitForFunction(() => hostMessages.some(msg => msg.t === 'changed' && msg.md.includes('loadedX')));
        }, 'original');
        await test('replace all preserves literal dollar replacements and undo', async page => {
            await page.evaluate(() => { msbridge.setContent('same same'); msbridge.replaceAll('same', '$&'); });
            assert.equal((await state(page)).md.trim(), '$& $&');
            assert.ok(await page.evaluate(() => hostMessages.some(msg => msg.t === 'changed' && msg.md.includes('$&'))));
            await page.evaluate(() => msbridge.editUndo());
            assert.equal((await state(page)).md.trim(), 'same same');
        }, 'original');
        await test('replace all stops if the undo snapshot fails', async page => {
            await page.evaluate(() => {
                codeTests.editor.vditor.undo.addToUndoStack = () => { throw new Error('test snapshot failure'); };
                msbridge.replaceAll('original', 'changed');
            });
            assert.equal((await state(page)).md.trim(), 'original');
            assert.ok(await page.evaluate(() => hostMessages.some(msg => msg.t === 'replaced' && msg.count === 0)));
        }, 'original');
        await test('oversized image is rejected before FileReader starts', async page => {
            await page.evaluate(() => codeTests.sendImage({ size: 33 * 1024 * 1024 }));
            assert.equal(await page.evaluate(() => hostMessages.some(msg => msg.t === 'saveImage')), false);
        });
        await test('only the application language picker appears', async page => {
            await language(page, 0, 6); await page.keyboard.press('Backspace');
            await page.evaluate(() => codeTests.root.dispatchEvent(new Event('input', { bubbles: true })));
            await page.waitForTimeout(280);
            assert.equal(await page.evaluate(() => getComputedStyle(codeTests.editor.vditor.hint.element).display), 'none');
            assert.equal(await page.locator('#ms-lang-suggest').isVisible(), true);
        });
        await test('inline document handlers cannot execute', async page => {
            await page.waitForTimeout(200);
            assert.equal(await page.evaluate(() => window.auditExecuted), undefined);
        }, '<img src="missing.png" onerror="window.auditExecuted = 1">');
        await test('diagram expressions cannot execute host-capable JavaScript', async page => {
            await page.waitForFunction(() => !!document.querySelector('.vditor-reset--error'));
            assert.equal(await page.evaluate(() => window.auditExecuted), undefined);
        }, '```echarts\n(()=>{window.auditExecuted=1; return {};})()\n```');
        await test('valid JSON chart still renders', async page => {
            await page.waitForSelector('.language-echarts canvas');
            assert.equal(await page.locator('.vditor-reset--error').count(), 0);
        }, '```echarts\n{"xAxis":{"data":["A","B"]},"yAxis":{},"series":[{"type":"bar","data":[1,2]}]}\n```');
        await test('math and Mermaid still render with script restrictions', async page => {
            await page.waitForSelector('.katex'); await page.waitForSelector('.language-mermaid svg');
        }, '$$\nx^2\n$$\n\n```mermaid\ngraph TD; A-->B;\n```');
        await test('recovery bootstrap preserves latest content and revision', async page => {
            await page.evaluate(() => {
                codeTests.root.focus(); codeTests.placeCaretInBlock(codeTests.root.querySelector('p'), true);
            });
            await page.keyboard.type(' updated');
            await page.waitForFunction(() => hostMessages.some(msg => msg.t === 'changed' && msg.md.includes('updated')));
            const snapshot = await page.evaluate(() => hostMessages.filter(msg => msg.t === 'changed').at(-1));
            const restored = await fixture({ content: snapshot.md, rev: snapshot.rev });
            try {
                assert.ok((await state(restored)).md.includes('updated'));
                await restored.evaluate(() => {
                    codeTests.root.focus(); codeTests.placeCaretInBlock(codeTests.root.querySelector('p'), true);
                });
                await restored.keyboard.type(' again');
                await restored.waitForFunction(rev => hostMessages.some(msg => msg.t === 'changed' && msg.rev > rev), snapshot.rev);
            } finally { await restored.close(); }
        }, '原稿');
        await test('shortcut replaces empty line without inserting a gap before adjacent code', async page => {
            await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                const paragraph = document.createElement('p'); paragraph.setAttribute('data-block', '0');
                paragraph.textContent = '\u200b'; block.after(paragraph);
                codeTests.root.focus(); codeTests.placeCaretInBlock(paragraph, false);
            });
            await page.keyboard.press('Control+Shift+k');
            const result = await page.evaluate(() => {
                const blocks = codeTests.root.querySelectorAll('[data-type="code-block"]');
                return { count: blocks.length, adjacent: blocks[0].nextElementSibling === blocks[1] };
            });
            assert.equal(result.count, 2); assert.equal(result.adjacent, true);
        }, '```python\nprint(1)\n```\n\n之后');
        await test('shortcut replaces empty line between paragraphs in place', async page => {
            await page.evaluate(() => {
                const first = codeTests.root.querySelector('p'); window.firstParagraph = first;
                const paragraph = document.createElement('p'); paragraph.setAttribute('data-block', '0');
                paragraph.textContent = '\u200b'; first.after(paragraph);
                codeTests.root.focus(); codeTests.placeCaretInBlock(paragraph, false);
            });
            await page.keyboard.press('Control+Shift+k');
            assert.deepEqual(await page.evaluate(() => Array.from(codeTests.root.children).map(node =>
                node.getAttribute('data-type') === 'code-block' ? 'CODE' : node.textContent.replace(/\u200b/g, ''))),
            ['之前', 'CODE', '之后']);
            assert.equal(await page.evaluate(() => firstParagraph.isConnected), true);
        }, '之前\n\n之后');
        await test('shortcut preserves other preexisting empty lines', async page => {
            await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                const previous = document.createElement('p'), current = document.createElement('p');
                previous.setAttribute('data-block', '0'); current.setAttribute('data-block', '0');
                previous.textContent = '\u200b'; current.textContent = '\u200b';
                window.otherEmptyLine = previous; block.after(previous); previous.after(current);
                codeTests.root.focus(); codeTests.placeCaretInBlock(current, false);
            });
            await page.keyboard.press('Control+Shift+k');
            assert.equal(await page.evaluate(() => otherEmptyLine.isConnected), true);
            assert.equal((await state(page)).blocks, 2);
        }, '```python\nprint(1)\n```\n\n之后');
        for (const offset of [0, 2, 4]) {
            await test('shortcut preserves text and inserts below at offset ' + offset, async page => {
                await page.evaluate(offset => {
                    const paragraph = codeTests.root.querySelector('p'); window.originalParagraph = paragraph;
                    const range = document.createRange(); range.setStart(paragraph.firstChild, offset); range.collapse(true);
                    getSelection().removeAllRanges(); getSelection().addRange(range); codeTests.root.focus();
                }, offset);
                await page.keyboard.press('Control+Shift+k');
                assert.deepEqual(await page.evaluate(() => ({ connected: originalParagraph.isConnected,
                    text: originalParagraph.textContent, nextType: originalParagraph.nextElementSibling.getAttribute('data-type') })),
                { connected: true, text: '保留原文', nextType: 'code-block' });
                assert.equal((await state(page)).blocks, 1);
            }, '保留原文\n\n之后');
        }
        await test('empty line shortcut undo and redo', async page => {
            await page.evaluate(() => {
                const paragraph = document.createElement('p'); paragraph.setAttribute('data-block', '0');
                paragraph.textContent = '\u200b'; codeTests.root.querySelector('p').after(paragraph);
                codeTests.root.focus(); codeTests.placeCaretInBlock(paragraph, false);
            });
            await page.keyboard.press('Control+Shift+k'); assert.equal((await state(page)).blocks, 1);
            await page.keyboard.press('Control+z'); await page.waitForTimeout(180);
            assert.equal((await state(page)).blocks, 0); assert.ok((await state(page)).md.includes('保留原文'));
            await page.keyboard.press('Control+y'); await page.waitForTimeout(180);
            assert.equal((await state(page)).blocks, 1);
        }, '保留原文\n\n之后');
        await test('shortcut ignores external input focus', async page => {
            await page.evaluate(() => {
                const paragraph = codeTests.root.querySelector('p'); codeTests.placeCaretInBlock(paragraph, true);
                const input = document.createElement('input'); input.id = 'outside'; input.value = 'keep';
                document.body.appendChild(input); input.focus();
            });
            await page.keyboard.press('Control+Shift+k');
            assert.equal((await state(page)).blocks, 0); assert.equal(await page.locator('#outside').inputValue(), 'keep');
        }, '保留原文');
        for (const key of ['Backspace', 'Delete']) {
            await test('selected language cleared with ' + key, async page => {
                await language(page, 0, 6); await page.keyboard.press(key);
                assert.equal((await state(page)).lang, ''); assert.equal((await state(page)).blocks, 1);
            });
        }
        await test('one backspace deletes one character', async page => {
            await language(page, 6); await page.keyboard.press('Backspace');
            assert.equal((await state(page)).lang, 'pytho');
        });
        await test('partial selection deleted', async page => {
            await language(page, 1, 4); await page.keyboard.press('Delete');
            assert.equal((await state(page)).lang, 'pon');
        });
        await test('forward delete respects caret', async page => {
            await language(page, 2); await page.keyboard.press('Delete');
            assert.equal((await state(page)).lang, 'pyhon');
        });
        await test('typing replaces selection', async page => {
            await language(page, 0, 6); await page.keyboard.press('j');
            assert.equal((await state(page)).lang, 'j');
        });
        await test('empty language never deletes frame', async page => {
            await language(page, 0, 6); await page.keyboard.press('Backspace');
            await page.keyboard.press('Backspace'); assert.equal((await state(page)).blocks, 1);
        });
        for (const key of ['Backspace', 'Delete']) {
            await test('remove empty frame with ' + key + ' preserves preceding paragraph and caret', async page => {
                await codeCaret(page);
                await page.evaluate(() => window.previousParagraph = codeTests.root.querySelectorAll('p')[1]);
                await page.keyboard.press(key);
                const result = await state(page);
                assert.equal(result.blocks, 0); assert.equal(result.beforeCaret, '和搞');
                assert.equal(await page.evaluate(() => previousParagraph.isConnected), true);
                assert.ok(!result.md.includes('```')); assert.ok(result.md.includes('和搞'));
            });
        }
        await test('frame deletion undo and redo', async page => {
            await codeCaret(page); await page.keyboard.press('Backspace');
            await page.keyboard.press('Control+z'); await page.waitForTimeout(180);
            assert.equal((await state(page)).blocks, 1);
            await page.keyboard.press('Control+y'); await page.waitForTimeout(180);
            assert.equal((await state(page)).blocks, 0);
        });
        await test('language deletion undo', async page => {
            await language(page, 0, 6); await page.keyboard.press('Backspace');
            await page.keyboard.press('Control+z'); await page.waitForTimeout(180);
            assert.equal((await state(page)).lang, 'python');
        });
        await test('nonempty code selection clears body before removing frame', async page => {
            await codeCaret(page);
            await page.evaluate(() => {
                const source = codeTests.root.querySelector('pre.vditor-ir__marker--pre > code');
                const range = document.createRange(); range.selectNodeContents(source);
                getSelection().removeAllRanges(); getSelection().addRange(range);
            });
            await page.keyboard.press('Backspace'); await page.waitForTimeout(180);
            assert.equal((await state(page)).blocks, 1);
            assert.ok(!(await state(page)).md.includes('print'));
            await page.keyboard.press('Backspace'); assert.equal((await state(page)).blocks, 0);
        }, '之前\n\n```python\nprint(1)\n```\n\n之后');
        await test('delayed code callback respects moved caret', async page => {
            await codeCaret(page);
            await page.evaluate(() => {
                const source = codeTests.root.querySelector('pre.vditor-ir__marker--pre > code');
                const range = document.createRange(); range.selectNodeContents(source);
                getSelection().removeAllRanges(); getSelection().addRange(range);
            });
            await page.keyboard.press('Backspace');
            await page.evaluate(() => codeTests.placeCaretInBlock(codeTests.root.querySelector('p'), true));
            await page.waitForTimeout(210); assert.equal((await state(page)).beforeCaret, '之前');
        }, '之前\n\n```python\nprint(1)\n```\n\n之后');
        await test('first frame falls back to next paragraph start', async page => {
            await codeCaret(page); await page.keyboard.press('Backspace');
            assert.equal((await state(page)).beforeCaret, '');
        }, '```python\n\n```\n\n之后');
        await test('only frame remains editable after removal', async page => {
            await codeCaret(page); await page.keyboard.press('Backspace'); await page.keyboard.type('hello');
            assert.ok((await state(page)).md.includes('hello'));
        }, '```python\n\n```');
        await test('surrogate pair backspace', async page => {
            await language(page, 3, 3, 'a😀b'); await page.keyboard.press('Backspace');
            assert.equal((await state(page)).lang, 'ab');
        });
        await test('surrogate pair forward delete', async page => {
            await language(page, 1, 1, 'a😀b'); await page.keyboard.press('Delete');
            assert.equal((await state(page)).lang, 'ab');
        });
        await test('missing selection does not edit language or frame', async page => {
            await language(page, 0, 6);
            const result = await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                getSelection().removeAllRanges();
                return [codeTests.editLangInfo(block, 'Backspace'), codeTests.removeEmptyCodeBlock(block)];
            });
            assert.deepEqual(result, [false, false]); assert.equal((await state(page)).lang, 'python');
        });
        await test('detached block operations are rejected', async page => {
            const result = await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]').cloneNode(true);
                return [codeTests.focusLangInfo(block), codeTests.editLangInfo(block, 'x'),
                    codeTests.removeEmptyCodeBlock(block), codeTests.placeCaretInBlock(block, true)];
            });
            assert.deepEqual(result, [null, false, false, false]);
        });
        await test('cross paragraph selection is not intercepted', async page => {
            await codeCaret(page);
            const result = await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                const range = getSelection().getRangeAt(0);
                range.setEndAfter(block.nextElementSibling);
                return [codeTests.editLangInfo(block, 'Backspace'), codeTests.removeEmptyCodeBlock(block)];
            });
            assert.deepEqual(result, [false, false]); assert.equal((await state(page)).blocks, 1);
        });
        await test('delayed language callback preserves cross paragraph selection', async page => {
            await language(page, 0, 6);
            await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                const range = getSelection().getRangeAt(0); range.setEndAfter(block.nextElementSibling);
            });
            await page.waitForTimeout(150);
            // Vditor hides fence markers on selectionchange; their text can disappear
            // from the range without the application collapsing the user's selection.
            assert.equal(await page.evaluate(() => getSelection().isCollapsed), false);
            assert.ok(await page.evaluate(() => getSelection().toString().includes('之后')));
            assert.equal(await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                return block.contains(getSelection().getRangeAt(0).endContainer);
            }), false);
            assert.equal(await page.evaluate(() => codeTests.pickerOpen), false);
        });
        await test('composition prevents custom deletion', async page => {
            await codeCaret(page);
            const result = await page.evaluate(() => {
                const root = codeTests.root, block = root.querySelector('[data-type="code-block"]');
                root.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true }));
                const rejected = codeTests.removeEmptyCodeBlock(block);
                const key = new KeyboardEvent('keydown', { key: 'Backspace', isComposing: true,
                    bubbles: true, cancelable: true });
                root.dispatchEvent(key);
                root.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true }));
                return rejected;
            });
            assert.equal(result, false); assert.equal((await state(page)).blocks, 1);
        });
        await test('legacy IME keycode 229 is not intercepted', async page => {
            await language(page, 0, 6);
            const intercepted = await page.evaluate(() => {
                const key = new KeyboardEvent('keydown', { key: 'Backspace', keyCode: 229,
                    bubbles: true, cancelable: true });
                codeTests.root.dispatchEvent(key); return key.defaultPrevented;
            });
            assert.equal(intercepted, false); assert.equal((await state(page)).lang, 'python');
        });
        await test('undo failure blocks language mutation and reports error', async page => {
            await language(page, 0, 6);
            await page.evaluate(() => { codeTests.editor.vditor.undo.addToUndoStack = () => { throw new Error('injected undo failure'); }; });
            await page.keyboard.press('Backspace');
            assert.equal((await state(page)).lang, 'python');
            assert.ok(await page.evaluate(() => hostMessages.some(msg => msg.t === 'jserror' && msg.src === 'recordCodeUndo')));
        });
        await test('undo failure blocks frame deletion', async page => {
            await codeCaret(page);
            await page.evaluate(() => { codeTests.editor.vditor.undo.addToUndoStack = () => { throw new Error('injected undo failure'); }; });
            await page.keyboard.press('Backspace'); assert.equal((await state(page)).blocks, 1);
        });
        await test('missing undo service leaves frame intact', async page => {
            await codeCaret(page);
            await page.evaluate(() => { codeTests.editor.vditor.undo = null; });
            await page.keyboard.press('Backspace'); assert.equal((await state(page)).blocks, 1);
        });
        await test('document replacement resets old picker and delayed focus', async page => {
            await language(page, 0, 6);
            await page.evaluate(() => window.msbridge.setContent('新文档'));
            await page.waitForTimeout(200);
            assert.equal(await page.evaluate(() => codeTests.pickerOpen), false);
            assert.ok((await state(page)).md.includes('新文档')); assert.equal((await state(page)).blocks, 0);
        });
        await test('mode rebuild clears old picker', async page => {
            await language(page, 0, 6);
            await page.evaluate(() => codeTests.createEditor('sv', '源码文档'));
            await page.waitForFunction(() => !!document.querySelector('.vditor-sv'));
            await page.waitForTimeout(200);
            assert.equal(await page.evaluate(() => codeTests.pickerOpen), false);
            assert.ok((await state(page)).md.includes('源码文档'));
        });
        await test('external input is unaffected by language picker', async page => {
            await language(page, 0, 6);
            await page.evaluate(() => {
                const input = document.createElement('input'); input.id = 'outside'; input.value = 'abc';
                document.body.appendChild(input); input.focus(); input.setSelectionRange(0, 3);
            });
            await page.keyboard.press('Backspace'); await page.waitForTimeout(100);
            assert.equal(await page.locator('#outside').inputValue(), '');
            assert.equal((await state(page)).lang, 'python');
            assert.equal(await page.evaluate(() => document.activeElement.id), 'outside');
        });
        await test('noneditable neighboring block creates editable fallback', async page => {
            await codeCaret(page);
            await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                block.previousElementSibling.setAttribute('contenteditable', 'false');
            });
            await page.keyboard.press('Backspace'); await page.keyboard.type('hello');
            assert.ok((await state(page)).md.includes('hello')); assert.equal((await state(page)).blocks, 0);
        });
        await test('protected empty paragraph content survives cleanup', async page => {
            await codeCaret(page);
            await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                const paragraph = document.createElement('p'), input = document.createElement('input');
                input.id = 'protected'; paragraph.appendChild(input); block.before(paragraph);
            });
            await page.keyboard.press('Backspace'); assert.equal(await page.locator('#protected').count(), 1);
        });
        await test('noneditable empty paragraph survives cleanup', async page => {
            await codeCaret(page);
            await page.evaluate(() => {
                const block = codeTests.root.querySelector('[data-type="code-block"]');
                const paragraph = document.createElement('p'); paragraph.id = 'protected';
                paragraph.setAttribute('contenteditable', 'false'); block.before(paragraph);
            });
            await page.keyboard.press('Backspace'); assert.equal(await page.locator('#protected').count(), 1);
        });
        await test('deleting second frame preserves first fenced code', async page => {
            await codeCaret(page, 1); await page.keyboard.press('Backspace');
            const result = await state(page);
            assert.equal(result.blocks, 1); assert.ok(result.md.includes('const s = "```";'));
        }, '```js\nconst s = "```";\n```\n\n和搞\n\n```python\n\n```');
        await test('deleting last frame does not reset long document scroll', async page => {
            await codeCaret(page);
            await page.evaluate(() => window.scrollTo(0, document.body.scrollHeight));
            const before = await state(page); await page.keyboard.press('Backspace');
            const after = await state(page);
            assert.equal(after.beforeCaret, '和搞'); assert.ok(after.scroll > 1000);
            assert.ok(Math.abs(after.scroll - before.scroll) < 200);
        }, Array.from({ length: 70 }, (_, i) => '段落' + i).join('\n\n') + '\n\n和搞\n\n```python\n\n```');
        assert.deepEqual(errors, [], 'Unhandled page errors');
        console.log(`${passed} checks passed; no unhandled page errors.`);
    } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => server.close());
