(function () {
    'use strict';
    // Keep the selected delimiters outside Lute's dollar-math grammar. Tokens
    // last for one conversion only; document text is never stored as a token.
    let mode = window.msInitialState && window.msInitialState.mathMode === 'mathjax' ? 'mathjax' : 'markdown';
    let serial = 0;
    const ownedSelector = '[data-ms-math-mode="mathjax"].vditor-ir__node,.language-math[data-ms-math-mode="mathjax"],[data-ms-math-literal],.ms-math-literal';
    const skipSelector = 'pre,code,script,style,textarea,.vditor-ir__marker,.vditor-ir__preview,[data-type="code"],[data-type="link-ref-defs-block"]';

    function escaped(text, index) {
        let count = 0;
        while (index > 0 && text[--index] === '\\') count++;
        return count % 2 === 1;
    }
    function body(node) {
        if (!node || node.nodeType !== Node.ELEMENT_NODE) return '';
        const owner = node.matches('.vditor-ir__node[data-ms-math-mode="mathjax"]') ? node :
            node.closest('.vditor-ir__node[data-ms-math-mode="mathjax"]');
        if (owner) return (owner.querySelector('code[data-type="math-inline"]') || owner).textContent;
        const source = node.getAttribute('data-ms-math-source') ?? node.getAttribute('data-math');
        return source === null ? node.textContent : source;
    }
    function isLiteral(node) { return node.hasAttribute('data-ms-math-literal') || node.classList.contains('ms-math-literal'); }
    function markdownSource(node) {
        if (isLiteral(node)) return node.textContent;
        const display = node.getAttribute('data-ms-math-display') === 'true';
        if (node.matches('.vditor-ir__node[data-ms-math-mode="mathjax"]')) {
            const markers = Array.from(node.children).filter(child => child.matches('span.vditor-ir__marker'));
            const open = markers.length ? markers[0].textContent : '';
            const close = markers.length > 1 ? markers[markers.length - 1].textContent : '';
            return open + body(node) + close;
        }
        return (display ? '\\[' : '\\(') + body(node) + (display ? '\\]' : '\\)');
    }
    function context() {
        const entropy = new Uint32Array(2);
        if (window.crypto && window.crypto.getRandomValues) window.crypto.getRandomValues(entropy);
        else { entropy[0] = Math.random() * 0xffffffff; entropy[1] = Math.random() * 0xffffffff; }
        const id = (++serial).toString(36) + entropy[0].toString(36) + entropy[1].toString(36);
        const prefix = 'MSMATHMODE' + id + 'TOKEN';
        const items = new Map();
        const reusable = new Map();
        function put(item) {
            // Reference links compare their label and definition before DOM
            // restoration. Identical source fragments must therefore use an
            // identical placeholder within this conversion. Caret bookmarks
            // remain local to their own occurrence.
            const key = item.bookmark === undefined ? item.source : null;
            if (key !== null && reusable.has(key)) {
                const token = reusable.get(key);
                // A visible label and its definition may share source while
                // only the visible text renders. Restoration applies the
                // literal rule in source-marker contexts independently.
                if (item.type === 'math') items.set(token, item);
                return token;
            }
            const token = prefix + items.size.toString(36) + 'END';
            items.set(token, item);
            if (key !== null) reusable.set(key, token);
            return token;
        }
        return { items, put, mode, pattern: new RegExp(prefix + '[0-9a-z]+END', 'g') };
    }
    function scanInline(text, ctx) {
        let out = '', i = 0;
        const closingCache = new Map();
        function nextClosing(marker, start, checkEscape = false, exactBacktick = false) {
            let close = closingCache.get(marker);
            if (close === undefined || (close >= 0 && close < start)) {
                close = text.indexOf(marker, start);
                while (checkEscape && close >= 0 && escaped(text, close)) close = text.indexOf(marker, close + marker.length);
                while (exactBacktick && close >= 0 && (text[close - 1] === '`' || text[close + marker.length] === '`')) {
                    close = text.indexOf(marker, close + marker.length);
                }
                closingCache.set(marker, close);
            }
            return close;
        }
        while (i < text.length) {
            if (text[i] === '`' && !escaped(text, i)) {
                let end = i + 1;
                while (text[end] === '`') end++;
                const marker = text.slice(i, end);
                const close = nextClosing(marker, end, false, true);
                if (close >= 0) { out += text.slice(i, close + marker.length); i = close + marker.length; continue; }
            }
            if (text[i] === '\\' && (text[i + 1] === '(' || text[i + 1] === '[') && !escaped(text, i)) {
                const display = text[i + 1] === '[';
                const closing = display ? '\\]' : '\\)';
                const close = nextClosing(closing, i + 2, true);
                const lf = display ? -1 : nextClosing('\n', i + 2);
                const cr = display ? -1 : nextClosing('\r', i + 2);
                if (close >= 0 && (display || ((lf < 0 || close < lf) && (cr < 0 || close < cr)))) {
                    const source = text.slice(i, close + 2);
                    out += ctx.put({ type: ctx.mode === 'mathjax' ? 'math' : 'literal', source, body: source.slice(2, -2), display });
                    i = close + 2; continue;
                }
            }
            if (text[i] === '$') {
                const marker = text[i + 1] === '$' ? '$$' : '$';
                const close = nextClosing(marker, i + marker.length, true);
                if (ctx.mode === 'mathjax') {
                    const lf = marker.length === 2 ? -1 : nextClosing('\n', i + marker.length);
                    const cr = marker.length === 2 ? -1 : nextClosing('\r', i + marker.length);
                    if (!escaped(text, i) && close >= 0 && (lf < 0 || close < lf) && (cr < 0 || close < cr)) {
                        // Protect the disabled expression as a whole: TeX
                        // escapes and Markdown punctuation inside stay literal.
                        const source = text.slice(i, close + marker.length);
                        out += ctx.put({ type: 'literal', source }); i = close + marker.length; continue;
                    }
                    out += ctx.put({ type: 'literal', source: '$' }); i++; continue;
                }
                // Leave legacy formula bodies wholly to Lute, including any
                // backslash delimiters used as part of their TeX expression.
                if (!escaped(text, i) && close >= 0) {
                    out += text.slice(i, close + marker.length); i = close + marker.length; continue;
                }
            }
            // Incomplete and escaped MathJax delimiters remain editable source.
            if (text[i] === '\\') {
                let end = i;
                while (text[end] === '\\') end++;
                if (/[()[\]]/.test(text[end] || '')) {
                    const source = text.slice(i, end + 1);
                    out += ctx.put({ type: 'literal', source }); i = end + 1; continue;
                }
            }
            out += text[i++];
        }
        return out;
    }
    function protectMarkdown(text, ctx) {
        const lines = String(text).split(/(?<=\n)/);
        let out = '', prose = '', fence = null;
        const state = { display: false, code: 0 };
        function flush() { out += scanInline(prose, ctx); prose = ''; state.display = false; state.code = 0; }
        function updateState(line) {
            for (let i = 0; i < line.length; i++) {
                if (line[i] === '`') {
                    let end = i + 1; while (line[end] === '`') end++;
                    const count = end - i;
                    if (!state.code) state.code = count;
                    else if (state.code === count) state.code = 0;
                    i = end - 1; continue;
                }
                if (!state.code && line[i] === '\\' && !escaped(line, i)) {
                    if (line[i + 1] === '[') state.display = true;
                    else if (line[i + 1] === ']') state.display = false;
                    i++;
                }
            }
        }
        for (const line of lines) {
            const opening = /^(?:[ \t]*>[ \t]*)*(?: {0,3})(?:(?:[-+*]|\d{1,9}[.)])[ \t]+)?(`{3,}|~{3,})/.exec(line);
            if (fence) {
                out += line;
                const closing = /^(?:[ \t]*>[ \t]*)*(?: {0,3})(`{3,}|~{3,})[ \t]*\r?\n?$/.exec(line);
                if (closing && closing[1][0] === fence.char && closing[1].length >= fence.length) fence = null;
                continue;
            }
            if (opening) { flush(); fence = { char: opening[1][0], length: opening[1].length }; out += line; continue; }
            if (!state.display && /^(?:[ \t]*>[ \t]*)*(?: {4}|\t)/.test(line)) { flush(); out += line; continue; }
            prose += line; updateState(line);
        }
        flush(); return out;
    }
    function element(item, flavor) {
        if (item.type === 'literal') {
            const literal = document.createElement('span');
            literal.className = 'ms-math-literal';
            if (flavor !== 'html') literal.setAttribute('data-ms-math-literal', 'true');
            literal.textContent = item.source;
            if (flavor !== 'html' && item.bookmark !== undefined) {
                const wbr = document.createElement('wbr');
                const offset = Math.min(Math.max(0, item.bookmark), item.source.length);
                if (literal.firstChild) literal.firstChild.splitText(offset).before(wbr);
                else literal.append(wbr);
            }
            return literal;
        }
        const preview = document.createElement('span');
        preview.className = 'language-math';
        preview.setAttribute('data-ms-math-mode', 'mathjax');
        preview.setAttribute('data-ms-math-display', String(item.display));
        preview.setAttribute('data-ms-math-source', item.body);
        preview.textContent = item.body;
        if (flavor === 'html') return preview;
        const wrapper = document.createElement('span');
        wrapper.className = 'vditor-ir__node';
        wrapper.setAttribute('data-type', 'inline-node');
        wrapper.setAttribute('data-ms-math-mode', 'mathjax');
        wrapper.setAttribute('data-ms-math-display', String(item.display));
        const marker = document.createElement('span');
        marker.className = 'vditor-ir__marker';
        marker.textContent = item.display ? '\\[' : '\\(';
        const code = document.createElement('code');
        code.className = 'vditor-ir__marker vditor-ir__marker--pre';
        code.setAttribute('data-newline', '1');
        code.setAttribute('data-type', 'math-inline');
        code.textContent = item.body;
        const target = document.createElement('span');
        target.className = 'vditor-ir__preview'; target.setAttribute('data-render', '2');
        target.append(preview);
        const end = marker.cloneNode(true); end.textContent = item.display ? '\\]' : '\\)';
        wrapper.append(marker, code, target, end);
        if (item.bookmark !== undefined) {
            const wbr = document.createElement('wbr');
            const offset = Math.min(Math.max(0, item.bookmark), item.body.length);
            if (code.firstChild) code.firstChild.splitText(offset).before(wbr);
            else code.append(wbr);
        }
        return wrapper;
    }
    function restoreHTML(html, ctx, flavor) {
        if (!ctx.items.size) return html;
        const template = document.createElement('template'); template.innerHTML = html;
        // Lute also copies Markdown tokens into link destinations, image alt
        // text and raw HTML attributes. Restore them as strings, never nodes;
        // otherwise temporary conversion tokens can leak into saved documents.
        for (const node of template.content.querySelectorAll('*')) {
            for (const attribute of Array.from(node.attributes)) {
                ctx.pattern.lastIndex = 0;
                if (ctx.pattern.test(attribute.value))
                    node.setAttribute(attribute.name, restoreMarkdown(attribute.value, ctx));
            }
        }
        const walker = document.createTreeWalker(template.content, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_COMMENT);
        const nodes = []; while (walker.nextNode()) nodes.push(walker.currentNode);
        for (const node of nodes) {
            const text = node.data; ctx.pattern.lastIndex = 0;
            const matches = Array.from(text.matchAll(ctx.pattern));
            if (!matches.length) continue;
            if (node.nodeType === Node.COMMENT_NODE) {
                node.data = restoreMarkdown(text, ctx); continue;
            }
            // The native parser decides whether a token landed in a code
            // block, code span, image source marker or raw HTML. Such tokens
            // must remain literal even when a nested Markdown container was
            // not recognized by the line scanner.
            if (node.parentElement && node.parentElement.closest('pre,code,script,style,textarea,[data-type="code"]')) {
                node.data = restoreMarkdown(text, ctx); continue;
            }
            const marker = node.parentElement && node.parentElement.closest('.vditor-ir__marker,.vditor-ir__preview,[data-type="link-ref-defs-block"]');
            const fragment = document.createDocumentFragment(); let start = 0;
            for (const match of matches) {
                fragment.append(document.createTextNode(text.slice(start, match.index)));
                const item = ctx.items.get(match[0]);
                const restored = marker && item.type === 'math' ? { type: 'literal', source: item.source,
                    bookmark: item.bookmark === undefined ? undefined : item.bookmark + 2 } : item;
                fragment.append(element(restored, flavor));
                start = match.index + match[0].length;
            }
            fragment.append(document.createTextNode(text.slice(start))); node.replaceWith(fragment);
        }
        return template.innerHTML;
    }
    function restoreMarkdown(text, ctx) {
        return String(text).replace(ctx.pattern, token => ctx.items.get(token).source);
    }
    function protectDOM(html, ctx, scan) {
        const template = document.createElement('template'); template.innerHTML = html;
        for (const node of Array.from(template.content.querySelectorAll(ownedSelector))) {
            if (!node.isConnected && !template.content.contains(node)) continue;
            if (node.parentElement && node.parentElement.closest(ownedSelector)) continue;
            const literalSource = isLiteral(node) ? node.textContent : '';
            const dollarMarker = literalSource.startsWith('$$') ? '$$' : '$';
            const pairedDollar = ctx.mode === 'mathjax' && literalSource.startsWith('$') &&
                literalSource.length >= dollarMarker.length * 2 && literalSource.endsWith(dollarMarker);
            const pairedBackslash = ctx.mode !== 'mathjax' &&
                ((literalSource.startsWith('\\(') && literalSource.endsWith('\\)')) ||
                    (literalSource.startsWith('\\[') && literalSource.endsWith('\\]')));
            if (scan && isLiteral(node) && !pairedDollar && !pairedBackslash &&
                !(node.parentElement && node.parentElement.closest(skipSelector))) {
                // A previously incomplete delimiter can become a complete
                // formula on the next keystroke. Keep native caret bookmarks
                // and scan it together with its new neighbouring text.
                node.replaceWith(...Array.from(node.childNodes)); continue;
            }
            let source = markdownSource(node);
            const display = node.getAttribute('data-ms-math-display') === 'true';
            const markers = Array.from(node.children).filter(child => child.matches('span.vditor-ir__marker'));
            const opening = markers[0], closing = markers.length > 1 ? markers[markers.length - 1] : null;
            const open = display ? '\\[' : '\\(', close = display ? '\\]' : '\\)';
            const standalone = node.matches('.language-math[data-ms-math-mode="mathjax"]');
            const valid = standalone || (opening && closing && opening.textContent === open && closing.textContent.startsWith(close));
            const trailing = valid && closing ? closing.textContent.slice(close.length) : '';
            if (valid) source = open + body(node) + close;
            const item = isLiteral(node) || ctx.mode !== 'mathjax' || !valid ? { type: 'literal', source } :
                { type: 'math', source, body: body(node), display };
            const code = node.querySelector('code[data-type="math-inline"]') || node;
            const bookmark = node.querySelector('wbr');
            let outsideBookmark;
            if (bookmark) {
                let container = code, offset = 0;
                if (opening && opening.contains(bookmark)) container = opening;
                else if (closing && closing.contains(bookmark)) {
                    container = closing; offset = (opening ? opening.textContent.length : 0) + body(node).length;
                } else if (container !== node) offset = opening ? opening.textContent.length : 0;
                const range = document.createRange(); range.selectNodeContents(container); range.setEndBefore(bookmark);
                const position = offset + range.toString().length;
                if (valid && closing && closing.contains(bookmark) && position >= open.length + body(node).length + close.length) {
                    outsideBookmark = position - open.length - body(node).length - close.length;
                } else item.bookmark = item.type === 'math' ? Math.max(0, position - open.length) : position;
            }
            const fragment = document.createDocumentFragment(); fragment.append(document.createTextNode(ctx.put(item)));
            if (outsideBookmark !== undefined) {
                fragment.append(document.createTextNode(trailing.slice(0, outsideBookmark)), document.createElement('wbr'),
                    document.createTextNode(trailing.slice(outsideBookmark)));
            } else if (trailing) fragment.append(document.createTextNode(trailing));
            node.replaceWith(fragment);
        }
        if (scan) {
            template.content.normalize();
            const walker = document.createTreeWalker(template.content, NodeFilter.SHOW_TEXT);
            const nodes = []; while (walker.nextNode()) nodes.push(walker.currentNode);
            for (const node of nodes) {
                if (node.parentElement && node.parentElement.closest(skipSelector)) continue;
                node.data = scanInline(node.data, ctx);
            }
        }
        return template.innerHTML;
    }

    const createLute = Lute.New;
    Lute.New = function () {
        const lute = createLute.apply(this, arguments);
        const originalInline = lute.SetInlineMath;
        let requestedInline = true;
        function configure() { if (originalInline) originalInline.call(lute, mode === 'mathjax' ? false : requestedInline); }
        if (originalInline) lute.SetInlineMath = function (value) { requestedInline = value !== false; configure(); };
        configure();
        for (const name of ['Md2VditorIRDOM', 'Md2HTML']) {
            const original = lute[name]; if (!original) continue;
            lute[name] = function (markdown) {
                configure();
                const ctx = context(), args = Array.from(arguments);
                args[0] = protectMarkdown(markdown, ctx);
                return restoreHTML(original.apply(lute, args), ctx, name === 'Md2HTML' ? 'html' : 'ir');
            };
        }
        for (const name of ['SpinVditorIRDOM', 'HTML2VditorIRDOM']) {
            const original = lute[name]; if (!original) continue;
            lute[name] = function (html) {
                configure();
                const ctx = context(), args = Array.from(arguments);
                args[0] = protectDOM(html, ctx, true);
                return restoreHTML(original.apply(lute, args), ctx, 'ir');
            };
        }
        for (const name of ['VditorIRDOM2Md', 'VditorIRDOM2HTML', 'HTML2Md']) {
            const original = lute[name]; if (!original) continue;
            lute[name] = function (html) {
                configure();
                if (name === 'HTML2Md') {
                    const pasted = document.createElement('template'); pasted.innerHTML = html;
                    if (pasted.content.querySelector('.vditor-ir__node')) return lute.VditorIRDOM2Md(html);
                }
                const ctx = context(), args = Array.from(arguments);
                args[0] = protectDOM(html, ctx, true);
                const result = original.apply(lute, args);
                return name === 'VditorIRDOM2Md' || name === 'HTML2Md' ? restoreMarkdown(result, ctx) : restoreHTML(result, ctx, 'html');
            };
        }
        const toSource = lute.Md2VditorSVDOM;
        if (toSource) lute.Md2VditorSVDOM = function () { configure(); return toSource.apply(lute, arguments); };
        return lute;
    };
    window.msMathMode = {
        setMode(value) { mode = value === 'mathjax' ? 'mathjax' : 'markdown'; return mode; },
        getMode() { return mode; },
        rawSource: body,
        markdownSource,
        // Useful to sanitize copied IR fragments without changing their source.
        isMathJax(node) { return Boolean(node && node.closest && node.closest('[data-ms-math-mode="mathjax"]')); }
    };
})();
