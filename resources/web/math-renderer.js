/* Offline MathJax rendering. Source parsing belongs to math-mode.js. */
(() => {
    'use strict';
    let loading = null, queue = Promise.resolve(), epoch = 0, loadFailed = false;
    let lastYield = performance.now();
    const jobs = new WeakMap();
    const errorTitle = '公式源码（公式未完整、语法错误或渲染器不可用）';
    const packages = ['base', 'ams', 'newcommand', 'boldsymbol', 'braket', 'cancel',
        'color', 'bbox', 'enclose', 'physics', 'unicode', 'verb', 'textmacros'];
    const paintAttributes = new Set(['fill', 'stroke', 'color', 'stop-color',
        'flood-color', 'lighting-color']);
    const referenceAttributes = new Set(['filter', 'mask', 'clip-path',
        'marker-start', 'marker-mid', 'marker-end', 'cursor']);
    function localReference(value) {
        return /^url\(\s*(['"]?)(#[\w:.-]+)\1\s*\)$/i.test(value);
    }
    function safePaint(value) {
        // SVG paint attributes accept URLs as well as colors. MathJax's color
        // package passes these through, including CSS-escaped URL tokens.
        return value.length <= 256 && !/[\\;]/.test(value) &&
            (value.toLowerCase() === 'none' || localReference(value) ||
                (!/var\s*\(/i.test(value) && window.CSS?.supports('color', value)));
    }
    function safeFamily(value) {
        const family = value.split(';')[0].trim();
        return family && family.length <= 256 && !/[{}\\()<>&]/.test(family) ? family : '';
    }
    function load() {
        if (loading) return loading;
        loading = new Promise((resolve, reject) => {
            window.MathJax = {
                loader: { paths: { mathjax: 'vditor/dist/js/mathjax' } },
                startup: { typeset: false },
                options: { enableMenu: false, enableAssistiveMml: false },
                tex: { packages, inlineMath: [['\\(', '\\)']], displayMath: [['\\[', '\\]']],
                    processEnvironments: false, processEscapes: false, processRefs: false,
                    maxBuffer: 16384, maxMacros: 1000 },
                svg: { fontCache: 'local' }
            };
            const script = document.createElement('script');
            script.id = 'msMathJaxScript';
            script.src = 'vditor/dist/js/mathjax/tex-svg-full.js';
            let settled = false;
            const timer = setTimeout(() => finish(Error('MathJax load timed out')), 10000);
            function finish(error) {
                if (settled) return;
                settled = true; clearTimeout(timer);
                if (error) { loadFailed = true; reject(error); }
                else { loadFailed = false; resolve(window.MathJax); }
            }
            script.onerror = () => finish(Error('MathJax unavailable'));
            script.onload = () => {
                const promise = window.MathJax?.startup?.promise;
                if (!promise) { finish(Error('MathJax initialization failed')); return; }
                promise.then(() => {
                    if (!window.MathJax.startup.constructors?.tex || !window.MathJax._?.mathjax?.mathjax)
                        finish(Error('Unsupported MathJax runtime'));
                    else finish();
                }, finish);
            };
            document.head.append(script);
        });
        return loading;
    }
    function safeInputPackage(mj) {
        const input = mj._.input.tex;
        const name = 'mswrite-safe-tex';
        if (!input.MapHandler.MapHandler.getMap(name)) {
            // MathJax 3.1's unicode extension keeps a module-wide font cache.
            // Keep that state with one input parser, just like user macros.
            const fonts = new WeakMap();
            new input.SymbolMap.CommandMap(name, { unicode: 'Unicode', mmlToken: 'MathMLToken' }, {
                Unicode(parser, command) {
                    const brackets = parser.GetBrackets(command);
                    const dimensions = brackets && /^\s*(?:\d+(?:\.\d*)?|\.\d+)\s*,\s*(?:\d+(?:\.\d*)?|\.\d+)\s*$/.test(brackets);
                    let family = dimensions ? parser.GetBrackets(command) : brackets;
                    if (family) family = safeFamily(family);
                    const argument = parser.GetArgument(command).trim().replace(/^0x/, 'x');
                    if (!/^(?:x[0-9a-f]+|[0-9]+)$/i.test(argument)) throw Error('Invalid Unicode character');
                    const point = parseInt(argument[0] === 'x' ? argument.slice(1) : argument,
                        argument[0] === 'x' ? 16 : 10);
                    if (!Number.isSafeInteger(point) || point < 0 || point > 0x10ffff ||
                        (point >= 0xd800 && point <= 0xdfff)) throw Error('Invalid Unicode character');
                    let cache = fonts.get(parser.configuration);
                    if (!cache) { cache = new Map(); fonts.set(parser.configuration, cache); }
                    if (family) cache.set(point, family);
                    else family = cache.get(point);
                    const attributes = {}, font = parser.stack.env.font;
                    if (family) {
                        attributes.fontfamily = family;
                        if (font?.includes('bold')) attributes.fontweight = 'bold';
                        if (font && /italic|-mathit/.test(font)) attributes.fontstyle = 'italic';
                    } else if (font) attributes.mathvariant = font;
                    const node = parser.create('token', 'mtext', attributes, String.fromCodePoint(point));
                    input.NodeUtil.default.setProperty(node, 'unicode', true);
                    parser.Push(node);
                },
                MathMLToken(parser, command) {
                    const kind = parser.GetArgument(command);
                    let attributeSource = parser.GetBrackets(command, '').trimStart();
                    const text = parser.GetArgument(command);
                    // mglyph can fetch an image while MathJax measures its
                    // size, before SVG sanitization has a chance to run.
                    if (!['mi', 'mn', 'mo', 'mtext', 'ms'].includes(kind))
                        throw Error('Unsupported MathML token');
                    const node = parser.create('node', kind), attributes = {};
                    const extra = new Set(['fontfamily', 'fontsize', 'fontweight', 'fontstyle', 'color', 'background']);
                    while (attributeSource) {
                        const match = /^([a-z]+)\s*=\s*('[^']*'|"[^"]*"|[^ ,]*)\s*,?\s*/i.exec(attributeSource);
                        if (!match) throw Error('Invalid MathML attribute');
                        const attribute = match[1].toLowerCase();
                        let value = match[2].replace(/^(['"])(.*)\1$/, '$2');
                        if (['href', 'src', 'style', 'id', 'class'].includes(attribute) ||
                            (!node.attributes.hasDefault(attribute) && !extra.has(attribute)))
                            throw Error('Unsupported MathML attribute');
                        if ((attribute === 'color' || attribute === 'background' || attribute === 'mathcolor' ||
                            attribute === 'mathbackground') && !safePaint(value.trim()))
                            throw Error('Unsafe MathML color');
                        if (attribute === 'fontfamily') {
                            value = safeFamily(value);
                            if (!value) throw Error('Unsafe MathML font');
                        }
                        const lower = value.toLowerCase();
                        attributes[attribute] = lower === 'true' ? true : lower === 'false' ? false : value;
                        attributeSource = attributeSource.slice(match[0].length);
                    }
                    node.appendChild(parser.create('text', text));
                    input.NodeUtil.default.setProperties(node, attributes);
                    parser.Push(node);
                }
            });
            input.Configuration.Configuration.create(name, { handler: { macro: [name] } });
        }
        return name;
    }
    function cleanSVG(container) {
        const allowed = new Set(['mjx-container', 'svg', 'g', 'path', 'defs', 'use', 'rect', 'line', 'polygon',
            'polyline', 'circle', 'ellipse', 'text', 'title', 'desc', 'clipPath']);
        const svg = container.querySelector('svg');
        if (!svg) throw Error('MathJax produced no SVG');
        const box = (svg.getAttribute('viewBox') || '').trim().split(/[\s,]+/).map(Number);
        if (box.length !== 4 || !box.every(Number.isFinite) ||
            Math.abs(box[0]) > 1000000 || Math.abs(box[1]) > 1000000 ||
            box[2] < 0 || box[3] < 0 || box[2] > 512000 || box[3] > 256000 ||
            container.outerHTML.length > 2 * 1024 * 1024)
            throw Error('Formula output too large');
        for (const el of [container, ...container.querySelectorAll('*')]) {
            if (!allowed.has(el.localName)) { el.remove(); continue; }
            for (const attr of [...el.attributes]) {
                const name = attr.name.toLowerCase();
                if (name.startsWith('on') || name === 'src' || name === 'srcdoc' || name === 'xml:base' ||
                    ((name === 'href' || name === 'xlink:href') && !/^#[\w:.-]+$/.test(attr.value)))
                    el.removeAttribute(attr.name);
                else if (paintAttributes.has(name) && !safePaint(attr.value.trim()))
                    el.removeAttribute(attr.name);
                else if (referenceAttributes.has(name) && attr.value.trim().toLowerCase() !== 'none' &&
                    !localReference(attr.value.trim()))
                    el.removeAttribute(attr.name);
                else if (name === 'style') {
                    // Unicode font arguments can contain semicolons and CSS
                    // escapes. Parse declarations and keep only the two styles
                    // needed by SVG output; a URL regexp alone is insufficient.
                    const vertical = el.style.getPropertyValue('vertical-align').trim();
                    const family = el.style.getPropertyValue('font-family').trim();
                    el.removeAttribute('style');
                    if (/^-?(?:\d+(?:\.\d+)?|\.\d+)(?:ex|em|px|%)$/.test(vertical) &&
                        Math.abs(parseFloat(vertical)) <= 1000)
                        el.style.setProperty('vertical-align', vertical);
                    if (family && family.length <= 256 && !/[;{}\\()<>&]/.test(family))
                        el.style.setProperty('font-family', family);
                    if (!el.getAttribute('style')) el.removeAttribute('style');
                }
            }
        }
        return container;
    }
    function convert(source, display, bold, current) {
        if (typeof source !== 'string' || source.length > 16384)
            return Promise.reject(Error('Formula too long'));
        const work = queue.then(async () => {
            if (current && !current()) return null;
            const mj = await load();
            if (current && !current()) return null;
            // Promise chains alone run every queued formula before browser
            // input gets another turn. Allow mode changes and edits to cancel
            // pending work while a large document is being rendered.
            if (performance.now() - lastYield >= 16) {
                await new Promise(resolve => setTimeout(resolve, 0));
                lastYield = performance.now();
                if (current && !current()) return null;
            }
            // Fresh input state prevents macros in one formula leaking to another
            // document, chat message, undo state, or export.
            const tex = new mj.startup.constructors.tex({ ...mj.config.tex,
                packages: packages.map(name => name === 'unicode' ? safeInputPackage(mj) : name) });
            const doc = mj._.mathjax.mathjax.document(document, { InputJax: tex,
                OutputJax: mj.startup.output, enableMenu: false, enableAssistiveMml: false });
            const node = doc.convert(bold ? '\\boldsymbol{' + source + '}' : source, { display });
            if (node.querySelector('[data-mml-node="merror"]')) throw Error('Invalid or incomplete formula');
            const style = mj.svgStylesheet?.();
            if (style && !document.getElementById('MJX-SVG-styles')) document.head.append(style);
            return cleanSVG(node);
        });
        queue = work.catch(() => {});
        return work;
    }
    function candidates(root) {
        return [...(root.matches?.('.language-math') ? [root] : []), ...root.querySelectorAll('.language-math')]
            .filter(el => !el.matches('code') && !el.closest('.vditor-ir__marker--pre,.vditor-wysiwyg__pre'));
    }
    function render(root, detached = false) {
        const generation = epoch;
        const promises = candidates(root).map(el => {
            const source = el.getAttribute('data-math') ?? el.textContent.replace(/\u00a0/g, ' ');
            const display = el.dataset.msMathDisplay === 'true' ||
                !!el.closest('[data-ms-math-display="true"]') ||
                el.tagName === 'DIV' || el.parentElement?.tagName === 'PRE';
            const bold = !!el.closest('strong,b');
            let quoteDepth = 0;
            for (let parent = el.parentElement; parent; parent = parent.parentElement)
                if (parent.tagName === 'BLOCKQUOTE') quoteDepth++;
            // Markdown quote prefixes on continuation lines belong to the
            // surrounding block, while the editable/saved source retains them.
            const renderSource = display && quoteDepth ? source.split('\n').map((line, index) => {
                if (!index) return line;
                for (let depth = 0; depth < quoteDepth; depth++) line = line.replace(/^ {0,3}> ?/, '');
                return line;
            }).join('\n') : source;
            const previous = jobs.get(el);
            if (!previous?.cancelled && previous?.source === source && previous.bold === bold && previous.display === display &&
                previous.renderSource === renderSource && previous.generation === generation)
                return previous.promise;
            const job = { source, bold, display, renderSource, generation };
            jobs.set(el, job); el.dataset.msMathPending = '1';
            el.setAttribute('data-math', source);
            const current = () => jobs.get(el) === job &&
                (detached || (el.isConnected && generation === epoch));
            job.promise = convert(renderSource, display, bold, current).then(node => {
                if (!node) { job.cancelled = true; return; }
                if (jobs.get(el) !== job || (!detached && (!el.isConnected || generation !== epoch))) return;
                el.replaceChildren(node); el.dataset.msMathDone = '1';
                el.classList.remove('vditor-reset--error');
                if (el.title === errorTitle) el.removeAttribute('title');
            }, () => {
                if (jobs.get(el) !== job || (!detached && (!el.isConnected || generation !== epoch))) return;
                el.textContent = (display ? '\\[' : '\\(') + source + (display ? '\\]' : '\\)');
                el.classList.add('vditor-reset--error'); el.dataset.msMathDone = '1';
                el.title = errorTitle;
            }).finally(() => {
                if (jobs.get(el) === job) {
                    delete el.dataset.msMathPending;
                    if (job.cancelled) jobs.delete(el);
                }
            });
            return job.promise;
        });
        return Promise.all(promises);
    }
    window.msMathRenderer = {
        render, convert,
        invalidate() {
            epoch++;
            // A missing local asset can recover after a mode switch or reload.
            // Keep failures cached within one generation to avoid retry storms.
            if (loadFailed) {
                loading = null; loadFailed = false;
                document.getElementById('msMathJaxScript')?.remove();
            }
        },
        pending(root) { return !!root.matches?.('[data-ms-math-pending="1"]') ||
            !!root.querySelector('[data-ms-math-pending="1"]'); },
        exportStyle() { return document.getElementById('MJX-SVG-styles')?.outerHTML || ''; }
    };
    // Vditor's internal renderer receives an unhandled engine name in MathJax
    // mode. Observe each newly parsed preview rather than letting its default
    // MathJax settings or KaTeX render the wrong delimiters.
    let scheduled = false;
    new MutationObserver(() => {
        if (scheduled || window.msMathMode?.getMode() !== 'mathjax') return;
        scheduled = true;
        queueMicrotask(() => {
            scheduled = false;
            if (window.msMathMode?.getMode() === 'mathjax' && document.body) render(document.body);
        });
    }).observe(document.documentElement, { childList: true, subtree: true });
    if (window.Vditor) {
        const original = Vditor.mathRender;
        Vditor.mathRender = (root, options) => window.msMathMode?.getMode() === 'mathjax'
            ? render(root) : original(root, options);
    }
})();
