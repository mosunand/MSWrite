/* The page receives only message data. Markdown is sanitized before it enters
   the live DOM; KaTeX runs with trust disabled. No model HTML can call the host. */
(() => {
    'use strict';
    const root = document.querySelector('#messages');
    const latest = document.querySelector('#latest');
    const context = document.querySelector('#context');
    const records = new Map();
    const post = data => window.chrome?.webview?.postMessage(data);
    window.addEventListener('error', event => post({t:'chatError',message:event.message,sequence:window.chatViewSequence ?? -1}));
    const lute = Lute.New();
    if (lute.SetSanitize) lute.SetSanitize(true);
    if (lute.SetInlineMathAllowDigitAfterOpenMarker) lute.SetInlineMathAllowDigitAfterOpenMarker(true);
    let follow = true, busy = false, toastTimer, paintTicket = 0;
    const button = (label, action) => {
        const b = document.createElement('button'); b.textContent = label; b.dataset.action = action;
        return b;
    };
    const selectionActive = () => !getSelection().isCollapsed && root.contains(getSelection().anchorNode);
    function toast(text) {
        const el = document.querySelector('#toast'); el.textContent = text; el.hidden = false;
        clearTimeout(toastTimer); toastTimer = setTimeout(() => { el.hidden = true; }, 1400);
    }
    function copy(text) { post({t:'chatCopy', text}); toast('已复制'); }
    function allMarkdown() {
        return [...records.values()].filter(r => r.data.kind < 2)
            .map(r => `## ${r.data.kind === 0 ? '我' : 'AI'}\n${r.data.text}`).join('\n\n');
    }
    // Lute handles dollar delimiters. Normalize the other common AI delimiters,
    // retaining fenced code and inline code verbatim.
    function normalizeMath(md) {
        let fence = null;
        return md.split('\n').map(line => {
            const marker = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line);
            if (marker) {
                if (!fence) fence = marker[1];
                else if (marker[1][0] === fence[0] && marker[1].length >= fence.length && !marker[2].trim()) fence = null;
                return line;
            }
            if (fence || /^( {4}|\t)/.test(line)) return line;
            return line.split(/(`+[^`]*(?:`[^`]+)*?`+)/g).map((part, i) => i % 2 ? part :
                part.replace(/\\\[/g, '$$$$').replace(/\\\]/g, '$$$$')
                    .replace(/\\\(/g, '$$').replace(/\\\)/g, '$$')).join('');
        }).join('\n');
    }
    function sanitize(html) {
        const t = document.createElement('template'); t.innerHTML = html;
        const allowed = new Set('P DIV SPAN BR HR EM STRONG B I S DEL U A BLOCKQUOTE H1 H2 H3 H4 H5 H6 OL UL LI PRE CODE TABLE THEAD TBODY TFOOT TR TH TD SUP SUB IMG INPUT'.split(' '));
        for (const el of [...t.content.querySelectorAll('*')]) {
            if (!allowed.has(el.tagName)) { el.remove(); continue; }
            for (const attr of [...el.attributes]) {
                const name = attr.name;
                if (name === 'class') {
                    el.className = el.className.split(/\s+/).filter(c => /^(language-[\w+-]+|math-inline|math-block)$/.test(c)).join(' ');
                } else if (name === 'href' && el.tagName === 'A') {
                    if (!/^https?:\/\//i.test(attr.value)) el.removeAttribute(name);
                } else if (name === 'src' && el.tagName === 'IMG') {
                    if (!/^data:image\/(png|jpeg|gif|webp);base64,/i.test(attr.value)) el.removeAttribute(name);
                } else if (!['alt','title','colspan','rowspan','start','checked'].includes(name)) {
                    el.removeAttribute(name);
                }
            }
            if (el.tagName === 'INPUT') { el.type = 'checkbox'; el.disabled = true; }
        }
        return t.content;
    }
    function markdown(text, target) {
        target.replaceChildren(sanitize(lute.Md2HTML(normalizeMath(text))));
        target.querySelectorAll('.language-math').forEach(el => {
            const source = el.textContent;
            const display = el.parentElement?.tagName === 'PRE' || el.tagName === 'DIV';
            const wrapper = document.createElement(display ? 'div' : 'span');
            wrapper.className = display ? 'math-block' : 'math-inline';
            try {
                katex.render(source, wrapper, {displayMode:display, throwOnError:true, trust:false,
                    strict:'ignore', maxSize:20, maxExpand:1000});
            } catch {
                wrapper.classList.add('math-pending'); wrapper.textContent = source;
                wrapper.title = '公式源码（等待完整公式或修正语法）';
            }
            (display && el.parentElement.tagName === 'PRE' ? el.parentElement : el).replaceWith(wrapper);
        });
        target.querySelectorAll('pre > code').forEach(code => {
            const pre = code.parentElement;
            const language = [...code.classList].find(c => c.startsWith('language-'))?.slice(9) || '';
            const source = code.textContent;
            if (language && hljs.getLanguage(language)) {
                code.innerHTML = hljs.highlight(source, {language, ignoreIllegals:true}).value;
            }
            const block = document.createElement('div'); block.className = 'code-block';
            const header = document.createElement('div'); header.className = 'code-head';
            const label = document.createElement('span'); label.textContent = language || '代码';
            const b = button('复制代码', 'copyCode'); b._copyText = source;
            header.append(label, b); pre.replaceWith(block); block.append(header, pre);
        });
        target.querySelectorAll('table').forEach(table => {
            const wrap = document.createElement('div'); wrap.className = 'table-wrap';
            table.replaceWith(wrap); wrap.append(table);
        });
    }
    function render(record) {
        const {data:d, el} = record;
        // Leave selected content in place while more tokens arrive.
        if (selectionActive() && el.contains(getSelection().anchorNode)) { record.pending = true; return; }
        record.pending = false;
        el.className = ['user','assistant','thinking','notice'][d.kind];
        if (d.kind === 3) {
            if (['error','ok'].includes(d.role)) el.classList.add(d.role);
            el.textContent = d.text; return;
        }
        if (d.kind === 2) {
            let details = el.querySelector('details');
            if (!details) {
                details = document.createElement('details');
                details.append(document.createElement('summary'), document.createElement('div'));
                details.lastChild.className = 'content'; el.replaceChildren(details);
            }
            details.firstChild.textContent = d.meta || '思考过程';
            details.lastChild.textContent = d.fullText || d.text;
            return;
        }
        let content = el.querySelector('.content');
        if (!content) {
            if (d.kind === 1) { const line = document.createElement('div'); line.className='byline'; line.textContent='AI'; el.append(line); }
            content = document.createElement('div'); content.className='content';
            const actions = document.createElement('div'); actions.className='actions';
            el.append(content, actions);
        }
        if (record.renderedText !== d.text || record.renderedPreferLatex !== !!d.preferLatex) {
            if (d.kind === 0) content.textContent = d.text;
            else markdown(d.preferLatex ? window.msLatexPreference.normalize(d.text) : d.text, content);
            record.renderedText = d.text;
            record.renderedPreferLatex = !!d.preferLatex;
            record.renderedImages = '';  // textContent 会清空内容,图片行必须重建
        }
        // 用户消息的图片附件(截图):显示原图,不再只是"📎 截图"文字
        const imgKey = (d.kind === 0 && Array.isArray(d.images))
            ? d.images.map(im => (im.n || '') + ':' + (im.src || '').length).join('|') : '';
        if (imgKey && record.renderedImages !== imgKey) {
            record.renderedImages = imgKey;
            let box = content.querySelector('.attach-imgs');
            if (!box) { box = document.createElement('div'); box.className = 'attach-imgs'; content.append(box); }
            box.replaceChildren();
            for (const im of d.images) {
                const img = document.createElement('img');
                img.className = 'attach-img';
                img.src = im.src;
                img.alt = im.n || '图片';
                img.title = im.n || '';
                box.append(img);
            }
        }
        content.classList.toggle('streaming', d.kind === 1 && !d.finalized);
        const actions = el.querySelector('.actions'); actions.replaceChildren();
        if (d.kind === 0 || d.finalized) {
            actions.append(button('复制', 'copy'));
            // 用户消息多一个"复制进询问框":原文回填输入框,改完再发
            if (d.kind === 0) actions.append(button('复制进询问框', 'askEdit'));
            if (d.kind === 1) { const b=button('重新生成', 'regenerate'); b.disabled=busy; actions.append(b); }
        }
        if (d.meta) { const meta=document.createElement('span'); meta.className='meta'; meta.textContent=d.meta; actions.append(meta); }
    }
    function scrollToBottom(force = false) {
        if (force) follow = true;
        requestAnimationFrame(() => {
            if (follow && !selectionActive()) window.scrollTo(0, document.documentElement.scrollHeight);
            latest.hidden = follow;
        });
    }
    window.chatView = {
        scrollToBottom,
        setTheme(light) {
            try {
                if (!document.documentElement) return false;
                document.documentElement.dataset.theme = light ? 'light' : 'dark';
                const codeTheme = document.querySelector('#codeTheme');
                if (codeTheme)
                    codeTheme.href = `vditor/dist/js/highlight.js/styles/github${light?'':'-dark'}.min.css`;
                return true;
            } catch (_) {
                return false;
            }
        },
        update(payload) {
            window.chatViewSequence = payload.sequence;
            if (payload.reset) { records.clear(); root.replaceChildren(); follow = true; }
            busy = payload.busy;
            const theme = payload.light ? 'light' : 'dark';
            if (document.documentElement.dataset.theme !== theme) {
                document.documentElement.dataset.theme = theme;
                document.querySelector('#codeTheme').href = `vditor/dist/js/highlight.js/styles/github${payload.light?'':'-dark'}.min.css`;
            }
            for (const [id,r] of records) if (id >= payload.count) { r.el.remove(); records.delete(id); }
            for (const data of payload.rows) {
                let r=records.get(data.id);
                if (!r) {
                    const el=document.createElement('article'); el.dataset.id=data.id;
                    r={el, data}; records.set(data.id,r); root.append(el);
                } else if (r.data.kind !== data.kind) {
                    r.el.replaceChildren();r.renderedText=undefined;
                }
                r.data=data; render(r);
            }
            root.querySelectorAll('[data-action="regenerate"]').forEach(b => { b.disabled=busy; });
            scrollToBottom();
            const ticket = ++paintTicket;
            window.msWaitForRender(root).then(() => {
                if (ticket === paintTicket) post({t:'chatRendered', count:records.size, sequence:payload.sequence});
            }, () => {
                if (ticket === paintTicket) post({t:'chatError', message:'Conversation rendering failed', sequence:payload.sequence});
            });
        }
    };
    window.addEventListener('scroll', () => {
        follow = document.documentElement.scrollHeight - innerHeight - scrollY < 60;
        latest.hidden = follow;
    }, {passive:true});
    latest.addEventListener('click', () => scrollToBottom(true));
    document.addEventListener('selectionchange', () => {
        if (!selectionActive()) for (const r of records.values()) if (r.pending) render(r);
    });
    document.addEventListener('click', e => {
        const link=e.target.closest('a');
        if (link) { e.preventDefault(); if (link.href) post({t:'chatLink',url:link.href}); return; }
        const b=e.target.closest('button'); if (!b) { context.hidden=true; return; }
        const record=records.get(Number(b.closest('article')?.dataset.id));
        switch (b.dataset.action) {
            case 'copy': copy(record.data.text); break;
            case 'askEdit':
                // 去掉"📎 附件名"尾巴,只把问题正文回填输入框
                post({t:'chatAskEdit', text: record.data.text.replace(/\n+📎[^\n]*$/, '')});
                break;
            case 'copyCode': copy(b._copyText); break;
            case 'copyAll': copy(allMarkdown()); break;
            case 'copySelection': copy(b._copyText); break;
            case 'regenerate': post({t:'chatRegenerate',row:record.data.id}); break;
            case 'export': post({t:'chatExport'}); break;
        }
        context.hidden=true;
    });
    document.addEventListener('contextmenu', e => {
        e.preventDefault(); context.replaceChildren();
        const selected=getSelection().toString();
        const record=records.get(Number(e.target.closest('article')?.dataset.id));
        if (selected || record?.data.kind < 2) {
            const b=button(selected?'复制选中文字':'复制本条回复', 'copySelection');
            b._copyText=selected || record.data.text; context.append(b);
        }
        context.append(button('复制全部对话','copyAll'), button('导出 Markdown…','export'));
        context.hidden=false;
        context.style.left=Math.max(0,Math.min(e.clientX,innerWidth-context.offsetWidth-8))+'px';
        context.style.top=Math.max(0,Math.min(e.clientY,innerHeight-context.offsetHeight-8))+'px';
    });
    document.addEventListener('keydown', e => { if(e.key==='Escape') context.hidden=true; });
    post({t:'chatReady'});
})();
