const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const http = require('node:http');
const { chromium } = require('playwright');

(async () => {
    const webRoot = path.resolve(__dirname, '../resources/web');
    const server = http.createServer(async (req, res) => {
        try {
            const file = path.resolve(webRoot, '.' + new URL(req.url, 'http://localhost').pathname);
            if (!file.startsWith(webRoot + path.sep)) throw Error('path');
            res.setHeader('Content-Type', ({'.html':'text/html', '.js':'text/javascript', '.css':'text/css',
                '.woff2':'font/woff2'})[path.extname(file)] || 'application/octet-stream');
            res.end(await fs.readFile(file));
        } catch { res.writeHead(404).end(); }
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const browser = await chromium.launch({executablePath:process.env.MSWRITE_BROWSER ||
        'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', headless:true});
    let passed = 0;
    try {
        const page = await browser.newPage({viewport:{width:830,height:900}});
        const errors = [];
        page.on('pageerror', e => errors.push(e.message));
        await page.addInitScript(() => { window.chrome.webview = {postMessage:m => (window.__messages ||= []).push(m)}; });
        await page.goto(`http://127.0.0.1:${server.address().port}/chat.html`);
        await page.waitForFunction(() => window.__messages?.some(m => m.t === 'chatReady'));
        const row = (text, finalized=true, id=0, kind=1) => ({id,kind,text,finalized,meta:'',fullText:'',role:''});
        const update = async (rows, reset=true, light=true, busy=false) => {
            await page.evaluate(p => chatView.update(p), {rows,reset,count:Math.max(...rows.map(r=>r.id))+1,light,busy});
            await page.waitForTimeout(100);
        };
        const check = async (name, fn) => { await fn(); assert.deepEqual(errors, []); ++passed; console.log('PASS',name); };
        await check('Markdown headings, lists, quotations and tables', async () => {
            await update([row('# 标题\n\n段落 **加粗** 和 *强调*。\n\n> 引用\n\n- 第一点\n- 第二点\n\n| 列一 | 列二 |\n| --- | --- |\n| 内容 | 结果 |')]);
            for (const s of ['h1','strong','em','blockquote','ul','table']) assert.equal(await page.locator(s).count(),1);
        });
        const formulas = [String.raw`$x^2$`, String.raw`$$\frac{x^2}{y_1}$$`,
            '$$\n'+String.raw`\begin{pmatrix}1 & 2 \\ 3 & 4\end{pmatrix}`+'\n$$',
            '$$\n'+String.raw`f(x)=\begin{cases}x^2 & x<0 \\ \sqrt{x} & x\ge0\end{cases}`+'\n$$',
            String.raw`\(a^2+b^2=c^2\)`, String.raw`\[`+'\n'+String.raw`\sum_{i=1}^{n}i=\frac{n(n+1)}{2}`+'\n'+String.raw`\]`];
        for (const text of formulas) await check('KaTeX ' + text.replaceAll('\n',' '), async () => {
            await update([row(text)]);
            assert.equal(await page.locator('.katex').count(),1,await page.locator('#messages').innerHTML());
            assert.equal(await page.locator('.math-pending,.katex-error').count(),0);
        });
        await check('Code remains literal and supports highlighting and copy', async () => {
            const code='const amount = "$5";\nconsole.log(amount);\n';
            await update([row('```javascript\n'+code+'```\n\nInline `$x$` and `\\(y\\)`')]);
            assert.equal(await page.locator('.katex').count(),0);
            assert.equal(await page.locator('pre code').textContent(),code);
            assert.ok(await page.locator('pre .hljs-keyword').count()>0);
            await page.getByRole('button',{name:'复制代码',exact:true}).click();
            assert.equal(await page.evaluate(() => __messages.at(-1).text),code);
        });
        await check('Untrusted HTML cannot execute or spoof host actions', async () => {
            await update([row('<img src=x onerror="window.hacked=1"><script>window.hacked=2</script>\n\n<button data-action="export">bad</button>\n\n[bad](javascript:alert(1))')]);
            assert.equal(await page.evaluate(() => window.hacked),undefined);
            assert.equal(await page.locator('#messages [onerror],#messages script,#messages [href^="javascript:"],#messages [data-action="export"]').count(),0);
        });
        await check('LaTeX preference renders escaped inline and both types of formula code', async () => {
            const text=String.raw`设函数 \$f(x)\$ 在点 $a$。`+'\n\n`$$f(x)=x^2$$`\n\n```latex\n$$x^3$$\n```';
            await update([{...row(text),preferLatex:true}]);
            assert.equal(await page.locator('.katex').count(),4);
            assert.equal(await page.locator('.code-block').count(),0);
            await update([{...row('`$$y^2$$`',true,1),preferLatex:false}],false);
            assert.equal(await page.locator('.katex').count(),4,'Later disabled preference must preserve earlier rendering');
            assert.equal(await page.locator('article[data-id="1"] .katex').count(),0);
        });
        await check('Reused message IDs replace old notice and thinking DOM', async () => {
            await update([row('Same text',true,0,3)]);
            await update([row('Same text',true,0,1)],false);
            assert.equal((await page.locator('article .content').textContent()).trim(),'Same text');
            await update([row('Thinking',true,0,2)],false);
            await update([row('User message',true,0,0)],false);
            assert.equal(await page.locator('article .content').textContent(),'User message');
            assert.equal(await page.locator('details').count(),0);
        });
        await check('Streaming content preserves mouse text selection', async () => {
            await update([row('Selectable streaming paragraph.',false)]);
            await page.locator('.content p').click({clickCount:3});
            const selected=await page.evaluate(() => getSelection().toString());
            assert.ok(selected.includes('Selectable'));
            await update([row('Selectable streaming paragraph. More tokens.',false)],false);
            assert.equal(await page.evaluate(() => getSelection().toString()),selected);
            await page.evaluate(() => getSelection().removeAllRanges());
            await page.waitForTimeout(100);
            assert.ok((await page.locator('.content').textContent()).includes('More tokens'));
        });
        await check('Streaming freezes scroll when reading older messages', async () => {
            await update(Array.from({length:18},(_,id)=>row(`## Answer ${id}\n\n`+'A readable paragraph. '.repeat(50),true,id)));
            await page.mouse.wheel(0,-900);
            await page.waitForTimeout(300);
            const before=await page.evaluate(() => scrollY);
            await update([row('New answer\n\n'+'More text. '.repeat(300),false,18)],false);
            assert.ok(Math.abs(await page.evaluate(()=>scrollY)-before)<3);
            assert.ok(await page.locator('#latest').isVisible());
            await page.locator('#latest').click();
            await page.waitForTimeout(150);
            assert.ok(await page.evaluate(()=>document.documentElement.scrollHeight-innerHeight-scrollY<3));
        });
        await check('Completed messages retain their DOM during later streaming', async () => {
            await update([row('First stable paragraph.'),row('Second',false,1)]);
            await page.evaluate(() => { window.__first=document.querySelector('article .content p'); });
            await update([row('Second with more tokens.',false,1)],false);
            assert.ok(await page.evaluate(()=>__first===document.querySelector('article .content p')));
        });
        await check('Following the latest response stays at the bottom', async () => {
            await update([row('Start\n\n'+'Paragraph. '.repeat(250),false)]);
            for (let i=0;i<4;i++) {
                await update([row('Start\n\n'+'Paragraph. '.repeat(300+i*50),false)],false);
                assert.ok(await page.evaluate(()=>document.documentElement.scrollHeight-innerHeight-scrollY<3));
            }
        });
        await check('Copy and regenerate actions use the correct message', async () => {
            await update([row('**Markdown source**')]);
            await page.getByRole('button',{name:'复制',exact:true}).click();
            assert.equal(await page.evaluate(()=>__messages.at(-1).text),'**Markdown source**');
            await page.getByRole('button',{name:'重新生成',exact:true}).click();
            assert.equal(await page.evaluate(()=>__messages.at(-1).row),0);
            await update([row('**Markdown source**')],false,true,true);
            assert.ok(await page.getByRole('button',{name:'重新生成',exact:true}).isDisabled());
        });
        await check('Long code and formulas scroll inside a narrow conversation', async () => {
            await page.setViewportSize({width:460,height:700});
            await update([row('```text\n'+'0123456789'.repeat(60)+'\n```\n\n$$'+Array.from({length:45},(_,i)=>`x_{${i}}`).join('+')+'$$')]);
            assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
            assert.ok(await page.locator('pre').evaluate(e=>e.scrollWidth>e.clientWidth));
            assert.ok(await page.locator('.math-block').evaluate(e=>e.scrollWidth>e.clientWidth));
        });
        await check('Light and dark text contrast stays readable', async () => {
            for (const light of [true,false]) {
                await update([row('## 阅读体验\n\n中文段落和 **强调**。\n\n> 引用内容')],true,light);
                const values=await page.evaluate(()=>({color:getComputedStyle(document.body).color,bg:getComputedStyle(document.documentElement).backgroundColor}));
                assert.notEqual(values.color,values.bg);
                assert.equal(await page.locator('html').getAttribute('data-theme'),light?'light':'dark');
            }
        });
        await page.setViewportSize({width:830,height:1000});
        await update([row('请解释下面的公式，并给一段示例代码。',true,0,0),row('## 从公式到实现\n\n现在可以直接选中回复中的文字，段落、列表和表格也采用统一的排版。\n\n$$\n\\sum_{i=1}^{n}i=\\frac{n(n+1)}{2}\n$$\n\n### 示例代码\n\n```python\ndef total(n):\n    return n * (n + 1) // 2\n```\n\n| 项目 | 说明 |\n| --- | --- |\n| 公式 | KaTeX 排版 |\n| 代码 | 高亮与单独复制 |\n\n> 流式输出时，上翻阅读不会被拉回底部。',true,1)]);
        await page.evaluate(()=>scrollTo(0,0));
        const artifactDir=path.resolve(__dirname,'../build');
        await fs.mkdir(artifactDir,{recursive:true});
        await page.screenshot({path:path.join(artifactDir,'chat-review.png'),fullPage:true});
        console.log(`${passed} browser chat checks passed.`);
    } finally { await browser.close(); server.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
