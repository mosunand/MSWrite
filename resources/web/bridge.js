/* Mswrite 编辑页桥接:C++(WebView2) <-> JS(Vditor)
 * 上行: window.chrome.webview.postMessage({t:...})
 * 下行: window.msbridge.<fn>(...) 由 C++ ExecuteScript 调用
 */
(function () {
    'use strict';
    var MSW_BRIDGE_VERSION = 169;
    window.msbridgeVer = MSW_BRIDGE_VERSION;
    window.mswValue = function () { return vd ? vd.getValue() : ''; }; // 启动校验(C++ 读日志) // 缓存排查:每次改动必须递增

    // 开发模式(?dev=1,由 C++ 侧 MSWRITE_DEV 注入):开启按键记录等排障代码
    var DEV = /(^|[?&])dev=1(&|$)/.test(location.search);

    var vd = null;                 // Vditor 实例
    var currentMode = 'ir';
    var imgSeq = 0;
    var pendingImages = Object.create(null);
    var rev = 0;                   // 文档修订号:每次内容变化自增,保存快照靠它判新旧
    var suppressUntil = 0;         // 程序设值后此时间窗内不视为脏
    var statsTimer = null;
    var selectionStatsTimer = null;
    var initial = window.msInitialState || {};
    rev = Number.isInteger(initial.rev) && initial.rev >= 0 ? initial.rev : 0;
    var lastValue = typeof initial.content === 'string' ? initial.content : '';
    var themePending = initial.theme || 'light';
    var fontSize = initial.fontSize || 16;
    var lineHeight = typeof initial.lineHeight === 'number' && initial.lineHeight >= 1.0
        ? Math.min(initial.lineHeight, 2.2) : 1.0;   // 正文行距(代码块/公式固定,不随动)
    var showLineNumbers = !!initial.lineNumbers;
    var preferLatex = !!initial.preferLatex;
    var latexBefore = null, latexTimer = null, applyingLatex = false;
    var docLang = '';              // 本文档默认代码语言:新建块自动带上(仍可手动改)
    var focusMode = false;
    var typewriter = false;
    var activeBlock = null;
    var docHost = '';              // 文档目录虚拟主机,如 doc1.local
    var imgObserverStarted = false;

    // 初始化门:Vditor 就绪前缓冲宿主指令,避免启动时序丢内容
    var vdReady = false;
    var editorGeneration = 0;
    var initQueue = [];
    function whenReady(fn) {
        if (vdReady) { try { fn(); } catch (e) { post({ t: 'jserror', msg: String(e && e.stack || e), src: 'whenReady' }); } }
        else initQueue.push(fn);
    }

    function post(obj) {
        try { window.chrome.webview.postMessage(obj); } catch (e) { /* 宿主未就绪 */ }
    }

    window.onerror = function (msg, src, line) {
        post({ t: 'jserror', msg: String(msg), src: String(src), line: line | 0 });
    };

    // 按键记录器(排障,仅开发模式):带修饰键/F 键是否到达页面,写 mswrite.log
    // 注意:它每次按键都会 getValue(),而 getValue 会规范化 DOM(空段落会被
    // 修剪)—— 生产环境必须关闭,否则等于把"空行被吃"的雷埋回来
    if (DEV) {
        var lastKeyLog = '';
        document.addEventListener('keydown', function (e) {
            if (!e.ctrlKey && !e.altKey && !e.metaKey && !/^F\d/.test(e.key)) return;
            if (e.code === 'ControlLeft' || e.code === 'ControlRight'
                || e.code === 'ShiftLeft' || e.code === 'ShiftRight'
                || e.code === 'AltLeft' || e.code === 'AltRight') return; // 裸修饰键不记
            var line = e.code + (e.ctrlKey ? '+C' : '') + (e.altKey ? '+A' : '') + (e.shiftKey ? '+S' : '');
            if (line === lastKeyLog) return; // 去重连按
            lastKeyLog = line;
            var ae = document.activeElement;
            post({ t: 'keylog', k: line,
                   focus: ae ? (ae.tagName + '.' + String(ae.className).slice(0, 20)) : 'none' });
            // 150ms 后报告效果:值是否变化(验证处理器真正生效)
            var before = window.mswValue();
            setTimeout(function () {
                var after = window.mswValue();
                if (after !== before)
                    post({ t: 'keylog', k: line + ' => ' + after });
            }, 150);
        }, true);
    }

    // ------------------------------------------------------------------
    // 统计(字数按 CJK 字 + 西文词计)
    // ------------------------------------------------------------------
    function countWords(text) {
        var cjk = (text.match(/[\u4e00-\u9fff\u3400-\u4dbf]/g) || []).length;
        var stripped = text.replace(/[\u4e00-\u9fff\u3400-\u4dbf]/g, ' ');
        var latin = (stripped.match(/[A-Za-z0-9]+(?:['\u2019-][A-Za-z0-9]+)*/g) || []).length;
        return cjk + latin;
    }

    function editorRoot() {
        // IR 的可编辑区是 .vditor-ir > pre.vditor-reset(contenteditable),
        // 内容块是 PRE 的直接子级;wysiwyg 同理为 .vditor-wysiwyg 元素
        if (vd && vd.vditor && vd.vditor[currentMode])
            return vd.vditor[currentMode].element;
        return document.querySelector('.vditor-ir pre.vditor-reset')
            || document.querySelector('.vditor-wysiwyg pre.vditor-reset')
            || document.querySelector('.vditor-wysiwyg')
            || document.querySelector('.vditor-sv textarea');
    }

    // ------------------------------------------------------------------
    // 文档开头正文(未命名文档的默认文件名候选):首个非空行【原文】,
    // 跳过代码围栏与块级公式整体。剥 Markdown 标记由 C++ 侧统一做。
    // ------------------------------------------------------------------
    function firstLineRaw() {
        if (!lastValue) return '';
        var lines = lastValue.slice(0, 65536).split('\n');
        var fenceCh = null, fenceLen = 0, inMath = false;
        for (var i = 0; i < lines.length; i++) {
            var t = lines[i].trim();
            if (fenceCh) {
                var n = 0;
                while (n < t.length && t.charAt(n) === fenceCh) n++;
                if (n >= fenceLen && !t.slice(n).trim()) fenceCh = null;
                continue;
            }
            if (inMath) { if (t === '$$') inMath = false; continue; }
            var fm = /^(`{3,}|~{3,})/.exec(t);
            if (fm) { fenceCh = t.charAt(0); fenceLen = fm[1].length; continue; }
            if (t === '$$') { inMath = true; continue; }
            if (t) return t.slice(0, 200);
        }
        return '';
    }
    var lastFirstLine = null;
    function pushFirstLine() {
        // Native code owns Markdown stripping. Send a bounded prefix so blank
        // markers, comments and front matter cannot hide the real first title.
        var t = lastValue.slice(0, 65536);
        if (t === lastFirstLine) return;   // 没变就不发
        lastFirstLine = t;
        post({ t: 'firstLine', text: t });
    }
    // 供 C++ 在"另存为/导出"对话框弹出前同步兜底取一次首行:
    // firstLine 消息有 300ms 防抖,打完字立刻保存时还没发出
    window.mswFirstLine = firstLineRaw;
    window.mswTitleSource = function () {
        return (vd ? vd.getValue() : lastValue).slice(0, 65536);
    };

    // Empty math loaded by Lute contains wbr caret bookmarks. Vditor restores
    // the FIRST wbr on Enter/undo, even if it belongs to an unrelated formula.
    // Clean at edit boundaries, after synchronous caret restoration finishes.
    function clearStaleCaretBookmarks() {
        var root = editorRoot();
        if (!root || currentMode === 'sv') return;
        root.querySelectorAll('wbr').forEach(function (node) { node.remove(); });
    }

    function prepareEditorInput(event) {
        var root = editorRoot();
        if (!root || !root.contains(event.target)) return;
        // A real edit ends the load-only suppression window, including IME
        // and paste. Otherwise typing just after setContent can remain clean.
        if (event.type === 'beforeinput' || (event.type === 'keydown'
            && !event.ctrlKey && !event.metaKey && !event.altKey
            && (event.key.length === 1 || /^(Enter|Backspace|Delete)$/.test(event.key))))
            suppressUntil = 0;
        if (!vd || codeInputComposing || event.isComposing || event.keyCode === 229
            || event.ctrlKey || event.metaKey || event.altKey || event.defaultPrevented) return;
        if (event.type === 'keydown' && event.key.length !== 1 &&
            !/^(Enter|Backspace|Delete)$/.test(event.key)) return;
        clearStaleCaretBookmarks();
        var undo = vd.vditor && vd.vditor.undo;
        var history = undo && undo[currentMode];
        // Visual classes differ from the initial DOM, so Vditor's strict first
        // position comparison rejects headings decorated by the application.
        // Snapshot the real caret before the first edit, not the document start.
        if (history && history.undoStack && history.redoStack
            && history.undoStack.length === 1 && !history.redoStack.length && !recordCodeUndo()) {
            if (event.cancelable) event.preventDefault();
            event.stopImmediatePropagation();
        }
    }
    document.addEventListener('keydown', prepareEditorInput, true);
    document.addEventListener('beforeinput', prepareEditorInput, true);

    function preferredText(text) {
        return preferLatex && window.msLatexPreference ? window.msLatexPreference.normalize(text) : text;
    }
    document.addEventListener('beforeinput', function (event) {
        if (!vd || currentMode !== 'ir' || event.isComposing || !event.cancelable ||
            event.inputType !== 'insertText' || event.data !== '$') return;
        if (!editorRoot().contains(event.target) || /^(INPUT|TEXTAREA|SELECT)$/.test(event.target.tagName)) return;
        var block = caretBlock(), selection = getSelection();
        if (!block || block.tagName !== 'P' || block.querySelector('[data-type]') ||
            !/^\${1,2}$/.test(block.textContent) || !selection.rangeCount || !selection.isCollapsed) return;
        var range = selection.getRangeAt(0);
        if (!block.contains(range.startContainer)) return;
        var tail = range.cloneRange(); tail.selectNodeContents(block);
        tail.setStart(range.startContainer, range.startOffset);
        if (tail.toString()) return;
        // Keep an empty inline pair editable; Lute treats bare $$ as a block.
        // The fourth dollar and subsequent formula content use the normal parser.
        event.preventDefault(); event.stopImmediatePropagation();
        clearStaleCaretBookmarks();
        recordMathUndo();
        var dollar = document.createTextNode('$');
        range.insertNode(dollar); range.setStartAfter(dollar); range.collapse(true);
        selection.removeAllRanges(); selection.addRange(range);
        suppressUntil = 0;
        recordMathUndo(); onInput(vd.getValue());
    }, true);
    function blockMarkdown(block) {
        return vd.vditor.lute.VditorIRDOM2Md(block.outerHTML);
    }
    function convertEditedLatexBlock(previous, target, mergeUndo) {
        if (!preferLatex || applyingLatex) return;
        if (currentMode === 'sv') { convertSourceLatex(previous); return; }
        if (currentMode !== 'ir') return;
        var block = target && target.isConnected ? target : caretBlock();
        if (!block || block.getAttribute('data-type') === 'math-block') return;
        var source = blockMarkdown(block);
        var converted = window.msLatexPreference.normalize(source, previous);
        if (converted === source) return;
        var selection = getSelection();
        if (!selection.rangeCount || !selection.isCollapsed) return;
        var range = selection.getRangeAt(0).cloneRange();
        var markerText = 'mswCaret' + Date.now().toString(36);
        var marker = document.createTextNode(' ' + markerText);
        applyingLatex = true;
        try {
            if (!mergeUndo) recordMathUndo();
            var isCode = block.getAttribute('data-type') === 'code-block';
            if (isCode) converted += '\n\n' + markerText;
            else {
                range.insertNode(marker);
                converted = window.msLatexPreference.normalize(blockMarkdown(block), previous);
                marker.remove();
            }
            var fragment = document.createElement('template');
            fragment.innerHTML = vd.vditor.lute.Md2VditorIRDOM(converted);
            var nodes = Array.prototype.slice.call(fragment.content.childNodes);
            block.replaceWith(fragment.content);
            var found = false;
            nodes.forEach(function (node) {
                var walker = document.createTreeWalker(node, NodeFilter.SHOW_TEXT);
                var text;
                while ((text = walker.nextNode())) {
                    var offset = text.data.indexOf(markerText);
                    if (offset < 0) continue;
                    // Keep a caret anchor outside an inline formula. An empty
                    // text node is discarded by Vditor's undo normalization.
                    var markerLength = markerText.length;
                    if (!isCode && offset > 0 && text.data[offset - 1] === ' ') { offset--; markerLength++; }
                    text.replaceData(offset, markerLength, '\u200b');
                    range = document.createRange(); range.setStart(text, offset + 1); range.collapse(true);
                    selection.removeAllRanges(); selection.addRange(range); found = true; break;
                }
            });
            clearStaleCaretBookmarks();
            nodes.forEach(function (node) {
                if (node.nodeType === 1) Vditor.mathRender(node, { cdn: vd.vditor.options.cdn,
                    math: vd.vditor.options.preview.math });
            });
            decorateMathBlocks(); recordMathUndo(); onInput(vd.getValue());
            if (found) editorRoot().focus({ preventScroll: true });
        } finally { marker.remove(); applyingLatex = false; }
        return true;
    }
    function convertSourceLatex(previous) {
        var source = vd.getValue();
        var converted = window.msLatexPreference.normalize(source, previous);
        if (converted === source) return;
        var selection = getSelection();
        if (!selection.rangeCount || !selection.isCollapsed) return;
        var markerText = 'mswCaret' + Date.now().toString(36);
        var marker = document.createTextNode(markerText);
        var range = selection.getRangeAt(0).cloneRange();
        applyingLatex = true;
        try {
            recordMathUndo(); range.insertNode(marker);
            var caret = vd.getValue().indexOf(markerText);
            marker.remove();
            if (caret < 0) return;
            var shift = 0;
            window.msLatexPreference.changes(source, previous).forEach(function (change) {
                if (change.end <= caret) shift += change.text.length - (change.end - change.start);
            });
            caret += shift;
            converted = converted.slice(0, caret) + markerText + converted.slice(caret);
            var root = editorRoot();
            root.innerHTML = vd.vditor.lute.Md2VditorSVDOM(converted);
            var walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT), text;
            while ((text = walker.nextNode())) {
                var offset = text.data.indexOf(markerText);
                if (offset < 0) continue;
                text.deleteData(offset, markerText.length);
                range = document.createRange();range.setStart(text, offset);range.collapse(true);
                selection.removeAllRanges();selection.addRange(range);break;
            }
            recordMathUndo();onInput(vd.getValue());
        } finally { marker.remove(); applyingLatex = false; }
    }
    document.addEventListener('beforeinput', function (event) {
        if (!preferLatex || event.isComposing || applyingLatex || !/^(ir|sv)$/.test(currentMode) ||
            /^(history|delete)/.test(event.inputType || '')) { latexBefore = null; return; }
        if (event.data && !/[$`~\n]/.test(event.data)) { latexBefore = null; return; }
        if (currentMode === 'sv') { latexBefore = vd.getValue(); return; }
        var block = caretBlock();
        latexBefore = block ? blockMarkdown(block) : null;
    }, true);
    document.addEventListener('input', function (event) {
        if (!preferLatex || event.isComposing || applyingLatex || latexBefore === null) return;
        var before = latexBefore; latexBefore = null;
        clearTimeout(latexTimer);
        latexTimer = setTimeout(function () { convertEditedLatexBlock(before); }, 0);
    }, true);

    // 光标所在块(编辑器直接子级)
    function caretBlock() {
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount) return null;
        var root = editorRoot();
        if (!root || !root.children) return null;
        var r = sel.getRangeAt(0);
        var node = r.startContainer;
        // 根锚点:空段落回车后常见。childNodes[offset] 经常是夹在块之间的
        // 文本节点,旧实现这时返回 null,Ctrl+Shift+K 就会退到文末。
        if (node === root) {
            var kids = root.children;
            if (!kids.length) return null;
            if (r.startOffset >= node.childNodes.length)
                return kids[kids.length - 1];
            var at = node.childNodes[r.startOffset];
            if (at && at.nodeType === 1 && at.parentElement === root)
                return at;
            var prev = at ? at.previousSibling : node.lastChild;
            while (prev) {
                if (prev.nodeType === 1 && prev.parentElement === root)
                    return prev;
                prev = prev.previousSibling;
            }
            var nxt = at;
            while (nxt) {
                if (nxt.nodeType === 1 && nxt.parentElement === root)
                    return nxt;
                nxt = nxt.nextSibling;
            }
            return kids[0];
        }
        var block = node.nodeType === 3 ? node.parentElement : node;
        while (block && block.parentElement !== root) block = block.parentElement;
        return (block && block.parentElement === root) ? block : null;
    }

    function placeCaretInBlock(block, atEnd) {
        var root = editorRoot(), sel = window.getSelection();
        if (!root || !block || block.nodeType !== 1 || !block.isConnected || !root.contains(block) || !sel
            || block.closest('[contenteditable="false"]') || /^(HR|IMG|INPUT|IFRAME)$/.test(block.tagName))
            return false;
        try {
            var walker = document.createTreeWalker(block, NodeFilter.SHOW_TEXT, {
                acceptNode: function (n) {
                    var p = n.parentElement;
                    while (p && p !== block) {
                        if (p.classList && p.classList.contains('vditor-ir__preview'))
                            return NodeFilter.FILTER_REJECT;
                        if (p.getAttribute && p.getAttribute('data-ms-ui'))
                            return NodeFilter.FILTER_REJECT;
                        if (p.getAttribute && p.getAttribute('contenteditable') === 'false')
                            return NodeFilter.FILTER_REJECT;
                        p = p.parentElement;
                    }
                    return NodeFilter.FILTER_ACCEPT;
                }
            });
            var first = null, last = null, n;
            while ((n = walker.nextNode())) {
                if (!first) first = n;
                last = n;
            }
            var rng = document.createRange();
            var node = atEnd ? last : first;
            if (node) {
                rng.setStart(node, atEnd ? node.textContent.length : 0);
            } else {
                rng.selectNodeContents(block);
                rng.collapse(!atEnd);
            }
            rng.collapse(true);
            sel.removeAllRanges();
            sel.addRange(rng);
            return true;
        } catch (e) { return false; }
    }

    // 代码块上下的视觉间距来自块 margin,浏览器不会在 margin 区域放光标。
    // 用户点“代码块上一行”时把落点归到相邻文本块,避免出现一条死区。
    document.addEventListener('mousedown', function (e) {
        if (e.button !== 0 || currentMode !== 'ir') return;
        var root = editorRoot();
        if (!root || e.target !== root || !root.children || !root.children.length) return;
        var y = e.clientY;
        var kids = root.children;
        var handled = false;
        for (var i = 0; i < kids.length; i++) {
            var cur = kids[i];
            var prev = i > 0 ? kids[i - 1] : null;
            var curRect = cur.getBoundingClientRect();
            var prevBottom = prev ? prev.getBoundingClientRect().bottom : root.getBoundingClientRect().top;
            if (y <= prevBottom || y >= curRect.top) continue;
            var curCode = cur.getAttribute && cur.getAttribute('data-type') === 'code-block';
            var prevCode = prev && prev.getAttribute && prev.getAttribute('data-type') === 'code-block';
            if (!curCode && !prevCode) continue;
            var target = curCode && prev ? prev : cur;
            var atEnd = target === prev;
            if (placeCaretInBlock(target, atEnd)) {
                e.preventDefault();
                e.stopPropagation();
                if (vd) vd.focus();
                document.dispatchEvent(new Event('selectionchange'));
            }
            return;
        }
        // 最后一个块【下方】的空白:末尾代码块是困局 —— Enter 只在块内加行,
        // 点下方又落不进任何元素(用户只能点上方逃逸)。这里在末尾代码块后
        // 补一个空段并落光标,给一条确定性的出路
        var last = kids[kids.length - 1];
        if (last && last.getAttribute && last.getAttribute('data-type') === 'code-block'
            && y >= last.getBoundingClientRect().bottom) {
            e.preventDefault();
            e.stopPropagation();
            var pad = document.createElement('p');
            pad.setAttribute('data-block', '0');
            last.after(pad);
            try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e9) {}
            var sel2 = window.getSelection();
            var rg2 = document.createRange();
            rg2.selectNodeContents(pad);
            rg2.collapse(false);
            sel2.removeAllRanges();
            sel2.addRange(rg2);
            if (vd) vd.focus();
            try { onInput(vd.getValue()); } catch (eA) {}
            document.dispatchEvent(new Event('selectionchange'));
            handled = true;
        }
        if (handled) return;
    }, true);

    // 点击分隔线(hr):光标会落进不可编辑的 <hr> 本体,"横线上有个光标"
    // 却什么都输不了。不拦点击(v96 教训),只在事后把落在 hr 上的光标
    // 规整到相邻的普通段落:优先下一行,没有就上一行
    document.addEventListener('mousedown', function (e) {
        if (e.button !== 0 || currentMode !== 'ir') return;
        var root = editorRoot();
        if (!root || !root.contains(e.target)) return;
        setTimeout(function () {
            try {
                var sel = window.getSelection();
                if (!sel || !sel.rangeCount) return;
                var anc = sel.anchorNode;
                if (!anc) return;
                var node = anc.nodeType === 1 ? anc : anc.parentElement;
                if (!node) return;
                var hr = node.closest ? node.closest('hr[data-block], .vditor-reset > hr') : null;
                if (!hr || !root.contains(hr)) return;
                var root2 = editorRoot();
                var next = hr.nextElementSibling;
                var prev = hr.previousElementSibling;
                var isPara = function (el) { return el && el.tagName === 'P'; };
                var target = null, atEnd = false;
                if (isPara(next)) { target = next; atEnd = false; }
                else if (isPara(prev)) { target = prev; atEnd = true; }
                if (!target) return;
                var rg = document.createRange();
                rg.selectNodeContents(target);
                rg.collapse(!atEnd);
                sel.removeAllRanges();
                sel.addRange(rg);
                if (vd) vd.focus();
                document.dispatchEvent(new Event('selectionchange'));
            } catch (e2) {}
        }, 0);
    }, true);

    // 选区纯文本:必须剥掉 .vditor-ir__preview —— 行内公式的 KaTeX 渲染
    // 结果是真实 DOM 文本,直接 r.toString() 会把渲染副本也算进来,
    // 全选 $x_1$ 上色会得到 "$x_1x1$"(重复的 x1 就是渲染副本)
    function selectionTextClean(r) {
        try {
            var holder = document.createElement('div');
            holder.appendChild(r.cloneContents());
            var pvs = holder.querySelectorAll('.vditor-ir__preview');
            for (var i = 0; i < pvs.length; i++) pvs[i].remove();
            return (holder.textContent || '').replace(/\u200b/g, '');
        } catch (e) {
            return r.toString().replace(/\u200b/g, '');
        }
    }

    function codeBlockOf(node) {
        var el = !node ? null : (node.nodeType === 1 ? node : node.parentElement);
        while (el && el !== document.body) {
            if (el.getAttribute && el.getAttribute('data-type') === 'code-block')
                return el;
            el = el.parentElement;
        }
        return null;
    }

    function currentCodeBlock() {
        var sel = window.getSelection();
        var fromCaret = codeBlockOf(caretBlock());
        if (fromCaret) return fromCaret;
        if (sel && sel.rangeCount)
            return codeBlockOf(sel.getRangeAt(0).startContainer);
        return null;
    }

    function caretPos() {
        try {
            var sel = window.getSelection();
            if (!sel || !sel.rangeCount) return { l: 1, c: 1 };
            var editor = editorRoot();
            if (!editor) return { l: 1, c: 1 };
            var r = sel.getRangeAt(0);
            var block = caretBlock();
            var line = Math.max(1, Array.prototype.indexOf.call(editor.children, block) + 1);
            var pre = document.createRange();
            pre.selectNodeContents(block || editor);
            pre.setEnd(r.startContainer, r.startOffset);
            return { l: line, c: pre.toString().length + 1 };
        } catch (e) {
            return { l: 1, c: 1 };
        }
    }

    var lastStatsValue = null, lastWords = 0, lastChars = 0, lastLines = 1;
    function pushStats() {
        if (!vd) return;
        // 用 input 回调缓存的全文统计:绝不调 getValue()
        // (IR 模式下 getValue 会规范化 DOM,空段落会被周期性修剪掉 —— 曾经的"空行被吃"bug)
        var v = lastValue;
        var cp = caretPos();
        // 字数/字符/行数只在全文变化时重算(光标移动也触发本函数,
        // 每次都全文跑正则很浪费;字符串 === 对同一引用是常数时间)
        if (v !== lastStatsValue) {
            lastStatsValue = v;
            lastWords = countWords(v);
            lastChars = v.replace(/\s/g, '').length;
            lastLines = v ? v.split('\n').length : 1;
        }
        post({
            t: 'stats',
            words: lastWords,
            chars: lastChars,
            lines: lastLines,
            cl: cp.l, cc: cp.c
        });
    }

    // ------------------------------------------------------------------
    // 大纲(IR/wysiwyg 从渲染后的 DOM 取,与 scrollToHeading 同源)
    // ------------------------------------------------------------------
    function pushOutline() {
        if (!vd) return;
        var items = [];
        var root = editorRoot();
        var heads = (!root || !root.querySelectorAll || currentMode === 'sv')
                        ? null
                        : root.querySelectorAll('h1,h2,h3,h4,h5,h6');
        if (heads) {
            // 从 DOM 收集:下标与 scrollToHeading 的 querySelectorAll 完全一致。
            // 旧实现按 markdown 行正则扫描,引用块/列表/表格里的标题与 setext
            // 标题在两边数量不同,点大纲会跳到错误位置
            for (var i = 0; i < heads.length; i++) {
                var h = heads[i];
                var clone = h.cloneNode(true);
                var marks = clone.querySelectorAll('[class*="vditor-ir__marker"]');
                for (var j = 0; j < marks.length; j++)
                    marks[j].remove();          // 去掉 IR 的行首 # 标记
                items.push({ level: parseInt(h.tagName.slice(1), 10),
                             text: (clone.textContent || '').trim() });
            }
        } else {
            // 源码模式没有渲染 DOM,退回按行扫描(跳围栏代码内的伪标题)
            var lines = lastValue.split('\n');
            var fence = false;
            lines.forEach(function (line) {
                if (/^\s*(```|~~~)/.test(line)) { fence = !fence; return; }
                if (fence) return;
                var m = /^(#{1,6})\s+(.+?)\s*#*$/.exec(line);
                if (m) items.push({ level: m[1].length, text: m[2] });
            });
        }
        // 大纲没变就不重发:C++ 侧每次都会清空重建列表,长文档下很贵
        var sig = '';
        for (var k = 0; k < items.length; k++)
            sig += items[k].level + ':' + items[k].text + '|';
        if (sig === lastOutlineSig) return;
        lastOutlineSig = sig;
        post({ t: 'outline', items: items });
    }
    var lastOutlineSig = '';

    // --- 打字触发的 YAML front matter 回退 ------------------------------
    // 行首敲第三个 '-',Lute 可能把 '---' 变成 front matter 并自动补闭合标记,
    // 后续输入全进了元数据区(用户:"输入---再按=,出现的是什么鬼")。
    // 触发用 MutationObserver 直盯 DOM:输入法的合成事件绕得开 input 监听,
    // 绕不开 DOM 变化。
    // 规则:文档加载时不带 front matter → 出现的 front matter 一律视为误触发,
    // 连同里面已敲的字符一起还原成正文;粘贴/加载自带的原样保留。
    // 注意:第二行及以后打 --- 也会触发(Lute 对中部 --- 一样造 fm 块,
    // 实测 kids: ['P','yaml-front-matter']),必须扫全篇,不能只看首块。
    var fmHadBefore = false;      // 加载时文档自带 front matter(setContent/初始渲染置位)
    var fmExisted = false;        // 已认可的 front matter(粘贴产生后置位,不再回退)
    var fmPasteGuardUntil = 0;    // 粘贴/拖放后的短窗:期间出现的 fm 属用户粘贴,保留
    document.addEventListener('paste', function () { fmPasteGuardUntil = Date.now() + 800; }, true);
    document.addEventListener('drop', function () { fmPasteGuardUntil = Date.now() + 800; }, true);
    // 同拍兜底:非合成输入时第一时间处理(Vditor 的 input 回调要等 undoDelay)
    document.addEventListener('input', function () {
        if (currentMode !== 'ir') return;
        setTimeout(revertTypingFrontMatter, 0);
    }, true);
    var fmObserver = new MutationObserver(function () {
        if (currentMode !== 'ir') return;
        setTimeout(revertTypingFrontMatter, 0);
    });
    function observeFrontMatter() {
        var root = editorRoot();
        if (!root) { setTimeout(observeFrontMatter, 300); return; }
        fmObserver.disconnect();
        fmObserver.observe(root.parentElement || root, { childList: true, subtree: true });
    }
    function syncFrontMatterFlag() {
        var root = editorRoot();
        if (!root || !root.firstElementChild
            || root.firstElementChild.getAttribute('data-type') !== 'yaml-front-matter') {
            fmHadBefore = false;
            return;
        }
        // 加载时文档就带 front matter:视为合法,输入路径不回退
        fmHadBefore = true;
        fmExisted = true;
    }
    function revertTypingFrontMatter() {
        var root = editorRoot();
        if (!root) return;
        // 扫描全部直接子块里的 front matter(首块/中部都要)
        var kids = root.children, i, first = null;
        for (i = 0; i < kids.length; i++) {
            if (kids[i].getAttribute && kids[i].getAttribute('data-type') === 'yaml-front-matter') {
                first = kids[i];
                break;
            }
        }
        if (!first) { fmExisted = false; return; }
        if (fmHadBefore || fmExisted) return;
        if (Date.now() < fmPasteGuardUntil) { fmExisted = true; return; }   // 粘贴产生:保留
        var code = first.querySelector('code[data-type="yaml-front-matter"]');
        var content = ((code ? code.textContent : '') || '').replace(/\u200b/g, '');
        content = content.replace(/\n+/g, '');   // 还原为正文段落,不保留换行
        try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e0) {}
        var p = document.createElement('p');
        p.setAttribute('data-block', '0');
        p.textContent = '---' + content;
        first.replaceWith(p);
        fmExisted = false;
        var sel = window.getSelection();
        var rg = document.createRange();
        rg.selectNodeContents(p);
        rg.collapse(false);
        sel.removeAllRanges();
        sel.addRange(rg);
        try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e1) {}
        if (vd) vd.focus();
        document.dispatchEvent(new Event('selectionchange'));
        try { onInput(vd.getValue()); } catch (e2) {}
        scheduleGutters();
    }

    function onInput(value) {
        if (typeof value !== 'string' && vd) value = vd.getValue();
        rev++;                      // 内容变了,修订号自增(保存快照靠它判新旧)
        if (typeof value === 'string')
            lastValue = value;      // 缓存全文,统计/大纲用它,不再调 getValue
        revertTypingFrontMatter();  // 打字误触发的空 front matter 还原成 '---' 段落
        // Vditor 的 IR 变换若在本回调之后才跑,这里补一拍延迟重试(幂等)
        setTimeout(revertTypingFrontMatter, 0);
        // 装饰(行号/高亮/统计)不受 suppress 影响:setValue 之后内容全新,
        // 旧代码在抑制窗口里直接 return,导致整篇代码块行号消失
        scheduleGutters();
        scheduleLangSuggest();
        clearTimeout(statsTimer);
        clearTimeout(selectionStatsTimer);
        statsTimer = setTimeout(function () {
            pushStats();
            pushOutline();
            pushFirstLine();   // 默认文件名候选(有变化才发)
            renderColorTags();
            decorateAllCodeBlocks();
            updateMathPop();   // 气泡内容跟随公式源码
        }, 300);
        // Vditor 的 KaTeX 预览有 ~400ms 延迟,补一拍刷新气泡内容
        if (lastMiNode) setTimeout(updateMathPop, 550);
        if (Date.now() < suppressUntil) return; // 程序设值引发的联动,不算脏
        // Cache the acknowledged content in the native process; renderer
        // recovery must not reload the document's original bootstrap text.
        post({ t: 'changed', rev: rev, md: lastValue });
    }

    // 文末"踩钢丝"根治:只在【回车】时判断 —— 新行本身就会引发一次自然
    // 滚动,微推(≤10px)叠在这一下里,观感上"不知不觉"。计算只有一次
    // rect 读取 + 两次数值比较,不碰布局写入。普通打字完全不触发。
    // 60ms/200ms 双发兜底 Vditor 的异步重渲染(Vditor 换行后可能换元素)
    var tailRaf = 0;
    function ensureTailBreathing() {
        if (tailRaf) return;
        tailRaf = requestAnimationFrame(function () {
            tailRaf = 0;
            try {
                var sel = window.getSelection();
                if (!sel || !sel.rangeCount) return;
                var r = sel.getRangeAt(0).getBoundingClientRect();
                if (!r || (!r.height && !r.width)) {
                    var b = caretBlock();
                    if (!b) return;
                    r = b.getBoundingClientRect();
                }
                var vh = window.innerHeight;
                var clearance = vh - r.bottom;
                if (clearance >= 8) return;      // 常量底隙已够,不动
                var delta = 10 - clearance;     // 补到 10px(≈半个字高)
                var maxY = document.documentElement.scrollHeight - vh;
                var y = window.scrollY || 0;
                if (y + delta > maxY) delta = Math.max(0, maxY - y);
                if (delta > 0) window.scrollBy(0, delta);
            } catch (e) {}
        });
    }
    function tailNudgeOnEnter() {
        setTimeout(ensureTailBreathing, 60);
        setTimeout(ensureTailBreathing, 200);
    }

    // ------------------------------------------------------------------
    // 专注模式(淡化非当前块)与打字机模式(当前块居中)
    // ------------------------------------------------------------------
    function onSelectionChange() {
        var block = caretBlock();
        if (focusMode) {
            if (activeBlock && activeBlock !== block)
                activeBlock.classList.remove('ms-active-block');
            activeBlock = block;
            if (activeBlock) activeBlock.classList.add('ms-active-block');
        }
        if (typewriter && block) {
            try {
                var br = block.getBoundingClientRect();
                var vh = window.innerHeight || 0;
                if (vh && (br.top < vh * 0.28 || br.bottom > vh * 0.72)) {
                    var targetTop = vh * 0.42;
                    window.scrollBy(0, Math.round(br.top - targetTop));
                }
            } catch (e) {}
        }
        scheduleCbEditSync();
        // Moving the caret must not cancel pending content/title/outline work.
        clearTimeout(selectionStatsTimer);
        selectionStatsTimer = setTimeout(function () { pushStats(); renderColorTags(); }, 350);
    }
    document.addEventListener('selectionchange', onSelectionChange);

    // ------------------------------------------------------------------
    // 代码块"代码语言"浮动按钮(Typora 式):光标进代码块时出现在块右下
    // 角,点击后光标定位到语言标记处,直接输入语言(python/go/…)即可切
    // 换该块的高亮着色
    // ------------------------------------------------------------------
    var codeLangBtn = null;
    var codeLangBlock = null;   // 当前按钮所属的代码块

    // 代码块编辑态同步:ms-cb-edit(光标在块内)+ ms-lang-editing(光标在
    // 语言标记里)。Vditor 重渲染会整块换元素把 class 冲掉,所以除了
    // selectionchange,headingObserver 的 DOM 变更回调也会调这里补挂
    // 选区变化/观察器回调都可能一帧内触发多次,合并到一帧跑一次
    var cbSyncRaf = 0;
    function scheduleCbEditSync() {
        if (cbSyncRaf) return;
        cbSyncRaf = requestAnimationFrame(function () {
            cbSyncRaf = 0;
            syncCbEditState();
        });
    }
    function syncCbEditState() {
        if (langPickerOpen) {
            var keep = (langPickerBlock && langPickerBlock.isConnected)
                ? langPickerBlock : currentCodeBlock();
            if (keep) {
                window._msCbEdit = keep;
                keep.classList.add('ms-cb-edit');
                keep.classList.add('ms-lang-editing');
                langPickerBlock = keep;
            }
            syncCodeLangBtn(keep);
            return;
        }
        var cb = currentCodeBlock();
        if (typeof window._msCbEdit === 'undefined') window._msCbEdit = null;
        if (window._msCbEdit !== cb) {
            if (window._msCbEdit && window._msCbEdit.classList) {
                window._msCbEdit.classList.remove('ms-cb-edit');
                window._msCbEdit.classList.remove('ms-lang-editing');
                window._msCbEdit.classList.remove('ms-cb-hi-ready');
                decorateReadingBlock(window._msCbEdit);
            }
            if (cb) cb.classList.add('ms-cb-edit');
            window._msCbEdit = cb;
            // 只在"进出代码块"时重排行号/高亮 —— 这才是布局变化的时刻。
            // 光标在块内/正文里移动不改变布局,逐次全量重排纯属浪费
            // (每个代码块两次 rect 读取,大文档上每次移动都抖)
            scheduleGutters();
        }
        if (cb) {
            var infoEl = langInfoEl(cb);
            var langEdit = false;
            if (infoEl) {
                var selL = window.getSelection();
                var sc = selL.rangeCount ? selL.getRangeAt(0).startContainer : null;
                langEdit = sc === infoEl || (infoEl.contains && infoEl.contains(sc));
            }
            cb.classList.toggle('ms-lang-editing', langEdit);
        }
        syncCodeLangBtn(cb);
        syncInlineMathActive();
        updateMathPop();
    }

    // 行内公式编辑预览气泡(图二形态):源码留在原地,渲染结果放深色圆角
    // 气泡里悬在源码【下方】,小箭头指向上方的公式。内容直接克隆行内预览
    // 已渲染的 KaTeX(Vditor 自己会随输入刷新,不重复调 katex)
    var mathPopEl = null;
    function ensureMathPop() {
        if (mathPopEl) return mathPopEl;
        mathPopEl = document.createElement('div');
        mathPopEl.id = 'ms-math-pop';
        mathPopEl.setAttribute('data-ms-ui', 'math');
        mathPopEl.hidden = true;
        document.body.appendChild(mathPopEl);
        return mathPopEl;
    }
    function updateMathPop() {
        var el = ensureMathPop();
        var node = lastMiNode;
        if (!node || !node.isConnected || !node.classList.contains('ms-mi-active')) {
            el.hidden = true;
            return;
        }
        var pv = node.querySelector('.vditor-ir__preview');
        var inner = (pv && pv.querySelector('.katex')) ? pv.innerHTML : '';
        if (!inner) { el.hidden = true; return; }
        if (el.__html !== inner) {
            el.__html = inner;
            el.innerHTML = inner;
        }
        var r = node.getBoundingClientRect();
        el.hidden = false;
        var w = el.offsetWidth, h = el.offsetHeight;
        var left = Math.max(8, Math.min(r.left + r.width / 2 - w / 2,
                                       window.innerWidth - w - 8));
        var top = r.bottom + 8;
        if (top + h > window.innerHeight - 8 && r.top - h - 8 > 0) {
            top = r.top - h - 8;
            el.classList.add('ms-pop-up');
        } else {
            el.classList.remove('ms-pop-up');
        }
        el.style.left = Math.round(left) + 'px';
        el.style.top = Math.round(top) + 'px';
        el.style.setProperty('--ms-pop-arrow-x',
            Math.round(Math.min(Math.max(r.left + r.width / 2 - left, 14), w - 14)) + 'px');
    }

    // 行内公式 Typora 双态:光标落在公式里 → ms-mi-active(显源码藏渲染),
    // 离开 → 只显 KaTeX。挂在 syncCbEditState 尾部,选区变化自然驱动
    var lastMiNode = null;
    function syncInlineMathActive() {
        var node = null;
        try {
            var sel = window.getSelection();
            if (sel && sel.rangeCount) {
                var c = sel.getRangeAt(0).startContainer;
                var el = c.nodeType === 3 ? c.parentElement : c;
                node = el && el.closest
                    ? el.closest('.vditor-ir__node[data-type="inline-node"]')
                    : null;
                // 确认这个 inline-node 真是行内公式(不是链接/其他行内节点)
                if (node && !node.querySelector('[data-type="math-inline"]'))
                    node = null;
            }
        } catch (e) { node = null; }
        if (lastMiNode === node) return;
        if (lastMiNode && lastMiNode.classList)
            lastMiNode.classList.remove('ms-mi-active');
        if (node) {
            // 基类一起补:打字时 Vditor 重渲染换元素会冲掉 ms-mi,而它
            // 原本要等 300ms 防抖的 decorateMathBlocks 才回——那段时间
            // CSS 全失效,源码/预览/$ 三个并排(用户截图的状态)。
            // 这里帧级把两个类一起挂上,重渲染后一帧内恢复正确形态
            node.classList.add('ms-mi');
            node.classList.add('ms-mi-active');
        }
        lastMiNode = node;
    }

    // 点击行内公式:浏览器把光标放进 KaTeX 渲染结果里(不可编辑)。
    // 不拦点击(v96 教训:mousedown 不能改变点击语义),只在事后把
    // 落在本节点内的光标规整到源码末尾 —— 点公式即进编辑
    document.addEventListener('mousedown', function (e) {
        var el = e.target;
        var node = el && el.closest
            ? el.closest('.vditor-ir__node[data-type="inline-node"]') : null;
        if (!node || !node.classList.contains('ms-mi')) return;
        setTimeout(function () {
            try {
                var sel = window.getSelection();
                if (!sel || !sel.rangeCount) return;
                var c = sel.getRangeAt(0).startContainer;
                if (!node.contains(c)) return;   // 光标不在本节点:不干预
                var code = node.querySelector('[data-type="math-inline"]');
                if (!code) return;
                var walker = document.createTreeWalker(code, NodeFilter.SHOW_TEXT);
                var last = null, tn;
                while ((tn = walker.nextNode())) last = tn;
                var rng = document.createRange();
                if (last) {
                    rng.setStart(last, last.textContent.length);
                } else {
                    rng.selectNodeContents(code);
                }
                rng.collapse(true);
                sel.removeAllRanges();
                sel.addRange(rng);
                document.dispatchEvent(new Event('selectionchange'));
            } catch (e2) {}
        }, 0);
    }, true);

    // 语言别名 → hljs 规范名(用户写 py/c++ 时也能正确着色)
    var LANG_ALIAS = {
        'c++': 'cpp', 'c#': 'csharp', 'cs': 'csharp', 'js': 'javascript',
        'ts': 'typescript', 'py': 'python', 'sh': 'bash', 'shell': 'bash',
        'zsh': 'bash', 'yml': 'yaml', 'md': 'markdown', 'rs': 'rust',
        'kt': 'kotlin', 'objc': 'objectivec', 'obj-c': 'objectivec'
    };
    var LANG_CATALOG = [
        ['python', 'Python', '#3572A5', 'Py'],
        ['javascript', 'JavaScript', '#f1e05a', 'JS'],
        ['typescript', 'TypeScript', '#3178c6', 'TS'],
        ['java', 'Java', '#b07219', 'Jv'],
        ['cpp', 'C++', '#f34b7d', 'C+'],
        ['c', 'C', '#555555', 'C'],
        ['csharp', 'C#', '#178600', 'C#'],
        ['go', 'Go', '#00add8', 'Go'],
        ['rust', 'Rust', '#dea584', 'Rs'],
        ['php', 'PHP', '#4F5D95', 'Ph'],
        ['ruby', 'Ruby', '#701516', 'Rb'],
        ['swift', 'Swift', '#F05138', 'Sw'],
        ['kotlin', 'Kotlin', '#A97BFF', 'Kt'],
        ['scala', 'Scala', '#c22d40', 'Sc'],
        ['html', 'HTML', '#e34c26', 'H'],
        ['css', 'CSS', '#563d7c', 'Cs'],
        ['scss', 'SCSS', '#c6538c', 'Sc'],
        ['less', 'Less', '#1d365d', 'Le'],
        ['json', 'JSON', '#292929', '{ }'],
        ['yaml', 'YAML', '#cb171e', 'Y'],
        ['xml', 'XML', '#0060ac', 'X'],
        ['sql', 'SQL', '#e38c00', 'Sq'],
        ['bash', 'Bash', '#89e051', 'Sh'],
        ['powershell', 'PowerShell', '#012456', 'Ps'],
        ['markdown', 'Markdown', '#083fa1', 'Md'],
        ['lua', 'Lua', '#000080', 'Lu'],
        ['r', 'R', '#198ce7', 'R'],
        ['matlab', 'MATLAB', '#e16737', 'M'],
        ['perl', 'Perl', '#0298c3', 'Pl'],
        ['haskell', 'Haskell', '#5e5086', 'Hs'],
        ['dart', 'Dart', '#00B4AB', 'Da'],
        ['elixir', 'Elixir', '#6e4a7e', 'Ex'],
        ['erlang', 'Erlang', '#B83998', 'Er'],
        ['clojure', 'Clojure', '#db5855', 'Cl'],
        ['groovy', 'Groovy', '#4298b8', 'Gy'],
        ['objectivec', 'Objective-C', '#438eff', 'Oc'],
        ['pascal', 'Pascal', '#E3F171', 'Pa'],
        ['fortran', 'Fortran', '#4d41b1', 'F'],
        ['vbnet', 'VB.NET', '#945db7', 'VB'],
        ['ini', 'INI', '#d1dbe0', 'In'],
        ['toml', 'TOML', '#9c4221', 'Tm'],
        ['dockerfile', 'Dockerfile', '#384d54', 'Dk'],
        ['makefile', 'Makefile', '#427819', 'Mk'],
        ['diff', 'Diff', '#e6ffed', 'Df'],
        ['plaintext', 'Plain Text', '#888888', 'Aa']
    ];
    function resolveLang(raw) {
        var s = String(raw || '').replace(/[\u200b\s]/g, '').toLowerCase();
        if (!s) return '';
        if (LANG_ALIAS[s]) return LANG_ALIAS[s];
        if (window.hljs && window.hljs.getLanguage && window.hljs.getLanguage(s))
            return s;
        return s;
    }
    function langMeta(id) {
        for (var i = 0; i < LANG_CATALOG.length; i++) {
            if (LANG_CATALOG[i][0] === id) return LANG_CATALOG[i];
        }
        var label = id || 'plain';
        return [id, label, '#6a737d', (label.slice(0, 2) || '?').toUpperCase()];
    }
    function ensureHljs() {
        if (window.hljs) return;
        if (window._msHljsLoading) return;
        window._msHljsLoading = true;
        var sc = document.createElement('script');
        sc.src = 'vditor/dist/js/highlight.js/highlight.min.js';
        sc.onload = function () {
            window._msHljsLoading = false;
            scheduleGutters();
        };
        sc.onerror = function () { window._msHljsLoading = false; };
        document.head.appendChild(sc);
    }
    // ------------------------------------------------------------------
    // 行号 + 编辑态高亮:全部画在 .vditor-ir 内的一层覆盖层里。
    // 覆盖层在 contenteditable 之外 —— Lute 永远读不到它,不会污染
    // markdown;又是文档流内定位,滚动/缩放天然对齐,不会悬空漂移。
    // 行号/高亮的字体与行高逐块从代码元素的计算样式复制,保证像素对齐。
    // ------------------------------------------------------------------
    var RENDERED_LANGS = {
        mermaid: 1, flowchart: 1, graphviz: 1, echarts: 1, mindmap: 1,
        plantuml: 1, wavedrom: 1, abc: 1, 'sequence': 1
    };
    function isRenderedBlock(block, lang) {
        if (RENDERED_LANGS[lang]) return true;
        var pv = block.querySelector('pre.vditor-ir__preview');
        if (!pv) return false;
        // 检测范围必须是 code 元素内部:预览自带的复制按钮(.vditor-copy)
        // 里有一个 <svg> 图标,查整个 pre 会把所有代码块误判成图表块,
        // 行号和高亮随之全部失效
        var code = pv.querySelector('code') || pv;
        return !!(code.querySelector && code.querySelector('svg,canvas'));
    }
    // 超大代码块(行数超过此值)不做语法着色/编辑态覆盖层:
    // 每次按键都全文重着色 + 整块 innerHTML 替换会把主线程拖死(卡死)
    var MS_HL_MAX_LINES = 400;
    var MS_HL_MAX_CHARS = 32000;
    // 光标所在块的可视代码元素:编辑态用源码 pre,阅读态用预览 pre。
    // 两者取错会导致行号贴到 display:none 的元素上(矩形全零)
    function activeCodeEl(block) {
        if (!block) return null;
        // 以 Vditor 自己的 --expand 为准(而不是我们的 ms-cb-edit):
        // 显示的到底是 marker 还是 preview,由 Vditor 决定,跟着它走
        if (block.classList.contains('vditor-ir__node--expand')) {
            var mk = block.querySelector('pre.vditor-ir__marker--pre');
            return (mk && (mk.querySelector('code') || mk)) || null;
        }
        var pv = block.querySelector('pre.vditor-ir__preview');
        if (pv) return pv.querySelector('code') || pv;
        var mk2 = block.querySelector('pre.vditor-ir__marker--pre');
        return (mk2 && (mk2.querySelector('code') || mk2)) || null;
    }
    var gutterLayer = null, gutterRaf = 0, hiCol = null, hiPre = null;
    function ensureGutterLayer() {
        var ir = document.querySelector('.vditor-ir');
        if (!ir) { gutterLayer = null; hiCol = null; hiPre = null; return null; }
        if (!gutterLayer || gutterLayer.parentNode !== ir) {
            gutterLayer = document.createElement('div');
            gutterLayer.className = 'ms-gutter-layer';
            gutterLayer.setAttribute('data-ms-ui', 'code');   // 观察器过滤用
            ir.appendChild(gutterLayer);
            hiCol = null;
            hiPre = null;
        }
        return gutterLayer;
    }
    function scheduleGutters() {
        if (gutterRaf) cancelAnimationFrame(gutterRaf);
        gutterRaf = requestAnimationFrame(function () {
            gutterRaf = 0;
            syncGutters();
        });
    }
    function fontCssOf(cs) {
        return 'font-family:' + cs.fontFamily + ';font-size:' + cs.fontSize
            + ';line-height:' + cs.lineHeight + ';letter-spacing:' + cs.letterSpacing
            + ';tab-size:' + (cs.tabSize || 4) + ';';
    }
    // 量出代码文字的【真实】行距:Chromium 对"文本行盒"与"块级 span"
    // 的行进取整会差 1/64px,长块里行号会整体下漂(1200 行累积 ~19px)。
    // 把量出来的行距直接作为每个行号 span 的高度,彻底消除漂移
    function measureLineAdvance(codeEl) {
        try {
            var walker = document.createTreeWalker(codeEl, NodeFilter.SHOW_TEXT);
            var pieces = [], tn, acc = 0, firstNewline = -1;
            while ((tn = walker.nextNode())) {
                var t = tn.textContent || '';
                if (firstNewline < 0) {
                    var localNl = t.indexOf('\n');
                    if (localNl >= 0) firstNewline = acc + localNl;
                }
                pieces.push({ node: tn, start: acc });
                acc += t.length;
                if (firstNewline >= 0 && acc > firstNewline) break;   // 两行就够
            }
            if (firstNewline < 0) return 0;
            function yAt(off) {
                for (var i = 0; i < pieces.length; i++) {
                    var p = pieces[i];
                    var l = off - p.start;
                    if (l >= 0 && l < p.node.textContent.length) {
                        var r = document.createRange();
                        r.setStart(p.node, l);
                        r.setEnd(p.node, Math.min(l + 1, p.node.textContent.length));
                        return r.getBoundingClientRect().top;
                    }
                }
                return null;
            }
            var y1 = yAt(0), y2 = yAt(firstNewline + 1);
            if (y1 === null || y2 === null) return 0;
            var d = y2 - y1;
            return (d > 4 && d < 200) ? d : 0;
        } catch (e) { return 0; }
    }
    function syncGutters() {
        var layer = ensureGutterLayer();
        if (!layer) return;
        var ir = layer.parentNode;
        var irRect = ir.getBoundingClientRect();
        var blocks = ir.querySelectorAll('[data-type="code-block"]');
        // 无行号模式(MarkText 式):行号列一概不生成 —— 从开着切换过来时
        // 把旧列全部拆掉。借鉴 MarkText code.ts:选项关闭时压根不创建
        // 行号 wrapper,不是画了再藏
        if (!showLineNumbers) {
            var old = layer.querySelectorAll('.ms-gutter-col');
            for (var oi = 0; oi < old.length; oi++) old[oi].remove();
        }
        // 先裁掉多余的旧列(块被删掉后遗留),再重新收集 ——
        // 收集与裁剪的顺序颠倒会让 cols 里留着已分离的元素
        var stale = layer.querySelectorAll('.ms-gutter-col');
        while (stale.length > blocks.length) {
            layer.removeChild(stale[stale.length - 1]);
            stale = layer.querySelectorAll('.ms-gutter-col');
        }
        var cols = [];
        for (var ci = 0; ci < layer.children.length; ci++) {
            if (layer.children[ci].classList.contains('ms-gutter-col'))
                cols.push(layer.children[ci]);
        }
        var seenHi = false;
        for (var i = 0; i < blocks.length; i++) {
            var block = blocks[i];
            var col = cols[i];
            // With line numbers off, only the active block needs geometry or
            // source text. Reading every code block on every scroll is costly.
            if (!showLineNumbers && block !== window._msCbEdit) {
                block.classList.remove('ms-cb-hi-ready');
                continue;
            }
            if (showLineNumbers && !col) {
                col = document.createElement('div');
                col.className = 'ms-gutter-col';
                col.setAttribute('data-ms-ui', 'code');
                layer.appendChild(col);
            }
            var info = block.querySelector('.vditor-ir__marker--info');
            var lang = resolveLang(info ? info.textContent : '');
            var codeEl = activeCodeEl(block);
            if (!codeEl || isRenderedBlock(block, lang)) {
                if (col) col.style.display = 'none';
                block.classList.remove('ms-cb-hi-ready');
                continue;
            }
            var text = codeEl.textContent || '';
            var n = text.split(String.fromCharCode(10)).length;
            if (text.length && text.charAt(text.length - 1) === String.fromCharCode(10)) n--;
            if (n < 1) n = 1;
            var cs = getComputedStyle(codeEl);
            var codeRect = codeEl.getBoundingClientRect();
            if (col) {
                col.style.display = 'block';
                if (col.__n !== n) {
                    col.__n = n;
                    col.innerHTML = '';
                    var advance = measureLineAdvance(codeEl);
                    col.__advance = advance;
                    for (var li = 0; li < n; li++) {
                        var s = document.createElement('span');
                        s.textContent = String(li + 1);
                        if (advance) s.style.height = advance + 'px';
                        col.appendChild(s);
                    }
                }
                var blockRect = block.getBoundingClientRect();
                // 样式没变就不写:每次写 cssText 都会触发整层样式重算。
                // 坐标取整:亚像素浮点每帧微差会让 __css 永不相等,
                // 与 observer 形成每帧重写的活环(公式块展开时被放大成卡死)
                var colCss = 'display:block;'
                    + fontCssOf(cs)
                    + 'left:' + Math.round(blockRect.left - irRect.left) + 'px;'
                    + 'top:' + Math.round(codeRect.top - irRect.top) + 'px;';
                if (col.__css !== colCss) {
                    col.__css = colCss;
                    col.style.cssText = colCss;
                }
            }
            // 编辑态高亮列:只给当前光标所在块,字体行高同源,点击落在真文字上
            if (block === window._msCbEdit
                && block.classList.contains('vditor-ir__node--expand')
                && window.hljs && !isRenderedBlock(block, lang)
                && n <= MS_HL_MAX_LINES && text.length <= MS_HL_MAX_CHARS) {
                seenHi = true;
                if (!hiCol || hiCol.parentNode !== layer) {
                    hiCol = document.createElement('div');
                    hiCol.className = 'ms-hi-col';
                    hiCol.setAttribute('data-ms-ui', 'code');
                    layer.appendChild(hiCol);
                }
                hiPre = codeEl.closest('pre');
                var signature = lang + '\n' + text;
                var html = hiCol.__source === signature ? hiCol.__html : highlightCode(text, lang);
                if (html === null) {
                    var d = document.createElement('div');
                    d.textContent = text;
                    html = d.innerHTML;
                }
                if (hiCol.__html !== html) {
                    hiCol.__html = html;
                    hiCol.innerHTML = html;
                }
                hiCol.__source = signature;
                var preRect = hiPre.getBoundingClientRect();
                var hiCss = 'display:block;' + fontCssOf(cs)
                    + 'left:' + (codeRect.left - irRect.left) + 'px;'
                    + 'top:' + (codeRect.top - irRect.top) + 'px;'
                    + 'width:' + Math.max(0, preRect.width - (codeRect.left - preRect.left)) + 'px;';
                if (hiCol.__css !== hiCss) {
                    hiCol.__css = hiCss;
                    hiCol.style.cssText = hiCss;
                }
                // 滚动监听跟着当前 pre 走:两个代码块之间直接跳转时,
                // hiCol 复用但 pre 换了,监听必须重绑到新 pre 上
                if (hiCol._msBoundPre !== hiPre && hiPre) {
                    if (hiCol._msBoundPre) hiCol._msBoundPre.removeEventListener('scroll', scheduleGutters);
                    hiCol._msBoundPre = hiPre;
                    hiPre.addEventListener('scroll', scheduleGutters);
                }
                hiCol.style.transform = 'translate(' + (-(hiPre.scrollLeft || 0)) + 'px,0)';
                block.classList.add('ms-cb-hi-ready');
            } else {
                // 非当前块绝不留 ms-cb-hi-ready:残留它会让源码文字
                // 透明化却没有高亮列兜底,代码看起来"消失"
                block.classList.remove('ms-cb-hi-ready');
            }
        }
        if (!seenHi) {
            if (hiCol && hiCol._msBoundPre) hiCol._msBoundPre.removeEventListener('scroll', scheduleGutters);
            if (hiCol && hiCol.parentNode) hiCol.remove();
            hiCol = null;
            hiPre = null;
        }
    }
    function highlightCode(text, lang) {
        if (!window.hljs || text.length > MS_HL_MAX_CHARS) return null;
        // 只按已知语言着色;未标语言/未知语言保持纯文本(Typora 同款),
        // 不做 auto 猜测 —— 猜错比不上色更糟
        if (!lang || !window.hljs.getLanguage(lang)) return null;
        try {
            return window.hljs.highlight(text, { language: lang, ignoreIllegals: true }).value;
        } catch (e) { return null; }
    }
    function previewRawText(code) {
        if (!code) return '';
        // 行号/测量用的辅助元素都是空节点,textContent 本身就等于源码文本,
        // 不必 cloneNode 整棵子树(大文档下每块克隆一次很贵)
        return code.textContent || '';
    }

    function decorateReadingBlock(block) {
        // --expand 时显示的是 marker(preview 被隐藏),不必装饰 preview
        if (!block || block.classList.contains('vditor-ir__node--expand')) return;
        var preview = block.querySelector('pre.vditor-ir__preview');
        var code = preview && (preview.querySelector('code') || preview);
        if (!code) return;
        var info = block.querySelector('.vditor-ir__marker--info');
        var lang = resolveLang(info ? info.textContent : '');
        // 图表类语言(mermaid/echarts/...):预览是被渲染出来的图形,
        // 动 innerHTML 会把图毁掉,一律不碰
        if (isRenderedBlock(block, lang)) return;
        var raw = previewRawText(code);
        // 超大块跳过重着色(每次输入都整块重写会拖死界面)
        if (raw.length > MS_HL_MAX_CHARS || raw.split('\n').length > MS_HL_MAX_LINES) return;
        ensureHljs();
        // Vditor 自己的 codeRender 已经用同一个 hljs 着过色(span 还在),
        // 我们再整块重写一遍纯属重复劳动 —— 直接记签名跳过。
        // 我们的主题色是按 .hljs-* 类名写的,Vditor 生成的 span 同样命中
        if (window.hljs && code.querySelector('span[class*="hljs-"]')) {
            code.setAttribute('data-ms-hi', lang + '\n' + raw);
            return;
        }
        if (window.hljs) {
            var prevHi = code.getAttribute('data-ms-hi') || '';
            var sig = lang + '\n' + raw;
            if (prevHi !== sig) {
                var html = highlightCode(raw, lang);
                if (html) {
                    code.innerHTML = html;
                    code.classList.add('hljs');
                }
                // 纯文本(未标语言/未知语言)也记签名,避免每次重扫
                code.setAttribute('data-ms-hi', sig);
            }
        }
    }

    function decorateAllCodeBlocks() {
        var root = editorRoot();
        if (!root || !root.querySelectorAll) return;
        var blocks = root.querySelectorAll('[data-type="code-block"]');
        for (var i = 0; i < blocks.length; i++) {
            if (!blocks[i].classList.contains('vditor-ir__node--expand'))
                decorateReadingBlock(blocks[i]);
        }
        decorateMathBlocks();
        scheduleGutters();
    }

    // 公式块装饰(借鉴 MarkText mathPreview):渲染态是 figure 卡片,
    // 空公式挂占位类,错误态由 Vditor 自己写 language-math vditor-reset--error,
    // 我们只负责外观。KaTeX 渲染本身是 Vditor 原生完成的。
    // 两个陷阱:①选择器必须限定 .vditor-ir__node —— 内部 code.language-math(源码)
    // 与 div.language-math(KaTeX 结果)同样带 data-type,宽选择器会把一个块数成三个;
    // ②空判定必须看【源码】而不是渲染结果 —— katex.renderToString('') 也会产出
    // .katex 空壳(这正是 MarkText 用 if(math) 判源码的原因)
    function decorateMathBlocks(blocksOnly) {
        var root = editorRoot();
        if (!root || !root.querySelectorAll) return;
        var nodes = root.querySelectorAll('.vditor-ir__node[data-type="math-block"]');
        for (var i = 0; i < nodes.length; i++) {
            var node = nodes[i];
            var src = node.querySelector('pre.vditor-ir__marker--pre code.language-math');
            var srcText = src ? (src.textContent || '').replace(/[\u200b\s]/g, '') : '';
            // toggle 前先比:值没变就不碰 class,不给 observer 喂变更
            var wantEmpty = !srcText;
            if (node.classList.contains('ms-math-empty') !== wantEmpty)
                node.classList.toggle('ms-math-empty', wantEmpty);
        }
        if (blocksOnly) return;
        // 行内公式节点挂基类(Typora 双态的 CSS 钩子):只认真含 math-inline 的
        var inlines = root.querySelectorAll('.vditor-ir__node[data-type="inline-node"]');
        for (var j = 0; j < inlines.length; j++) {
            var n2 = inlines[j];
            var isMath = !!n2.querySelector('[data-type="math-inline"]');
            if (isMath !== n2.classList.contains('ms-mi'))
                n2.classList.toggle('ms-mi', isMath);
        }
    }

    var langPickerOpen = false;
    var langPickerBlock = null;
    var codeInputComposing = false;

    document.addEventListener('compositionstart', function (e) {
        var root = editorRoot();
        if (root && root.contains(e.target)) codeInputComposing = true;
    }, true);
    document.addEventListener('compositionend', function () { codeInputComposing = false; }, true);

    function codeEditRoot(block) {
        var root = editorRoot();
        return vd && currentMode === 'ir' && root && root.isConnected && block && block.nodeType === 1 && block.isConnected
            && root.contains(block) && block.getAttribute('data-type') === 'code-block' ? root : null;
    }

    function rangeWithin(node, range) {
        return !!(node && range && node.contains(range.startContainer) && node.contains(range.endContainer));
    }

    function resetLangPicker() {
        if (langPickerBlock && langPickerBlock.classList)
            langPickerBlock.classList.remove('ms-lang-editing');
        langPickerOpen = false;
        langPickerBlock = null;
        clearTimeout(langSuggestTimer);
        hideLangSuggest();
    }

    function recordCodeUndo() {
        try { return recordMathUndo(); }
        catch (e) {
            post({ t: 'jserror', msg: String(e && e.stack || e), src: 'recordCodeUndo' });
            return false;
        }
    }

    function langInfoEl(block) {
        if (!codeEditRoot(block)) return null;
        return block.querySelector('.vditor-ir__marker--info')
            || block.querySelector('[data-type="code-block-info"]');
    }

    function ensureLangInfo(block) {
        if (!codeEditRoot(block)) return null;
        var info = langInfoEl(block);
        if (info) return info;
        var openMk = block.querySelector('[data-type="code-block-open-marker"]');
        if (!openMk) return null;
        info = document.createElement('span');
        info.className = 'vditor-ir__marker vditor-ir__marker--info';
        info.setAttribute('data-type', 'code-block-info');
        info.textContent = '';
        if (openMk.nextSibling)
            openMk.parentNode.insertBefore(info, openMk.nextSibling);
        else
            openMk.parentNode.appendChild(info);
        return info;
    }

    function focusLangInfo(block) {
        var root = codeEditRoot(block), s = window.getSelection();
        if (!root || !s || codeInputComposing) return null;
        var inf = ensureLangInfo(block);
        if (!inf || !inf.isConnected) return null;
        if (block.classList) {
            block.classList.add('ms-cb-edit');
            block.classList.add('ms-lang-editing');
        }
        try {
            var r;
            if (s && s.rangeCount) {
                var r0 = s.getRangeAt(0);
                // 已在语言文本里的选区也必须保留，不能把全选折叠掉。
                var inNode = inf.contains(r0.startContainer) && inf.contains(r0.endContainer);
                if (inNode) return inf;
                // 光标在语言文本之外时，移到末尾继续编辑。
                r = document.createRange();
                r.selectNodeContents(inf);
                r.collapse(false);
                s.removeAllRanges();
                s.addRange(r);
            } else {
                r = document.createRange();
                r.selectNodeContents(inf);
                r.collapse(false);
                s.removeAllRanges();
                s.addRange(r);
            }
            root.focus({ preventScroll: true });
        } catch (e) { return null; }
        return inf;
    }

    function editLangInfo(block, key) {
        if (!codeEditRoot(block) || codeInputComposing || typeof key !== 'string') return false;
        var insert = Array.from(key).length === 1 ? key : '';
        if (!insert && key !== 'Backspace' && key !== 'Delete') return false;
        var selection = window.getSelection();
        if (!selection || !selection.rangeCount) return false;
        var selected = selection.getRangeAt(0);
        // 只允许当前代码框内的选区；跨段选区和外部输入不能误写旧语言标记。
        if (!rangeWithin(block, selected)) return false;
        var info = ensureLangInfo(block);
        if (!info) return false;
        var text = (info.textContent || '').replace(/\u200b/g, '');
        var start = text.length, end = start;
        if (rangeWithin(info, selected)) {
            try {
                var prefix = selected.cloneRange();
                prefix.selectNodeContents(info);
                prefix.setEnd(selected.startContainer, selected.startOffset);
                start = prefix.toString().replace(/\u200b/g, '').length;
                end = start + selected.toString().replace(/\u200b/g, '').length;
            } catch (e) { return false; }
        } else if (!selection.isCollapsed || !langPickerOpen || langPickerBlock !== block) {
            return false;
        }
        start = Math.max(0, Math.min(start, text.length));
        end = Math.max(start, Math.min(end, text.length));
        // UTF-16 代理对必须一起删除，避免粘贴的 Unicode 字符变成半个字符。
        if (start === end && key === 'Backspace' && start > 0) {
            start--;
            if (start > 0 && /[\uDC00-\uDFFF]/.test(text.charAt(start))
                && /[\uD800-\uDBFF]/.test(text.charAt(start - 1))) start--;
        }
        if (start === end && key === 'Delete' && end < text.length) {
            end++;
            if (/[\uD800-\uDBFF]/.test(text.charAt(end - 1))
                && /[\uDC00-\uDFFF]/.test(text.charAt(end))) end++;
        }
        if (start === end && !insert) return true; // 边界按键已处理，保留代码框。
        clearStaleCaretBookmarks();
        if (!recordCodeUndo()) return true; // 已命中此编辑，阻止原生逻辑绕过撤销保护。
        var node = document.createTextNode(text.slice(0, start) + insert + text.slice(end));
        info.replaceChildren(node);
        var range = document.createRange();
        range.setStart(node, start + insert.length); range.collapse(true);
        selection.removeAllRanges(); selection.addRange(range);
        recordCodeUndo();
        suppressUntil = 0;
        onInput(vd.getValue());
        scheduleLangSuggest();
        return true;
    }

    function openLangPicker(block) {
        if (!codeEditRoot(block) || codeInputComposing) return;
        langPickerOpen = true;
        langPickerBlock = block;
        window._msCbEdit = block;
        if (!focusLangInfo(block)) { resetLangPicker(); return; }
        scheduleLangSuggest();
        setTimeout(function () {
            var root = codeEditRoot(block);
            if (!langPickerOpen || langPickerBlock !== block || !root
                || !root.contains(document.activeElement)) return;
            var selection = window.getSelection();
            if (!selection || !selection.rangeCount || !rangeWithin(block, selection.getRangeAt(0))) {
                resetLangPicker(); return;
            }
            focusLangInfo(block);
            scheduleLangSuggest();
        }, 80);
    }

    function closeLangPicker(applyHit) {
        if (!langPickerOpen && !applyHit) {
            hideLangSuggest();
            return;
        }
        var block = langPickerBlock;
        langPickerOpen = false;
        langPickerBlock = null;
        if (applyHit && langSuggestHits[langSuggestIdx]) {
            applyLangChoice(langSuggestHits[langSuggestIdx][0]);
            return;
        }
        hideLangSuggest();
        if (block && block.classList) {
            block.classList.remove('ms-lang-editing');
            block.classList.add('vditor-ir__node--expand');
            // 把光标从 info 节点(语言标记)移到代码正文区,避免后续 Backspace
            // 被"空代码块删除"逻辑截获把整块删掉
            var source = block.querySelector('pre.vditor-ir__marker--pre > code');
            if (source) {
                var range = document.createRange();
                range.selectNodeContents(source); range.collapse(true);
                var selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
                editorRoot().focus({ preventScroll: true });
            }
        }
        scheduleGutters();
        decorateAllCodeBlocks();
        syncCbEditState();   // 按钮与编辑态类立即复位,不等下一帧
    }

    function syncCodeLangBtn(block) {
        // 只有"当前光标所在的代码块"才显示按钮:离开代码块后 codeLangBlock
        // 还留着旧引用,滚动重算若只认它,按钮会在光标离开后又冒出来
        var inCode = block && block === window._msCbEdit
                     && block.getAttribute
                     && block.getAttribute('data-type') === 'code-block';
        if (!inCode && !langPickerOpen) {
            if (codeLangBtn) codeLangBtn.style.display = 'none';
            hideLangSuggest();
            return;
        }
        if (langPickerOpen && langPickerBlock)
            block = langPickerBlock;
        codeLangBlock = block;
        if (langPickerOpen || (block && block.classList.contains('ms-lang-editing'))) {
            if (codeLangBtn) codeLangBtn.style.display = 'none';
            scheduleLangSuggest();
            return;
        }
        hideLangSuggest();
        var info = langInfoEl(block);
        var lang = info ? (info.textContent || '').replace(/[\u200b]/g, '').trim() : '';
        if (!codeLangBtn) {
            codeLangBtn = document.createElement('div');
            codeLangBtn.id = 'ms-code-lang';
            // mousedown 不抢编辑器焦点;点击后打开语言候选(Typora:点右下角语言)
            codeLangBtn.addEventListener('mousedown', function (e) {
                e.preventDefault();
                e.stopPropagation();
                var b = currentCodeBlock() || window._msCbEdit || codeLangBlock;
                if (!b || !b.getAttribute || b.getAttribute('data-type') !== 'code-block')
                    return;
                openLangPicker(b);
            });
            document.body.appendChild(codeLangBtn);
        }
        var id = resolveLang(lang);
        var meta = langMeta(id);
        ensureLangIcons();
        var label = lang ? meta[1] : '代码语言';
        codeLangBtn.innerHTML = (lang ? langBadgeHtml(meta) : '')
            + '<span>' + label + '</span>';
        codeLangBtn.style.display = 'flex';
        var rect = block.getBoundingClientRect();
        var w = codeLangBtn.offsetWidth || 88;
        var h = codeLangBtn.offsetHeight || 24;
        codeLangBtn.style.left = Math.round(rect.right - w - 10) + 'px';
        codeLangBtn.style.top = Math.round(rect.bottom - h - 8) + 'px';
        if (rect.bottom < 0 || rect.top > window.innerHeight)
            codeLangBtn.style.display = 'none';
    }

    var langSuggestEl = null, langSuggestIdx = 0, langSuggestHits = [], langSuggestTimer = 0;
    function ensureLangSuggest() {
        if (langSuggestEl) return langSuggestEl;
        langSuggestEl = document.createElement('div');
        langSuggestEl.id = 'ms-lang-suggest';
        langSuggestEl.hidden = true;
        langSuggestEl.addEventListener('mousedown', function (e) {
            e.preventDefault();
            e.stopPropagation();
            var item = e.target;
            while (item && item !== langSuggestEl) {
                if (item.getAttribute && item.getAttribute('data-lang')) {
                    applyLangChoice(item.getAttribute('data-lang'));
                    return;
                }
                var role = item.getAttribute && item.getAttribute('data-role');
                if (role === 'setdoc') { setDocDefaultLang(); return; }
                if (role === 'cleardoc') { clearDocDefaultLang(); return; }
                item = item.parentElement;
            }
        });
        document.body.appendChild(langSuggestEl);
        return langSuggestEl;
    }
    function hideLangSuggest() {
        if (langSuggestEl) langSuggestEl.hidden = true;
        langSuggestHits = [];
        langSuggestIdx = 0;
    }
    function filterLangs(q) {
        q = String(q || '').replace(/[\u200b]/g, '').trim().toLowerCase();
        var ranked = [];
        for (var i = 0; i < LANG_CATALOG.length; i++) {
            var id = LANG_CATALOG[i][0], label = LANG_CATALOG[i][1].toLowerCase();
            var score = -1;
            if (!q) score = 50 + i;
            else if (id === q || label === q) score = 0;
            else if (id.indexOf(q) === 0 || label.indexOf(q) === 0) score = 1;
            else if (id.indexOf(q) >= 0 || label.indexOf(q) >= 0) score = 2;
            else {
                for (var alias in LANG_ALIAS) {
                    if (LANG_ALIAS[alias] !== id) continue;
                    if (alias === q) { score = 0; break; }
                    if (alias.indexOf(q) === 0) { score = 1; break; }
                }
            }
            if (score >= 0) ranked.push({ score: score, i: i, row: LANG_CATALOG[i] });
        }
        ranked.sort(function (a, b) { return a.score - b.score || a.i - b.i; });
        var out = [];
        for (var j = 0; j < ranked.length && out.length < 10; j++) out.push(ranked[j].row);
        return out;
    }
    // 语言官方图标(simple-icons 的单色路径,随工程离线分发)。
    // 首次打开候选时懒加载,加载完重渲染一次;缺失的语言回退字母徽标
    var langIconCache = {};
    var langIconsLoading = false, langIconsDone = false;
    function ensureLangIcons() {
        if (langIconsLoading || langIconsDone) return;
        langIconsLoading = true;
        var ids = [];
        for (var i = 0; i < LANG_CATALOG.length; i++) ids.push(LANG_CATALOG[i][0]);
        var pending = ids.length;
        ids.forEach(function (id) {
            fetch('icons/' + id + '.svg').then(function (r) {
                if (!r.ok) throw new Error('miss');
                return r.text();
            }).then(function (t) {
                var m = /<svg[^>]*>([\s\S]*)<\/svg>/i.exec(t);
                if (m) {
                    langIconCache[id] = m[1].replace(/<title>[\s\S]*?<\/title>/gi, '')
                                            .replace(/\s*fill="[^"]*"/gi, '');
                }
            }).catch(function () {}).then(function () {
                if (--pending <= 0) {
                    langIconsLoading = false;
                    langIconsDone = true;
                    if (langSuggestEl && !langSuggestEl.hidden) syncLangSuggest();
                }
            });
        });
    }
    function langBadgeHtml(row) {
        var icon = langIconCache[row[0]];
        if (icon) {
            return '<span class="ms-lang-ico ms-lang-ico--svg" style="color:' + row[2] + '">'
                 + '<svg viewBox="0 0 24 24" aria-hidden="true">' + icon + '</svg></span>';
        }
        return '<span class="ms-lang-ico" style="background:' + row[2] + '">' + row[3] + '</span>';
    }
    function renderLangSuggest(hits, active) {
        var el = ensureLangSuggest();
        ensureLangIcons();
        langSuggestHits = hits;
        langSuggestIdx = active < 0 ? 0 : active;
        if (!hits.length) { el.hidden = true; return; }
        var html = '';
        for (var i = 0; i < hits.length; i++) {
            var row = hits[i];
            html += '<div class="ms-lang-item' + (i === langSuggestIdx ? ' is-active' : '')
                + '" data-lang="' + row[0] + '">'
                + langBadgeHtml(row)
                + '<span>' + row[1] + '</span></div>';
        }
        // 底部:本文档默认语言(每文档独立,按路径持久化,不写进 md)
        if (docLang) {
            var cur = langMeta(resolveLang(docLang));
            html += '<div class="ms-lang-foot" data-role="cleardoc">'
                 + langBadgeHtml(cur)
                 + '<span>★ 本文档默认:' + cur[1] + '(点击取消)</span></div>';
        } else if (langSuggestHits[langSuggestIdx]) {
            html += '<div class="ms-lang-foot" data-role="setdoc">'
                 + '<span>★ 设为本文档默认:' + langSuggestHits[langSuggestIdx][1] + '</span></div>';
        }
        el.innerHTML = html;
        el.hidden = false;
        // 候选锚在【代码块右下角】(与"代码语言"按钮同位)。旧实现锚在
        // ```lang 语言标记上,块一高候选就飘到块顶左上角,和用户点击的
        // 位置完全脱节;块滚出视口后更会落在无关内容上
        var anchorBlock = (langPickerBlock && langPickerBlock.isConnected)
            ? langPickerBlock
            : (window._msCbEdit && window._msCbEdit.isConnected ? window._msCbEdit : codeLangBlock);
        if (!anchorBlock || !anchorBlock.isConnected) return;
        var rect = anchorBlock.getBoundingClientRect();
        var box = el.getBoundingClientRect();
        var w = box.width, h = box.height;
        var left = rect.right - w - 10;   // 右缘与代码块对齐(按钮同款 10px 内缩)
        var top = rect.bottom + 6;        // 默认挂块下方
        if (top + h > window.innerHeight - 8 && rect.top - h - 6 > 8)
            top = rect.top - h - 6;       // 下方放不下且上方有空间 → 挂块上方
        el.style.left = Math.max(8, Math.min(left, window.innerWidth - w - 8)) + 'px';
        el.style.top = Math.max(8, Math.min(top, window.innerHeight - h - 8)) + 'px';
    }
    function scheduleLangSuggest() {
        if (langSuggestTimer) clearTimeout(langSuggestTimer);
        langSuggestTimer = setTimeout(syncLangSuggest, 40);
    }
    function syncLangSuggest() {
        if (codeInputComposing) return;
        // 语言选择打开期间,即使 Vditor 把光标抢回代码区,候选也不能关。
        // 旧实现要求"光标必须在 info 节点里",插入代码块后光标在正文,
        // 候选永远弹不出来 —— 用户看到的就是"没有语言 Logo 列表"
        var block = langPickerBlock || currentCodeBlock() || window._msCbEdit || codeLangBlock;
        if (!codeEditRoot(block)) { resetLangPicker(); return; }
        if (!editorRoot().contains(document.activeElement)) { hideLangSuggest(); return; }
        var selection = window.getSelection();
        if (!selection || !selection.rangeCount || !rangeWithin(block, selection.getRangeAt(0))) {
            resetLangPicker(); return;
        }
        var info = langInfoEl(block);
        var inInfo = false;
        if (info) {
            var selL = window.getSelection();
            if (selL && selL.rangeCount) {
                var sc = selL.getRangeAt(0).startContainer;
                inInfo = sc === info || (info.contains && info.contains(sc));
            }
        }
        if (!langPickerOpen && !inInfo) { hideLangSuggest(); return; }
        block.classList.add('ms-lang-editing');
        if (langPickerOpen && !inInfo)
            focusLangInfo(block);
        var q = ((info && info.textContent) || '').replace(/[\u200b]/g, '');
        var hits = filterLangs(q);
        var keep = 0;
        if (langSuggestHits.length) {
            var cur = langSuggestHits[langSuggestIdx] && langSuggestHits[langSuggestIdx][0];
            for (var hi = 0; hi < hits.length; hi++) {
                if (hits[hi][0] === cur) { keep = hi; break; }
            }
        }
        renderLangSuggest(hits, keep);
    }
    function applyLangChoice(id) {
        var block = langPickerBlock || window._msCbEdit || codeLangBlock;
        if (!block) return;
        var info = ensureLangInfo(block);
        if (!info) return;
        info.textContent = id;
        langPickerOpen = false;
        langPickerBlock = null;
        hideLangSuggest();
        block.classList.remove('ms-lang-editing');
        try {
            var code = block.querySelector('pre.vditor-ir__marker--pre code')
                    || block.querySelector('pre.vditor-ir__marker--pre')
                    || block.querySelector('pre.vditor-ir__preview code');
            if (code) {
                var r = document.createRange();
                r.selectNodeContents(code);
                r.collapse(true);
                var s = window.getSelection();
                s.removeAllRanges();
                s.addRange(r);
            }
        } catch (e) {}
        rerender();
        scheduleGutters();
        decorateAllCodeBlocks();
        if (vd) vd.focus();
    }

    // 本文档默认语言:把当前高亮候选设为默认(或取消)。默认只影响
    // 以后新建的块,当前块的语言不被它改动 —— 手动选择仍走 applyLangChoice
    function setDocDefaultLang() {
        var id = langSuggestHits[langSuggestIdx] && langSuggestHits[langSuggestIdx][0];
        if (!id) return;
        docLang = id;
        post({ t: 'doclang', lang: id });
        langPickerOpen = false;
        langPickerBlock = null;
        hideLangSuggest();
        if (vd) vd.focus();
    }
    function clearDocDefaultLang() {
        docLang = '';
        post({ t: 'doclang', lang: '' });
        hideLangSuggest();
        syncLangSuggest();
    }
    document.addEventListener('keydown', function (e) {
        var root = editorRoot();
        if (codeInputComposing || e.isComposing || e.keyCode === 229 || e.defaultPrevented
            || !e.cancelable || !root || !root.contains(e.target)) return;
        var suggestVisible = langSuggestEl && !langSuggestEl.hidden;
        if (!suggestVisible && !langPickerOpen) return;
        if (e.key === 'ArrowDown') {
            e.preventDefault(); e.stopPropagation();
            if (langSuggestHits.length)
                renderLangSuggest(langSuggestHits, (langSuggestIdx + 1) % langSuggestHits.length);
        } else if (e.key === 'ArrowUp') {
            e.preventDefault(); e.stopPropagation();
            if (langSuggestHits.length)
                renderLangSuggest(langSuggestHits, (langSuggestIdx - 1 + langSuggestHits.length) % langSuggestHits.length);
        } else if (e.key === 'Enter' || e.key === 'Tab') {
            if (langSuggestHits[langSuggestIdx]) {
                e.preventDefault(); e.stopPropagation();
                applyLangChoice(langSuggestHits[langSuggestIdx][0]);
            }
        } else if (e.key === 'Escape') {
            e.preventDefault(); e.stopImmediatePropagation();
            closeLangPicker(false);
        } else if (langPickerOpen && !e.ctrlKey && !e.altKey && !e.metaKey) {
            // Vditor 会把焦点抢回代码正文:语言选择期间字母/退格一律写进 info
            var block = langPickerBlock || currentCodeBlock();
            if (e.key === 'Backspace' || e.key === 'Delete' || Array.from(e.key).length === 1) {
                // 同一按键只处理一次，不再落入后面的空代码框删除监听器。
                if (editLangInfo(block, e.key)) {
                    e.preventDefault(); e.stopImmediatePropagation();
                }
            }
        }
    }, true);
    // 点外部关闭语言候选:langPickerOpen 期间旧实现没有任何"点空白退出"
    // 的路径,反而由 syncLangSuggest 把光标一次次拽回语言标记 —— 用户看到
    // 的就是"怎么点都在、像卡死"。Esc/Enter/点候选项仍然有效;这里补上
    // 鼠标路径:点击候选面板、语言标记、"代码语言"按钮之外任意处即关闭。
    // 在 mousedown 捕获阶段关(早于浏览器放光标),关闭本身不动光标,
    // 点击落点交给浏览器默认行为,不打架
    document.addEventListener('mousedown', function (e) {
        if (!langPickerOpen || e.button !== 0) return;
        var t = e.target;
        if (langSuggestEl && !langSuggestEl.hidden && langSuggestEl.contains(t)) return;
        if (codeLangBtn && codeLangBtn.contains(t)) return; // 按钮自己有 open 逻辑
        var block = langPickerBlock;
        var info = block ? langInfoEl(block) : null;
        if (info && info.contains(t)) return;               // 继续编辑语言文本
        langPickerOpen = false;
        langPickerBlock = null;
        hideLangSuggest();
        if (block && block.classList) {
            block.classList.remove('ms-lang-editing');
            // 点外部退出语言选择态:把光标从 info 节点(语言标记)移回代码正文区。
            // 否则光标停在 .vditor-ir__marker--info 里,后续 Backspace 会被
            // "空代码块删除"处理器截获,把整个代码块删掉
            var src = block.querySelector('pre.vditor-ir__marker--pre > code');
            if (src) {
                var rng = document.createRange();
                rng.selectNodeContents(src); rng.collapse(true);
                var s = getSelection(); s.removeAllRanges(); s.addRange(rng);
                editorRoot().focus({ preventScroll: true });
            }
        }
        syncCbEditState();                                  // 按钮与编辑态类立即复位
    }, true);
    // 退出代码编辑态:把光标移到相邻块(下一个块落到块首,否则上一个块落到块尾)。
    // 用 Esc 触发 —— 点空白退出的方案会把"点空白放光标"的正常操作吃掉,
    // 长块铺满屏幕时 Esc 是最明确、不冲突的退出方式
    function exitCodeEdit(block) {
        var sib = block.nextElementSibling || block.previousElementSibling;
        if (!sib) return false;
        try {
            var rng = document.createRange();
            rng.selectNodeContents(sib);
            rng.collapse(!block.nextElementSibling);
            var s = window.getSelection();
            s.removeAllRanges();
            s.addRange(rng);
            // Vditor 不跟随程序化选区变化,展开态要手动收起
            block.classList.remove('vditor-ir__node--expand');
            block.classList.remove('ms-cb-edit');
            block.classList.remove('ms-lang-editing');
            if (window._msCbEdit === block) window._msCbEdit = null;
            decorateReadingBlock(block);
            syncCodeLangBtn(null);
            scheduleGutters();
            return true;
        } catch (e) { return false; }
    }
    function removeEmptyCodeBlock(block) {
        var root = codeEditRoot(block), selection = window.getSelection();
        if (!root || !selection || !selection.rangeCount || codeInputComposing) return false;
        var range = selection.getRangeAt(0), code = activeCodeEl(block);
        if (!rangeWithin(block, range) || !code || (code.textContent || '').replace(/[\u200b\s]/g, ''))
            return false;
        var parent = block.parentElement;
        clearStaleCaretBookmarks();
        if (!recordCodeUndo()) return true;
        var previous = block.previousElementSibling, next = block.nextElementSibling;
        function emptyParagraph(node) {
            return node && node.tagName === 'P' && !node.textContent.replace(/[\u200b\s]/g, '')
                && !node.matches('[contenteditable="false"],[data-ms-ui],[data-type]')
                && !node.querySelector('img,video,audio,iframe,table,embed,object,hr,input,button,select,textarea,svg,canvas,[data-type],'
                    + '[contenteditable="false"],[data-ms-ui]');
        }
        // 只清理代码框紧邻的空段，不改写其他段落或整篇文档的空行。
        while (emptyParagraph(previous)) {
            var before = previous.previousElementSibling; previous.remove(); previous = before;
        }
        while (emptyParagraph(next)) {
            var after = next.nextElementSibling; next.remove(); next = after;
        }
        block.remove();
        resetLangPicker();
        window._msCbEdit = null; codeLangBlock = null;
        if (codeLangBtn) codeLangBtn.style.display = 'none';
        var target = previous || next;
        if (!target) {
            target = document.createElement('p'); target.setAttribute('data-block', '0');
            target.appendChild(document.createTextNode('\u200b')); parent.appendChild(target);
        }
        root.focus({ preventScroll: true });
        if (!placeCaretInBlock(target, !!previous)) {
            // 邻接块不可编辑时，在原位置附近保留一个可继续输入的段落。
            var fallback = document.createElement('p'); fallback.setAttribute('data-block', '0');
            fallback.appendChild(document.createTextNode('\u200b'));
            parent.insertBefore(fallback, next);
            placeCaretInBlock(fallback, false);
        }
        recordCodeUndo();
        suppressUntil = 0;
        onInput(vd.getValue());
        syncCbEditState();
        scheduleGutters();
        return true;
    }
    // 完全空的代码块里按 Delete/Backspace:删掉整块。
    // Vditor 对代码块边界的删除有合并/换行逻辑,在空块上会误插换行;
    // 旧实现直接吞掉按键,表现为"删不掉空代码块"。
    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Backspace' && e.key !== 'Delete') return;
        var root = editorRoot();
        if (e.ctrlKey || e.altKey || e.metaKey || codeInputComposing || e.isComposing || e.keyCode === 229
            || e.defaultPrevented || !e.cancelable || !root || !root.contains(e.target)) return;
        var block = currentCodeBlock();
        if (!codeEditRoot(block) || !block.classList.contains('vditor-ir__node--expand')) return;
        var codeEl = activeCodeEl(block);
        if (!codeEl) return;
        // 【核心修复】光标在语言标记 (.vditor-ir__marker--info) 里时:Backspace/Delete
        // 是编辑【语言文本】,绝不能落到下面的"空代码块删整块"逻辑
        var selI = window.getSelection();
        if (selI && selI.rangeCount) {
            var infoI = langInfoEl(block);
            var cI = selI.getRangeAt(0).startContainer;
            if (infoI && (cI === infoI || (infoI.contains && infoI.contains(cI)))) {
                if (editLangInfo(block, e.key)) {
                    e.preventDefault(); e.stopImmediatePropagation();
                }
                return;
            }
        }
        if ((codeEl.textContent || '').replace(/[\u200b\s]/g, '')) {
            // 全选删除:选区覆盖全部代码时,清内容但保留代码框(Typora 语义:
            // 删空代码不拆框,再按一次删除才删框)。Vditor 原生会把框一起拆掉
            var selAll = window.getSelection();
            if (selAll && selAll.rangeCount && !selAll.isCollapsed) {
                var rAll = selAll.getRangeAt(0);
                if (codeEl.contains(rAll.commonAncestorContainer)) {
                    var whole = document.createRange();
                    whole.selectNodeContents(codeEl);
                    if (rAll.compareBoundaryPoints(Range.START_TO_START, whole) <= 0
                        && rAll.compareBoundaryPoints(Range.END_TO_END, whole) >= 0) {
                        e.preventDefault();
                        e.stopPropagation();
                        try {
                            whole.deleteContents();
                            var tn = document.createTextNode('');
                            codeEl.appendChild(tn);
                            var rc = document.createRange();
                            rc.setStart(tn, 0);
                            rc.collapse(true);
                            selAll.removeAllRanges();
                            selAll.addRange(rc);
                            rerender();
                            scheduleGutters();
                            onInput();
                            // Vditor 异步重渲染可能换元素:补挂展开态和光标,
                            // 保证下一次删除操作能命中"空块删框"路径
                            setTimeout(function () {
                                try {
                                    // 用户已移动光标或切换文档时，旧回调不能抢走新光标。
                                    var cb2 = currentCodeBlock();
                                    if (!codeEditRoot(block) || cb2 !== block || codeInputComposing) return;
                                    var code2 = activeCodeEl(cb2);
                                    if (!code2) return;
                                    cb2.classList.add('vditor-ir__node--expand');
                                    var rn = document.createRange();
                                    rn.selectNodeContents(code2);
                                    rn.collapse(true);
                                    var s2 = window.getSelection();
                                    s2.removeAllRanges();
                                    s2.addRange(rn);
                                    syncCbEditState();
                                } catch (e2) {}
                            }, 150);
                        } catch (errAll) {}
                        return;
                    }
                }
            }
            // 非空块的边界保护(Typora 语义,区别于 MarkText 的"退格转段落"):
            // 光标折叠在代码绝对开头按 Backspace / 绝对末尾按 Delete 时,
            // Vditor 会拆围栏把裸代码漏进正文 —— 拦下,块不动。
            // 正常位置(前面/后面还有字符)照常删除。
            var selB = window.getSelection();
            if (selB && selB.rangeCount && selB.isCollapsed) {
                var rb = selB.getRangeAt(0);
                var cn = e.key === 'Backspace' ? rb.startContainer : rb.endContainer;
                var co = e.key === 'Backspace' ? rb.startOffset : rb.endOffset;
                if (codeEl.contains(cn)) {
                    try {
                        var probeB = document.createRange();
                        probeB.selectNodeContents(codeEl);
                        if (e.key === 'Backspace')
                            probeB.setEnd(cn, co);
                        else
                            probeB.setStart(cn, co);
                        // ZWSP 是渲染伪影不算内容;真实空格算(逐格回删)
                        if (!probeB.toString().replace(/\u200b/g, '')) {
                            e.preventDefault();
                            e.stopPropagation();
                            return;
                        }
                    } catch (eB) {}
                }
            }
            return;
        }
        // 就地移除：相邻段落、光标与滚动位置不会被 setValue 全文重建破坏。
        if (removeEmptyCodeBlock(block)) {
            e.preventDefault(); e.stopImmediatePropagation();
        }
    }, true);

    // 光标是否在本块"内容行首":块首到光标之间只剩标记元素(标题 # 、
    // 引用 > 、列表符、<font> 这类行内标签)和零宽空格才算。标题 Backspace
    // 与 Enter 两个处理器共用
    function caretAtContentStart(block) {
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount || !sel.isCollapsed) return false;
        var r = sel.getRangeAt(0);
        if (!block.contains(r.startContainer)) return false;
        var probe = document.createRange();
        probe.selectNodeContents(block);
        try { probe.setEnd(r.startContainer, r.startOffset); } catch (e) { return false; }
        var holder = document.createElement('div');
        holder.appendChild(probe.cloneContents());
        holder.querySelectorAll('.vditor-ir__marker, [data-type="heading-marker"], span.vditor-ir__node')
            .forEach(function (n) {
                var t = (n.textContent || '').trim();
                if (/^<[^>]+>$/.test(t) || /^[#>`~\-+*.\d\s]*$/.test(t))
                    n.remove();
            });
        return holder.textContent.replace(/\u200b/g, '').trim() === '';
    }

    // 光标是否在本块"内容行尾"(行首判定的镜像,Delete 处理器用)
    function caretAtContentEnd(block) {
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount || !sel.isCollapsed) return false;
        var r = sel.getRangeAt(0);
        if (!block.contains(r.startContainer)) return false;
        var probe = document.createRange();
        probe.selectNodeContents(block);
        try { probe.setStart(r.startContainer, r.startOffset); } catch (e) { return false; }
        var holder = document.createElement('div');
        holder.appendChild(probe.cloneContents());
        holder.querySelectorAll('.vditor-ir__marker, [data-type="heading-marker"], span.vditor-ir__node')
            .forEach(function (n) {
                var t = (n.textContent || '').trim();
                if (/^<[^>]+>$/.test(t) || /^[#>`~\-+*.\d\s]*$/.test(t))
                    n.remove();
            });
        return holder.textContent.replace(/\u200b/g, '').trim() === '';
    }

    // 行首 Backspace 且上一兄弟是空段:删掉空段,保住本块格式。
    // Vditor 原生是把本块并进空段 —— 标题级别、<font> 颜色在合并里全丢,
    // 用户看到的就是"删掉空行后标题变普通文本、颜色没了"。
    // 判定"行首"用 DOM 剥标记:块首到光标之间只剩标记元素(标题的 # 、
    // 引用 > 、列表符、<font> 标签等)和零宽空格,才算内容行首
    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Backspace' || e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return;
        if (!vd || currentMode !== 'ir') return;
        if (currentCodeBlock()) return;             // 代码块有自己的删除语义
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount || !sel.isCollapsed) return;
        var block = caretBlock();
        if (!block) return;
        var prev = block.previousElementSibling;
        // 空行不限于 P:标题上方的空行是空 H1-H6(Vditor 让空行继承标题级),
        // 引用里的空行是空 BLOCKQUOTE。列表/表格结构不碰
        if (!prev || !/^(P|H[1-6]|BLOCKQUOTE)$/.test(prev.tagName)) return;
        if ((prev.textContent || '').replace(/\u200b/g, '').trim()) return;  // 上一段非空
        if (prev.querySelector('img')) return;
        if (!caretAtContentStart(block)) return; // 光标不在内容行首
        e.preventDefault();
        e.stopPropagation();
        try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e3) {}
        // 删掉空段后整篇序列化往返:只 prev.remove()+rerender() 的话,Vditor
        // 就地重解析会把段首 <font ...> 这类行内 HTML 标记拆坏(实测丢 '>' 变纯文本)
        // 纯 DOM 删除,不走 getValue/setValue 整篇往返:往返会让 Lute 把
        // 连续空行折叠掉 —— 多行空行上按一下就全没了("跳到上一行有内容
        // 的位置"的根源)。也不 rerender:就地重解析会拆坏段首 <font> 标记。
        // 只同步标脏(onInput),DOM 与光标原样保留,一次只删一个空段
        prev.remove();
        try { onInput(vd.getValue()); } catch (e4) {}
        scheduleGutters();
    }, true);

    // 块末尾 Delete 且下一兄弟是空块:只删掉那个空块。
    // Vditor 原生会"穿透空行"把下一个内容块也合并进来(实测
    // "甲甲/空/乙乙"一键变"甲甲乙乙"同段) —— 静默合并后用户再按
    // Ctrl+1,两行字一起变标题;再连续按,内容一片片消失
    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Delete' || e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return;
        if (!vd || currentMode !== 'ir') return;
        if (currentCodeBlock()) return;             // 代码块有自己的删除语义
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount || !sel.isCollapsed) return;
        var block = caretBlock();
        if (!block) return;
        // 标题内容行首的 Delete:只删第一个可见字符,标题级别不动。
        // Vditor 原生会把标题级别一起删掉("按一下删除,标题没了")
        if (/^H[1-6]$/.test(block.tagName) && caretAtContentStart(block)) {
            var walker = document.createTreeWalker(block, NodeFilter.SHOW_TEXT);
            var n, target = null;
            while ((n = walker.nextNode())) {
                var pn = n.parentElement;
                if (pn && pn.closest && pn.closest('.vditor-ir__marker, [data-type="heading-marker"]'))
                    continue;
                if (n.textContent.replace(/\u200b/g, '')) { target = n; break; }
            }
            if (target) {
                e.preventDefault();
                e.stopPropagation();
                try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e5) {}
                var before = target.textContent;
                target.textContent = before.replace(/^(\u200b)?./, '');
                var dr = document.createRange();
                dr.setStart(target, 0);
                dr.collapse(true);
                var ds = window.getSelection();
                ds.removeAllRanges();
                ds.addRange(dr);
                editorRoot().focus({ preventScroll: true });
                // 不 rerender(避免整篇重解析折叠连续空行/拆坏行内标记),
                // 只同步标脏
                try { onInput(vd.getValue()); } catch (e6) {}
                scheduleGutters();
                return;
            }
        }
        var next = block.nextElementSibling;
        if (!next || !/^(P|H[1-6]|BLOCKQUOTE)$/.test(next.tagName)) return;
        if ((next.textContent || '').replace(/\u200b/g, '').trim()) return; // 非空块:交给原生合并
        if (next.querySelector('img')) return;
        if (!caretAtContentEnd(block)) return;      // 光标不在内容行尾
        e.preventDefault();
        e.stopPropagation();
        try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e3) {}
        // 纯 DOM 删除(理由同 Backspace 处理器):一次只删一个空块
        next.remove();
        try { onInput(vd.getValue()); } catch (e4) {}
        scheduleGutters();
    }, true);

    // 内容行首 Enter 的两处后置矫正:
    // ① 标题:Vditor 会在上面插一个【空标题】(隐形细线,点进去打字秒变
    //   标题)→ 降级成普通空段(Typora 语义);
    // ② 行内 HTML 开标签(<font ...>)开头的块:Vditor 的 Enter 会把开标签
    //   切进上一段,标签对跨块断裂 → 挪回本段开头,上一段清空
    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Enter' || e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return;
        if (!vd || currentMode !== 'ir') return;
        if (currentCodeBlock()) return;
        var hb = caretBlock();
        if (!hb) return;
        var isHeading = /^H[1-6]$/.test(hb.tagName);
        var hasInlineTag = !!(hb.firstElementChild
            && hb.firstElementChild.getAttribute
            && hb.firstElementChild.getAttribute('data-type') === 'html-inline');
        if (!isHeading && !hasInlineTag) return;
        if (!caretAtContentStart(hb)) return;   // 只在内容行首才管
        // 行内 HTML 开标签(<font ...>)开头的块:Vditor 的 Enter 会把标签对
        // 拆散(上一段变裸标签文本/垃圾 DIV,实测不可修复) —— capture 阶段
        // 直接拦截,自己上面插一个空段(Typora 语义),本块与光标都不动
        if (hasInlineTag && !isHeading) {
            e.preventDefault();
            e.stopPropagation();
            try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (eT) {}
            var empty = document.createElement('p');
            empty.setAttribute('data-block', '0');
            hb.parentNode.insertBefore(empty, hb);
            try { onInput(vd.getValue()); } catch (eE) {}
            scheduleGutters();
            return;
        }
        setTimeout(function () {
            try {
                var cur = caretBlock();
                if (!cur) return;
                var demote = null;
                if (/^H[1-6]$/.test(cur.tagName)
                    && !(cur.textContent || '').replace(/\u200b/g, '').trim()) {
                    demote = cur;                       // 光标自己落进空标题
                } else if (/^H[1-6]$/.test(cur.tagName)) {
                    var prev = cur.previousElementSibling;
                    if (prev && /^H[1-6]$/.test(prev.tagName)
                        && !(prev.textContent || '').replace(/\u200b/g, '').trim())
                        demote = prev;                  // 上方空标题
                }
                if (demote) {
                    var p = document.createElement('p');
                    p.setAttribute('data-block', '0');
                    demote.replaceWith(p);
                    try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e2) {}
                    // 纯 DOM 替换,不 rerender(避免整篇重解析的连锁副作用)
                    try { onInput(vd.getValue()); } catch (e5) {}
                    scheduleGutters();
                    return;
                }
                // 开标签被切进上一段:挪回本段开头,上一段换成正规空段。
                // 上一段可能是 Vditor 临时造的 DIV(垃圾块),一并替换
                var pv = cur.previousElementSibling;
                if (!pv || !/^(P|H[1-6]|DIV)$/.test(pv.tagName)) return;
                var stray = pv.querySelectorAll('span[data-type="html-inline"]');
                if (stray.length !== 1) return;
                var rest = (pv.textContent || '').replace(stray[0].textContent, '')
                    .replace(/\u200b/g, '').trim();
                if (rest) return;                       // 上一段还有别的内容:不动
                cur.insertBefore(stray[0], cur.firstChild);
                var rep = document.createElement('p');
                rep.setAttribute('data-block', '0');
                pv.replaceWith(rep);
                try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e6) {}
                try { onInput(vd.getValue()); } catch (e7) {}
                scheduleGutters();
            } catch (e3) {}
        }, 0);
    }, true);

    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Escape') return;
        // 查找栏打开时,Esc 先关它
        if (findbar && findbar.style.display !== 'none') return;
        if (langPickerOpen || (langSuggestEl && !langSuggestEl.hidden)) {
            closeLangPicker(false);
            e.preventDefault();
            e.stopPropagation();
            return;
        }
        var block = currentCodeBlock();
        if (!block || !block.classList.contains('vditor-ir__node--expand')) return;
        if (exitCodeEdit(block)) {
            e.preventDefault();
            e.stopPropagation();
        }
    }, true);

    // 滚动/窗口变化时按钮跟随(轻量:仅在按钮可见时重算)。
    // 行号列在文档流内,不需要跟随滚动 —— 旧代码这里每次滚动都重算,
    // 正是行号"悬空漂移"观感的来源之一
    document.addEventListener('scroll', function () {
        // 只要还在代码块里就重算按钮(不能只在可见时才重算:块滚出屏幕
        // 一次按钮就被藏掉,再滚回来就永远不出现了)
        if (codeLangBlock && codeLangBlock.isConnected)
            syncCodeLangBtn(codeLangBlock);
        if (langSuggestEl && !langSuggestEl.hidden) scheduleLangSuggest();
        if (mathPopEl && !mathPopEl.hidden) updateMathPop();   // 气泡跟随公式
    }, true);
    window.addEventListener('resize', function () {
        if (codeLangBlock && codeLangBlock.isConnected)
            syncCodeLangBtn(codeLangBlock);
        if (langSuggestEl && !langSuggestEl.hidden) scheduleLangSuggest();
        if (mathPopEl && !mathPopEl.hidden) updateMathPop();
        scheduleGutters();
    });

    // ------------------------------------------------------------------
    // 编辑器级快捷键(C++ 未拦截的组合在此处理;Ctrl+S/O 等应用级不会到这)
    // ------------------------------------------------------------------
    var lastFind = { query: '', idx: -1 };

    function ins(text) {
        if (vd) { vd.insertValue(text, true); vd.focus(); }
    }
    // 插入后强制重渲染:Vditor 无 render API,派发 input 事件触发引擎重解析
    // (execCommand/insertValue 插入的 **x**/# 标记停在文本态的根因就是缺这一步)
    function rerender() {
        try {
            var t = editorRoot();
            if (t) t.dispatchEvent(new Event('input', { bubbles: true }));
        } catch (e) {}
    }
    function notice(msg) { post({ t: 'notice', msg: msg }); }

    var contextMathBlock = null;
    function mathBlockAt(node) {
        var el = node && (node.nodeType === 1 ? node : node.parentElement);
        var block = el && el.closest('[data-block="0"][data-type="math-block"]');
        return currentMode === 'ir' && block && editorRoot().contains(block) ? block : null;
    }

    function selectedMathBlock() {
        var sel = window.getSelection();
        if (currentMode !== 'ir' || !sel || !sel.rangeCount) return null;
        var range = sel.getRangeAt(0);
        var block = mathBlockAt(range.startContainer);
        if (block && block === mathBlockAt(range.endContainer)) return block;
        if (range.collapsed) return null;
        var blocks = editorRoot().querySelectorAll('[data-block="0"][data-type="math-block"]');
        for (var i = 0; i < blocks.length; i++) {
            var boundary = document.createRange();
            boundary.selectNode(blocks[i]);
            if (range.intersectsNode(blocks[i]) &&
                range.compareBoundaryPoints(Range.START_TO_START, boundary) >= 0 &&
                range.compareBoundaryPoints(Range.END_TO_END, boundary) <= 0)
                return blocks[i];
        }
        return null;
    }

    // Parse only complete outer formatting groups. Escaped braces and TeX
    // comments must not close a group inside an aligned/cases environment.
    function mathGroupEnd(text, start) {
        var depth = 0;
        for (var i = start; i < text.length; i++) {
            if (text[i] === '\\') { i++; continue; }
            if (text[i] === '%') {
                var newline = text.indexOf('\n', i);
                if (newline < 0) return -1;
                i = newline;
                continue;
            }
            if (text[i] === '{') depth++;
            if (text[i] === '}' && --depth === 0) return i;
        }
        return -1;
    }

    function mathFormat(text) {
        var state = { color: null, bold: false, start: 0, end: text.length };
        while (true) {
            var part = text.slice(state.start, state.end);
            var match = /^\s*\\(textcolor\{(#[0-9a-f]{3}(?:[0-9a-f]{3})?)\}|pmb)\s*\{/i.exec(part);
            if (!match) break;
            var open = match[0].length - 1;
            var close = mathGroupEnd(part, open);
            if (close < 0 || part.slice(close + 1).trim()) break;
            if (match[2]) {
                if (state.color !== null) break;
                state.color = match[2].toLowerCase();
            } else {
                if (state.bold) break;
                state.bold = true;
            }
            var start = open + 1, end = close;
            if (part[start] === '\n' && part[end - 1] === '\n' && end - start >= 2) {
                start++;
                end--;
            }
            state.end = state.start + end;
            state.start += start;
        }
        state.body = text.slice(state.start, state.end);
        return state;
    }

    function formatMathBlock(action, color) {
        var block = contextMathBlock && contextMathBlock.isConnected
            && editorRoot().contains(contextMathBlock) ? contextMathBlock : selectedMathBlock();
        contextMathBlock = null;
        if (!block) {
            var selection = window.getSelection();
            if (currentMode === 'ir' && selection && selection.rangeCount && !selection.isCollapsed &&
                Array.prototype.some.call(editorRoot().querySelectorAll('[data-block="0"][data-type="math-block"]'),
                    function (node) { return selection.getRangeAt(0).intersectsNode(node); })) {
                notice('请单独选择一个公式块设置样式');
                return true;
            }
            return false;
        }
        var source = block.querySelector('pre.vditor-ir__marker--pre > code.language-math');
        if (!source) return true;
        var original = source.textContent;
        var format = mathFormat(original);
        if (!format.body.trim()) { notice('请先输入公式'); return true; }
        if (action === 'color') {
            if (color !== 'clear' && !/^#[0-9a-f]{3}(?:[0-9a-f]{3})?$/i.test(color)) return true;
            var nextColor = color === 'clear' ? null : color.toLowerCase();
            if (format.color === nextColor) return true;
            format.color = nextColor;
        } else {
            format.bold = !format.bold;
        }
        var prefix = '', suffix = '';
        if (format.color) { prefix += '\\textcolor{' + format.color + '}{\n'; suffix = '\n}' + suffix; }
        // boldsymbol does not propagate into array/aligned cells. pmb applies
        // to the whole rendered expression and survives Markdown/HTML export.
        if (format.bold) { prefix += '\\pmb{\n'; suffix = '\n}' + suffix; }
        var text = prefix + format.body + suffix;
        try {
            window.katex.renderToString(text, {
                displayMode: true, macros: vd.vditor.options.preview.math.macros || {}
            });
        } catch (error) {
            notice('公式尚未完整或存在语法错误，请修正后再设置样式');
            return true;
        }
        var expanded = block.classList.contains('vditor-ir__node--expand');
        var sel = window.getSelection();
        var range = sel.rangeCount ? sel.getRangeAt(0) : null;
        var start = format.body.length, end = start;
        if (range && source.contains(range.startContainer) && source.contains(range.endContainer)) {
            var before = range.cloneRange();
            before.selectNodeContents(source);
            before.setEnd(range.startContainer, range.startOffset);
            start = Math.max(0, Math.min(format.body.length, before.toString().length - format.start));
            end = Math.max(start, Math.min(format.body.length, before.toString().length + range.toString().length - format.start));
        }
        clearStaleCaretBookmarks();
        recordMathUndo();
        source.textContent = text;
        range = document.createRange();
        range.setStart(source.firstChild, prefix.length + start);
        range.setEnd(source.firstChild, prefix.length + end);
        sel.removeAllRanges();
        sel.addRange(range);
        // This changes only CODE text, so use the same preview path as native
        // code paste. The input parser skips noncollapsed/line-start ranges.
        var preview = block.querySelector('.vditor-ir__preview');
        if (preview) {
            var rendered = document.createElement('div');
            rendered.className = 'language-math';
            rendered.setAttribute('data-type', 'math-block');
            rendered.textContent = text;
            preview.replaceChildren(rendered);
            preview.setAttribute('data-render', '1');
            Vditor.mathRender(preview, { cdn: vd.vditor.options.cdn, math: vd.vditor.options.preview.math });
        }
        if (!expanded) {
            range = document.createRange();
            range.selectNode(block);
            sel.removeAllRanges();
            sel.addRange(range);
        }
        decorateMathBlocks(true);
        recordMathUndo();
        onInput(vd.getValue());
        editorRoot().focus({ preventScroll: true });
        return true;
    }

    document.addEventListener('mousedown', function () { contextMathBlock = null; }, true);
    document.addEventListener('keydown', function () { contextMathBlock = null; }, true);

    function recordMathUndo() {
        var root = editorRoot(), undo = vd && vd.vditor && vd.vditor.undo;
        if (!root || !root.isConnected || !undo || typeof undo.addToUndoStack !== 'function') return false;
        // Native rendering sets data-render before its async KaTeX callback.
        // Until data-math exists, keep the raw preview in the undo snapshot;
        // otherwise the renderer's completion creates a second, empty undo.
        var pending = Array.prototype.filter.call(root.querySelectorAll(
            '.vditor-ir__preview[data-render="1"]'), function (preview) {
            var math = preview.firstElementChild;
            return math && math.classList.contains('language-math') && !math.hasAttribute('data-math');
        });
        pending.forEach(function (preview) { preview.setAttribute('data-render', '2'); });
        try { undo.addToUndoStack(vd.vditor); return true; }
        finally { pending.forEach(function (preview) { preview.setAttribute('data-render', '1'); }); }
    }

    // Keep LaTeX as text inside CODE. Browser insertText/insertHTML split
    // multiline input into DOM blocks, which Lute can silently truncate.
    function insertMathSource(text) {
        if (!vd || currentMode !== 'ir') return false;
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount) return false;
        var range = sel.getRangeAt(0);
        var el = range.startContainer.nodeType === 1
            ? range.startContainer : range.startContainer.parentElement;
        var source = el && el.closest('pre.vditor-ir__marker--pre > code.language-math');
        if (!source || !editorRoot().contains(source) || !source.contains(range.endContainer))
            return false;
        text = String(text).replace(/\r\n?/g, '\n');
        var remaining = source.textContent;
        var before = range.cloneRange();
        before.selectNodeContents(source);
        before.setEnd(range.startContainer, range.startOffset);
        var start = before.toString().length;
        var end = start + range.toString().length;
        // A complete fenced formula pasted into an empty/selected formula
        // supplies the body, not nested dollar fences.
        if (!(remaining.slice(0, start) + remaining.slice(end)).trim()) {
            var fenced = /^\s*\$\$[ \t]*\n?([\s\S]*?)\n?[ \t]*\$\$\s*$/.exec(text);
            if (fenced) text = fenced[1];
        }
        clearStaleCaretBookmarks();
        vd.vditor.undo.recordFirstPosition(vd.vditor, { key: 'v' });
        recordMathUndo();
        range.deleteContents();
        var node = document.createTextNode(text);
        range.insertNode(node);
        range.setStartAfter(node);
        range.collapse(true);
        sel.removeAllRanges();
        sel.addRange(range);
        rerender();
        clearStaleCaretBookmarks();
        decorateMathBlocks();
        recordMathUndo();
        onInput(vd.getValue());
        return true;
    }

    function insertCodeSource(text) {
        if (!vd || currentMode !== 'ir') return false;
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount) return false;
        var range = sel.getRangeAt(0);
        var block = currentCodeBlock();
        var source = block && block.querySelector('pre.vditor-ir__marker--pre > code');
        if (!source) return false;
        var pre = source.parentElement;
        if (!(source.contains(range.startContainer) || range.startContainer === pre)
            || !(source.contains(range.endContainer) || range.endContainer === pre)) return false;
        var beforeMarkdown = preferLatex ? blockMarkdown(block) : null;
        var before = range.cloneRange();
        before.selectNodeContents(source);
        before.setEnd(range.startContainer, range.startOffset);
        var start = before.toString().length;
        var end = start + range.toString().length;
        text = String(text).replace(/\r\n?/g, '\n');
        suppressUntil = 0;
        clearStaleCaretBookmarks();
        vd.vditor.undo.recordFirstPosition(vd.vditor, { key: 'v' });
        recordMathUndo();
        // Empty code lines can leave the selection on PRE instead of CODE.
        // Insert literal text using the source range, independent of paste.target.
        var node = document.createTextNode(source.textContent.slice(0, start)
            + text + source.textContent.slice(end));
        source.replaceChildren(node);
        range = document.createRange();
        range.setStart(node, start + text.length); range.collapse(true);
        sel.removeAllRanges(); sel.addRange(range);
        rerender();
        if (preferLatex && convertEditedLatexBlock(beforeMarkdown, null, true)) return true;
        recordMathUndo();
        onInput(vd.getValue());
        return true;
    }

    document.addEventListener('beforeinput', function (event) {
        if (event.isComposing || !event.cancelable || !event.data || !/[\r\n]/.test(event.data)) return;
        if (insertMathSource(event.data)) {
            event.preventDefault();
            event.stopPropagation();
        }
    }, true);

    function selectBlockText() {
        var b = caretBlock();
        if (!b) { notice('未选中(光标不在文本块中)'); return; }
        var rng = document.createRange();
        rng.selectNodeContents(b);
        // 整块选中时默认不带标记(# / $$ / ** / <font> 等):这样加粗、上色、
        // 复制都不会把标记带上,也不会在替换时把标记吃掉。手动拖选不受影响
        trimMarkerRange(rng);
        var sel = window.getSelection();
        sel.removeAllRanges();
        sel.addRange(rng);
    }

    // 该文本节点是否"纯标记":IR 的 # / $$ / ** / ``` 等,以及 <font> 颜色标签本身
    function isMarkerNode(n) {
        var raw = n.textContent || '';
        var t = raw.replace(/[\u200b\s]/g, '').replace(/<\/?font[^>]*>/gi, '');
        if (!t)
            return true;                        // 空白 / 零宽 / 颜色标签本身
        var el = n.parentElement;
        if (el && el.closest) {
            // 行内 HTML 标签(Vditor 包成 span.vditor-ir__node)
            if (el.closest('span.vditor-ir__node') && /^<\/?[a-zA-Z][^>]*>$/.test(t.trim()))
                return true;
            if (el.closest('[class*="vditor-ir__marker"]')) {
                if (/^[#*_`~>+\-=|\d.()\[\]$\\\/]+$/.test(t))
                    return true;                // 标记元素内且只有标记字符
                if (/^(`{3,}|~{3,})[A-Za-z0-9#+-]*$/.test(t))
                    return true;                // 围栏:```cpp / ~~~python
            }
        }
        return false;
    }

    // 把选区收缩到"内容"范围:首尾的纯标记不计入(只收缩,不扩张)
    function trimMarkerRange(range) {
        var root = range.commonAncestorContainer;
        if (root && root.nodeType === 3)
            root = root.parentElement;
        if (!root || !root.querySelectorAll)
            return range;
        var nodes = [];
        var walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        var n;
        while ((n = walker.nextNode())) {
            if (!range.intersectsNode(n) || isMarkerNode(n))
                continue;
            nodes.push(n);
        }
        if (!nodes.length)
            return range;
        try {
            var first = nodes[0];
            var probe = document.createRange();
            probe.setStart(first, 0);
            if (range.compareBoundaryPoints(Range.START_TO_START, probe) < 0)
                range.setStart(first, 0);
            var last = nodes[nodes.length - 1];
            probe = document.createRange();
            probe.setStart(last, last.textContent.length);
            if (range.compareBoundaryPoints(Range.END_TO_END, probe) > 0)
                range.setEnd(last, last.textContent.length);
        } catch (e) { /* 选区异常时保持原样 */ }
        return range;
    }

    function selectWord() {
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount) return;
        var r = sel.getRangeAt(0);
        var node = r.startContainer;
        // 光标在元素节点(块首/块尾)时:下潜到首个文本节点
        if (node.nodeType !== 3) {
            var walker = document.createTreeWalker(node, NodeFilter.SHOW_TEXT);
            var tn = walker.nextNode();
            if (tn) {
                var tr = document.createRange();
                tr.setStart(tn, 0);
                tr.collapse(true);
                sel.removeAllRanges();
                sel.addRange(tr);
                node = tn;
                r = tr;
            }
        }
        // 文本节点内:按词字符(字母/数字/中文)双向扫描
        if (node.nodeType === 3) {
            var text = node.textContent;
            var off = r.startOffset;
            var W = /[A-Za-z0-9一-鿿]/;
            // 跳过零宽字符/标记等非词字符,定位最近的词首
            while (off < text.length && !W.test(text[off])) off++;
            var st = off;
            while (st > 0 && W.test(text[st - 1])) st--;
            var en = off;
            while (en < text.length && W.test(text[en])) en++;
            if (en > st) {
                var nr = document.createRange();
                nr.setStart(node, st);
                nr.setEnd(node, en);
                sel.removeAllRanges();
                sel.addRange(nr);
                return;
            }
        }
        // 非文本/空位:回退到浏览器原生 wordboundary
        try {
            sel.modify('move', 'backward', 'wordboundary');
            sel.modify('extend', 'forward', 'wordboundary');
        } catch (e) { notice('无法选词'); }
    }

    function deleteWord() {
        selectWord();
        try { document.execCommand('delete'); } catch (e) {}
    }

    function jumpToSelection() {
        var sel = window.getSelection();
        if (!sel.rangeCount) return;
        var n = sel.getRangeAt(0).startContainer;
        var el = n.nodeType === 3 ? n.parentElement : n;
        if (el && el.scrollIntoView) el.scrollIntoView({ block: 'center' });
        notice('已跳转到所选内容');
    }

    function copyAsMarkdown() {
        var txt = window.getSelection().toString();
        if (!txt) { notice('没有选中的内容'); return; }
        navigator.clipboard.writeText(txt)
            .then(function () { notice('已复制为 Markdown'); })
            .catch(function () { notice('复制失败'); });
    }

    // 光标所在词(无选区时的 Ctrl+B/I 目标;含 CJK)
    function wordAtCaret() {
        var sel = window.getSelection();
        if (!sel.rangeCount) return null;
        var r = sel.getRangeAt(0);
        var node = r.startContainer;
        if (node.nodeType !== 3) return null;
        var text = node.textContent;
        var off = r.startOffset;
        var W = /[A-Za-z0-9一-鿿]/;
        // 光标若停在空格/标点上,向左回收进词内(词尾场景)
        if (off > 0 && !W.test(text[off]) ) { off--; }
        var st = off;
        while (st > 0 && W.test(text[st - 1])) st--;
        var en = off;
        while (en < text.length && W.test(text[en])) en++;
        if (en <= st) return null;
        return { node: node, start: st, end: en, text: text.slice(st, en) };
    }

    function wrapInline(marker) {
        if (marker === '**' && formatMathBlock('bold')) return;
        var sel = window.getSelection();
        if (!sel.rangeCount) return;
        var r = sel.getRangeAt(0);
        var txt = selectionTextClean(r);
        if (!txt) {
            // 无选区:整段加粗(仅普通段落;标题/代码等提示)
            var b = caretBlock();
            if (!b) { notice('光标不在文本中'); return; }
            if (b.tagName !== 'P') { notice('请先用鼠标选中要加粗的范围'); return; }
            txt = (b.textContent || '').replace(/\u200b/g, '').trim();
            if (!txt) {
                // 空块:插入空标记,光标居中
                vd.insertValue(marker + marker, true);
                try { for (var i = 0; i < marker.length; i++) sel.modify('move', 'backward', 'character'); } catch (e) {}
                vd.focus();
                return;
            }
            var wr = document.createRange();
            wr.selectNodeContents(b);
            sel.removeAllRanges();
            sel.addRange(wr);
            r = wr;
        }
        // 关键:execCommand 的 insertText 会【替换】选区(insertValue 只插入不替换,
        // 曾导致 **北京**北京 双份),且原生触发 input 让 IR 立即重渲染
        var ok = false;
        try { ok = document.execCommand('insertText', false, marker + txt + marker); } catch (e) {}
        if (!ok) {
            try {
                r.deleteContents();
                r.collapse(true);
                sel.removeAllRanges();
                sel.addRange(r);
                vd.insertValue(marker + txt + marker, true);
            } catch (e2) {}
        }
        rerender();
        vd.focus();
    }

    // Ctrl+Shift+K 代码块:走 Lute 官方结构(可编辑/语言可改/高亮生效),
    // 插入后光标直接落进代码区可打字;选中文字时把选中文本带进代码块。
    // 旧实现 ins('```cpp') 是纯文本插入:围栏停在文本态、光标落在块外、
    // 语言写死 cpp —— "要按两次、只能 cpp"的怪体验全是它造成的
    // Ctrl+Shift+K:空行原地变成代码框；有文字时保留原段，在段后插入。
    // 旧实现拿当前块文本去 getValue() 里搜,空行/重复段落/匹配失败一律
    // insertAfter = 文末 —— 这就是"总在最后创建"的根因。
    // Insert Lute's native code DOM beside the current block. Serializing the
    // whole document through setValue drops empty paragraphs between blocks.
    function insertCodeBlock() {
        if (!vd) return;
        var sel = window.getSelection();
        var selTxt = (sel && sel.rangeCount ? sel.getRangeAt(0).toString() : '').replace(/\u200b/g, '');
        // 本文档默认语言:新建空块自动带上(选中文本带入时也套),仍可手动改
        var autoLang = (!selTxt && docLang) ? docLang : '';
        var fence = '\n```' + autoLang + '\n' + (selTxt || '') + '\n```\n';

        if (currentMode === 'sv') {
            var ta = document.querySelector('.vditor-sv textarea');
            var v = ta ? ta.value : vd.getValue();
            var start = ta ? ta.selectionStart : v.length;
            var end = ta ? ta.selectionEnd : v.length;
            var picked = (ta ? v.slice(start, end) : selTxt) || '';
            // 文档默认语言同样作用于源码模式(此前只有 IR 分支带上,行为不一致)
            var svLang = (!picked && docLang) ? docLang : '';
            try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e) {}
            vd.setValue(v.slice(0, start) + '\n```' + svLang + '\n' + picked + '\n```\n' + v.slice(end), false);
            return;
        }

        var root = editorRoot();
        var block = caretBlock();
        if (!block && root && root.children.length)
            block = root.children[root.children.length - 1];

        var lute = vd.vditor && vd.vditor.lute;
        if (!root || !lute || !sel || !sel.rangeCount || codeInputComposing
            || !rangeWithin(root, sel.getRangeAt(0))) return;
        var template = document.createElement('template');
        template.innerHTML = lute.Md2VditorIRDOM(fence);
        var inserted = template.content.querySelector('div[data-type="code-block"]');
        if (!inserted) return;
        vd.vditor.undo.recordFirstPosition(vd.vditor, { key: 'k' });
        if (!recordCodeUndo()) return;
        // 空段落上按 Ctrl+Shift+K:空段直接【变成】代码块。旧逻辑插到空段
        // 后面,空段留在块上方 —— 就是"代码块前面多了空行"(空段还会被
        // Lute 塞零宽空格,看着更像一行)
        if (block && block.getAttribute && block.getAttribute('data-type') !== 'code-block'
            && !(block.textContent || '').replace(/\u200b/g, '').trim()
            && !block.matches('[contenteditable="false"],[data-ms-ui]')
            && !block.querySelector('img,video,audio,iframe,embed,object,input,button,select,textarea,svg,canvas,'
                + '[contenteditable="false"],[data-ms-ui]')
            && !/(UL|OL|BLOCKQUOTE|TABLE|HR)/.test(block.tagName)) {
            // 只替换当前空段，不补间隔段，也不删除用户已有的其他空行。
            block.replaceWith(inserted);
        } else if (block) {
            // 上一个兄弟是代码块时补一个空段:否则两块直接相邻,
            // "代码块下面再做一个代码块,中间的空行没了"
            var prevCb = block.getAttribute && block.getAttribute('data-type') === 'code-block'
                ? block : null;
            if (prevCb) {
                var gapP = document.createElement('p');
                gapP.setAttribute('data-block', '0');
                prevCb.after(gapP);
                gapP.after(inserted);
            } else {
                block.after(inserted);
            }
        } else {
            root.appendChild(inserted);
        }
        var newBlockIdx = Array.prototype.indexOf.call(root.querySelectorAll('div[data-type="code-block"]'), inserted);
        var source = inserted.querySelector('pre.vditor-ir__marker--pre > code');
        var range = document.createRange(); range.selectNodeContents(source); range.collapse(true);
        sel.removeAllRanges(); sel.addRange(range);
        suppressUntil = 0;
        rerender();
        recordMathUndo();
        scheduleGutters();
        decorateAllCodeBlocks();
        var focusNew = function () {
            try {
                var root2 = editorRoot();
                var cbs = root2.querySelectorAll('div[data-type="code-block"]');
                var idx = Math.min(newBlockIdx, Math.max(0, cbs.length - 1));
                var cb = cbs[idx];
                if (!cb) return;
                window._msCbEdit = cb;
                cb.classList.add('vditor-ir__node--expand');
                cb.classList.add('ms-cb-edit');
                if (selTxt || autoLang) {
                    var code = cb.querySelector('pre.vditor-ir__marker--pre code') || cb.querySelector('pre code');
                    if (code) {
                        var rng = document.createRange();
                        rng.selectNodeContents(code);
                        rng.collapse(false);
                        var s3 = window.getSelection();
                        s3.removeAllRanges();
                        s3.addRange(rng);
                    }
                    vd.focus();
                    syncCbEditState();
                } else {
                    // Typora:新建空代码块先选语言(输入 p 出 Python/PHP/Perl + Logo)
                    openLangPicker(cb);
                }
            } catch (e) {}
        };
        focusNew();
    }

    // 段落内容恰好是 ``` / ```lang 时按回车:由我们直接把它变成真代码块。
    // Vditor IR 的输入态围栏转换不稳定,一旦没转成,getValue 会把反引号
    // 转义落盘(\\`\\`\\`),围栏从此永久失效 —— 保存后整块退化成一段文字,
    // 就是"代码块变成一行"的根源。这里用 setValue 走 Lute 官方结构,转换
    // 确定性发生;先压撤销栈,转错了能 Ctrl+Z 回来
    function convertFenceParagraph(block) {
        // 剥零宽与转义反斜杠:段落里可能是刚敲的 ```,也可能是历史上
        // 被 Lute 转义落盘的 \` 形态(后者按回车即修复成真代码块)
        var raw = (block.textContent || '').replace(/\u200b/g, '').replace(/\\([`~])/g, '$1');
        var m = /^\s*(`{3,}|~{3,})([\w+#.-]*)\s*$/.exec(raw);
        if (!m) return false;
        var lang = m[2] || '';
        var fenceCh = m[1].charAt(0);
        // 敲的是裸 ``` 而文档设了默认语言 → 转出来的块自动带默认语言
        var typedFence = fenceCh + fenceCh + fenceCh + lang;
        var effectiveLang = lang || (docLang || '');
        var fence = fenceCh + fenceCh + fenceCh + effectiveLang;
        var v = vd.getValue();
        var lines = v.split('\n');
        var replaced = false;
        var newBlockIdx = 0;
        var fencesBefore = 0;
        for (var i = 0; i < lines.length; i++) {
            // 剥零宽与转义反斜杠后比对:getValue 里围栏可能被 Lute 转成 \` 形态
            var ln = lines[i].replace(/\u200b/g, '').replace(/\\([`~])/g, '$1');
            if (ln.trim() === typedFence && fencesBefore % 2 === 0) {
                // 只认"开启位"的围栏:前面代码块的闭合 ``` 与用户敲的裸 ```
                // 文本相同,首字符匹配会命中别人的闭合围栏并改坏那个块
                lines[i] = fence;
                lines.splice(i + 1, 0, '', fenceCh + fenceCh + fenceCh);
                newBlockIdx = Math.floor(fencesBefore / 2);
                replaced = true;
                break;
            }
            if (/^\s*(```|~~~)/.test(lines[i]))
                fencesBefore++;
        }
        if (!replaced) return false;
        try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (e) {}
        vd.setValue(lines.join('\n'), false); // 用户操作,走 onInput 标脏
        scheduleGutters();
        decorateAllCodeBlocks();
        var focusConverted = function () {
            try {
                var root = editorRoot();
                var cbs = root.querySelectorAll('div[data-type="code-block"]');
                var cb = cbs[Math.min(newBlockIdx, cbs.length - 1)];
                if (!cb) return;
                window._msCbEdit = cb;
                cb.classList.add('vditor-ir__node--expand');
                cb.classList.add('ms-cb-edit');
                if (effectiveLang) {
                    var code = cb.querySelector('pre.vditor-ir__marker--pre code') || cb.querySelector('pre code');
                    if (!code) return;
                    var rng = document.createRange();
                    rng.selectNodeContents(code);
                    rng.collapse(true);
                    var s = window.getSelection();
                    s.removeAllRanges();
                    s.addRange(rng);
                    vd.focus();
                    syncCbEditState();
                } else {
                    openLangPicker(cb);
                }
            } catch (e) {}
        };
        setTimeout(focusConverted, 60);
        setTimeout(focusConverted, 180);
        return true;
    }

    // insertMD parses complete fences. insertText splits them into paragraphs;
    // insertValue expects HTML, not Markdown. Focus once, before further input.
    function insertMathBlock() {
        if (!vd) return;
        var root = editorRoot();
        var sel = window.getSelection();
        if (!root || !sel || !sel.rangeCount || !root.contains(sel.anchorNode))
            return;
        var text = selectionTextClean(sel.getRangeAt(0)).trim();
        var paragraph = currentMode === 'ir' ? caretBlock() : null;
        if (!paragraph || paragraph.tagName !== 'P') paragraph = null;
        var selector = '[data-block="0"][data-type="math-block"]';
        var previous = Array.prototype.slice.call(root.querySelectorAll(selector));
        clearStaleCaretBookmarks();
        vd.vditor.undo.recordFirstPosition(vd.vditor, { key: 'm' });
        recordMathUndo();
        vd.insertMD('\n$$\n' + text + '\n$$\n');
        clearStaleCaretBookmarks();
        // Native insertion leaves its original paragraph behind. Consume only
        // an empty paragraph, including one emptied by wrapping selected text.
        if (paragraph && paragraph.isConnected &&
            !paragraph.textContent.replace(/\u200b/g, '').trim() &&
            !paragraph.querySelector('img,svg,video,audio,iframe,input,[data-type]'))
            paragraph.remove();
        if (currentMode === 'sv') {
            vd.vditor.undo.addToUndoStack(vd.vditor);
            onInput(vd.getValue());
            return;
        }
        var nodes = root.querySelectorAll(selector);
        for (var i = 0; i < nodes.length; i++) {
            var node = nodes[i];
            if (previous.indexOf(node) !== -1) continue;
            node.classList.add('vditor-ir__node--expand');
            var source = node.querySelector('pre.vditor-ir__marker--pre code')
                || node.querySelector('code');
            if (source) {
                var range = document.createRange();
                range.selectNodeContents(source);
                range.collapse(!text);
                sel.removeAllRanges();
                sel.addRange(range);
                root.focus({ preventScroll: true });
            }
            break;
        }
        decorateAllCodeBlocks();
        recordMathUndo();
        onInput(vd.getValue());
    }

    // 行内公式(Typora 语义):$…$ 嵌在文字里,不单独占行。
    // Ctrl+Shift+E:有选区包 $…$;无选区插 $|$ 光标居中。
    function insertMathInline() {
        if (!vd) return;
        var sel = window.getSelection();
        var selTxt = (sel && sel.rangeCount ? sel.getRangeAt(0).toString() : '').replace(/\u200b/g, '');
        if (selTxt) {
            var ok = false;
            try { ok = document.execCommand('insertText', false, '$' + selTxt + '$'); } catch (e) {}
            if (!ok) {
                try { vd.insertValue('$' + selTxt + '$', true); } catch (e2) {}
            }
            rerender();
            vd.focus();
            return;
        }
        // 无选区:插 $ + NBSP + $(光标在 NBSP 前,继续输入即成行内公式)。
        // 占位必须用 NBSP:ZWSP 会被 Lute 剥掉,空对折回 $$ 又被转成块级
        try { document.execCommand('insertText', false, '$' + String.fromCharCode(160) + '$'); } catch (e) {}
        try {
            var s2 = window.getSelection();
            if (s2.rangeCount)
                s2.modify('move', 'backward', 'character');   // 光标移进两个 $ 之间
        } catch (e3) {}
        rerender();
        vd.focus();
    }

    function clearFormat() {
        try {
            document.execCommand('removeFormat');
            notice('已清除格式(部分格式需手动删除标记)');
        } catch (e) {}
    }

    // Ctrl+U 下划线:markdown 里没有原生语法,与 Typora 一致用 <u>…</u> 落盘;
    // 显示层走行内 HTML 渲染管线(隐藏标签 + 画下划线)。再次按下=取消
    function toggleUnderline() {
        var sel = window.getSelection();
        if (!sel || !sel.rangeCount) return;
        var b = caretBlock();
        if (!b) { notice('光标不在文本块中'); return; }
        var r = sel.getRangeAt(0);

        // 已在下划线里:取消(找到覆盖选区的 <u> 对,删标记+拆 span)
        var s = r.startContainer, e = r.endContainer;
        var tags = b.querySelectorAll('span.vditor-ir__node');
        for (var i = 0; i < tags.length; i++) {
            if (!/^<u>$/i.test((tags[i].textContent || '').trim()))
                continue;
            for (var j = i + 1; j < tags.length; j++) {
                if (tags[j].parentNode !== tags[i].parentNode)
                    break;
                if (/^<\/u>$/i.test((tags[j].textContent || '').trim())) {
                    if (nodeBefore(tags[i], s) && nodeBefore(e, tags[j])) {
                        // 命中:删这对标签,拆下划线 span
                        var uSpans = tags[i].parentNode.querySelectorAll('span[data-ms-underline]');
                        for (var k = 0; k < uSpans.length; k++) {
                            var us = uSpans[k];
                            if (nodeBefore(tags[i], us) && nodeBefore(us, tags[j])) {
                                while (us.firstChild) us.parentNode.insertBefore(us.firstChild, us);
                                us.remove();
                            }
                        }
                        tags[i].remove();
                        tags[j].remove();
                        rerender();
                        setTimeout(renderColorTags, 100);
                        if (vd) vd.focus();
                        return;
                    }
                    break;
                }
            }
        }

        // 添加:选区文本包进 <u>…</u>(与 applyColor 同一套写法)
        var txt = selectionTextClean(r).replace(/<\/?u>/gi, '');
        if (!txt) {
            if (b.tagName !== 'P') { notice('请先选中要加下划线的文字'); return; }
            txt = (b.textContent || '').replace(/\u200b/g, '').replace(/<\/?u>/gi, '').trim();
            if (!txt) return;
            var wr = document.createRange();
            wr.selectNodeContents(b);
            sel.removeAllRanges();
            sel.addRange(wr);
            r = wr;
        }
        var marked = '<u>' + txt + '</u>';
        var ok = false;
        try { ok = document.execCommand('insertText', false, marked); } catch (eu) {}
        if (!ok) {
            try {
                r.deleteContents();
                r.collapse(true);
                sel.removeAllRanges();
                sel.addRange(r);
                vd.insertValue(marked, true);
            } catch (eu2) {}
        }
        rerender();
        setTimeout(renderColorTags, 150);
        if (vd) vd.focus();
    }

    // ------------------------------------------------------------------
    // 文字颜色
    // 文件里存 <font color="#xxx">…</font>(可移植、重开不丢)。但 Vditor 的 IR
    // 内核不渲染行内 HTML(实测 sanitize 开关都一样,Lute 只当纯文本),所以显示
    // 层由我们自己补:隐藏标签文本 + 给中间内容上色。
    // 只隐藏、不删除 —— getValue() 仍能读到标签文本,落盘/重开颜色都不会丢。
    // ------------------------------------------------------------------
    var MS_COLOR_SPAN = /<span[^>]*data-ms-color[^>]*>([\s\S]*?)<\/span>/gi;
    var MS_UNDERLINE_SPAN = /<span[^>]*data-ms-underline[^>]*>([\s\S]*?)<\/span>/gi;

    // a 在文档序上是否早于 b
    function nodeBefore(a, b) {
        try { return !!(a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING); }
        catch (e) { return false; }
    }

    // 选区是否落在该节点内(落在里面就先别动,免得打乱光标)
    function touchesSelection(node) {
        try {
            var sel = window.getSelection();
            if (!sel || !sel.rangeCount) return false;
            return sel.getRangeAt(0).intersectsNode(node);
        } catch (e) { return false; }
    }

    // 把正文里字面量的行内 HTML 标签渲染成真实效果:
    //   <font color="#xxx">…</font> -> 内容上色
    //   <u>…</u>                    -> 内容加下划线(Ctrl+U,与 Typora 落盘格式一致)
    // 共用一套"隐藏标签文本 + 包效果 span"的管线;只隐藏不删除,
    // getValue() 仍读得到标签,落盘/重开不丢
    var INLINE_TAGS = [
        { open: /^<font\s+color=["']?([#0-9a-zA-Z(),.\s]+?)["']?\s*>$/i, close: /^<\/font>$/i,
          attr: 'data-ms-color', style: function (m) { return 'color:' + m[1]; } },
        { open: /^<u>$/i, close: /^<\/u>$/i,
          attr: 'data-ms-underline', style: function () { return 'text-decoration:underline'; } },
        // markdown 链接:Vditor IR 实测不把 [文字](链接) 渲染成 <a>(一直
        // 显示字面量),这里自己渲染:隐藏 [ / ](url) 标记 span,中间文字
        // 变成可点击的 <a>。href 取自闭合标记 "](url)" 的 url 部分
        { open: /^\[[^\]]*\]\(.+\)$/, close: null,
          attr: 'data-ms-link', style: function () { return 'color:#4285f4;cursor:pointer;'; },
          isLink: true }
    ];

    // 进入被装饰的块时还原原始结构:装饰出来的东西(display:none 标记 +
    // 效果包裹 span)与 Vditor 的 Enter/Backspace 编辑处理冲突,实测会把
    // 段落拆成垃圾结构(冒出 DIV 子块、内容断裂) —— 颜色/下划线/链接
    // 各种"抽风"的总根源。光标在哪个块,哪个块就保持裸结构
    function undecorateBlock(block) {
        if (!block) return;
        // 快检:块里没有任何装饰就不动(也避免无谓地重置选区 ——
        // 每次 selectionchange 都 removeAllRanges/addRange 会干扰
        // Vditor 的撤销快照,实测图片粘贴后的撤销被搞挂)
        if (!block.querySelector('[data-ms-tag="1"], span[data-ms-color], span[data-ms-underline], span[data-ms-href]'))
            return;
        // 光标在本块内时,按【文本偏移】记住位置:还原装饰会移动节点,
        // 浏览器会把光标重映射到段尾(实测 offset 跳到内容后),行首/行尾
        // 判定与 Enter/删除处理全被带偏。文本偏移在装饰/还原前后不变
        var savedTextOff = null;
        try {
            var selS = window.getSelection();
            if (selS && selS.rangeCount && selS.isCollapsed) {
                var rr = selS.getRangeAt(0);
                if (block.contains(rr.startContainer)) {
                    var twS = document.createTreeWalker(block, NodeFilter.SHOW_TEXT);
                    var offS = 0, nnS;
                    while ((nnS = twS.nextNode())) {
                        if (nnS === rr.startContainer) { offS += rr.startOffset; break; }
                        offS += nnS.textContent.length;
                    }
                    savedTextOff = offS;
                }
            }
        } catch (eS0) {}
        try {
            // 链接渲染的 [ 隐标记 / <span 蓝字> / ](url) 隐标记 -> 单节点原文
            var links = [...block.querySelectorAll('span[data-ms-href]')];
            for (var li = 0; li < links.length; li++) {
                var link = links[li];
                var pre = link.previousSibling, post = link.nextSibling;
                var full = '[' + (link.textContent || '') + '](' + (link.getAttribute('data-ms-href') || '') + ')';
                var node = document.createElement('span');
                node.className = 'vditor-ir__node';
                node.textContent = full;
                if (pre && pre.getAttribute && pre.getAttribute('data-ms-tag') === '1') pre.remove();
                if (post && post.getAttribute && post.getAttribute('data-ms-tag') === '1') post.remove();
                link.parentNode.replaceChild(node, link);
            }
            // 颜色/下划线效果包裹 span -> 展开回平级
            var wraps = [...block.querySelectorAll('span[data-ms-color], span[data-ms-underline]')];
            for (var wi = 0; wi < wraps.length; wi++) {
                var w = wraps[wi];
                var parent = w.parentNode;
                while (w.firstChild) parent.insertBefore(w.firstChild, w);
                parent.removeChild(w);
                if (parent.nodeType === 1 && !parent.childNodes.length)
                    parent.appendChild(document.createTextNode('\u200b'));
            }
            // 恢复隐藏标记的显示
            var marks = [...block.querySelectorAll('[data-ms-tag="1"]')];
            for (var mi = 0; mi < marks.length; mi++) {
                marks[mi].removeAttribute('data-ms-tag');
                marks[mi].style.display = '';
            }
        } catch (e) {}
        if (savedTextOff !== null) {
            try {
                var twR = document.createTreeWalker(block, NodeFilter.SHOW_TEXT);
                var accR = 0, nodeR = null, offR = 0, lastR = null, lastAccR = 0;
                while ((nodeR = twR.nextNode())) {
                    // 严格 >:偏移恰好落在某节点末尾(如标记文本结束处)时,
                    // 要落到【下一个】文本节点开头,而不是停在标记里
                    if (accR + nodeR.textContent.length > savedTextOff) {
                        offR = savedTextOff - accR;
                        break;
                    }
                    lastR = nodeR; lastAccR = accR;
                    accR += nodeR.textContent.length;
                }
                if (!nodeR && lastR) {   // 光标在块末:停在最后一个节点结尾
                    nodeR = lastR; offR = nodeR.textContent.length;
                }
                if (nodeR) {
                    var nrR = document.createRange();
                    nrR.setStart(nodeR, Math.min(offR, nodeR.textContent.length));
                    nrR.collapse(true);
                    var nsR = window.getSelection();
                    nsR.removeAllRanges();
                    nsR.addRange(nrR);
                }
            } catch (eR0) {}
        }
    }

    function renderColorTags() {
        var root = editorRoot();
        if (!root || !root.querySelectorAll) return;
        // 光标所在的块:先还原裸结构,本帧也不再装饰
        var caretBlk = null;
        try { caretBlk = caretBlock(); } catch (e0) {}
        if (caretBlk) undecorateBlock(caretBlk);
        var tags = root.querySelectorAll('span.vditor-ir__node');
        for (var i = 0; i < tags.length; i++) {
            var open = tags[i];
            if (open.getAttribute('data-ms-tag'))
                continue;                                   // 已处理
            var openText = (open.textContent || '').trim();
            var rule = null, m = null;
            for (var r = 0; r < INLINE_TAGS.length; r++) {
                m = INLINE_TAGS[r].open.exec(openText);
                if (m) { rule = INLINE_TAGS[r]; break; }
            }
            if (!rule)
                continue;
            // markdown 链接特殊:Lute 把整个 [文字](链接) 包在【单个】
            // vditor-ir__node 里(不是三个标记 span)。走专用路径:拆开重建
            if (rule.isLink) {
                renderLinkNode(open);
                continue;
            }
            // 找配对的闭合标签(同一父节点下的后续兄弟)
            var close = null;
            for (var j = i + 1; j < tags.length; j++) {
                if (tags[j].parentNode !== open.parentNode)
                    break;
                if (rule.close.test((tags[j].textContent || '').trim())) { close = tags[j]; break; }
            }
            if (!close || touchesSelection(open) || touchesSelection(close)
                || (caretBlk && open.parentNode && caretBlk.contains(open)))
                continue;   // 光标所在块不装饰(裸结构才安全)
            // 内容包一层效果 span(颜色/下划线靠它);链接规则用 <a> 承载,
            // 点击可在浏览器打开。链接规则的 m[1] 是 href
            var wrap = document.createElement(rule.isLink ? 'a' : 'span');
            if (rule.isLink) {
                wrap.setAttribute('href', m[1]);
                wrap.setAttribute('target', '_blank');
            }
            wrap.setAttribute(rule.attr, '1');
            wrap.setAttribute('style', rule.style(m));
            open.parentNode.insertBefore(wrap, open.nextSibling);
            var sib = wrap.nextSibling;
            while (sib && sib !== close) {
                var next = sib.nextSibling;
                wrap.appendChild(sib);
                sib = next;
            }
            // 链接规则:从闭合标记 "](url)" 提取 href 设置到 <a>
            if (rule.isLink) {
                var um = rule.close.exec((close.textContent || '').trim());
                if (um && um[1])
                    wrap.setAttribute('href', um[1]);
            }
            // 隐藏标签本身(保留文本:markdown 往返要用)
            open.setAttribute('data-ms-tag', '1');
            open.style.display = 'none';
            close.setAttribute('data-ms-tag', '1');
            close.style.display = 'none';
        }
    }

    // 把单个 span.vditor-ir__node 里的 [文字](链接) 重建为:
    //   [隐藏标记] <a 文字> [隐藏标记]
    // 只隐藏标记文本,markdown 往返不丢;重开后同样生效
    function renderLinkNode(open) {
        var full = (open.textContent || '').trim();
        var lm = /^\[([^\]]*)\]\(([^\s)]+)\)$/.exec(full);
        var linkBlk = null;
        try { linkBlk = caretBlock(); } catch (eL) {}
        if (!lm || touchesSelection(open) || (linkBlk && linkBlk.contains(open)))
            return;   // 光标所在块不装饰(裸结构才安全)
        var text = lm[1], href = lm[2];
        // 三个节点:前标记(隐) / 链接样式 span(承载 data-ms-href) / 后标记(隐)
        // 注意不能用 <a>:Lute 的 DOM→md 会把 <a> 转回 [x](y),与行首的 "[" 
        // 标记文本拼成嵌套链接;未知 span 则被 Lute 丢弃、只留文字
        var mk = document.createDocumentFragment();
        var openSpan = document.createElement('span');
        openSpan.textContent = '[';
        openSpan.setAttribute('data-ms-tag', '1');
        openSpan.style.display = 'none';
        var link = document.createElement('span');
        link.setAttribute('data-ms-href', href);
        link.setAttribute('style', 'color:#4285f4;cursor:pointer;');
        link.textContent = text;
        var closeSpan = document.createElement('span');
        closeSpan.textContent = '](' + href + ')';
        closeSpan.setAttribute('data-ms-tag', '1');
        closeSpan.style.display = 'none';
        mk.appendChild(openSpan);
        mk.appendChild(link);
        mk.appendChild(closeSpan);
        open.parentNode.replaceChild(mk, open);
    }

    // 上色/下划线 span 万一被 Lute 带进 markdown,落盘/导出前剥掉(只剥自己生成的)
    function stripColorSpans(text) {
        try {
            return String(text)
                .replace(MS_COLOR_SPAN, '$1')
                .replace(MS_UNDERLINE_SPAN, '$1');
        } catch (e) { return text; }
    }

    // 设标题:光标放块首,输入 '#'*n + 空格(IR 按打字方式转换,无递归)
    // ------------------------------------------------------------------
    // 标题标记显示开关(editor.html 按类 ms-show-marker 隐藏/显示 # 与徽标)。
    // 规则:只有"鼠标点中的那个标题"显示标记;Ctrl+1~6、键盘移动光标、
    // Vditor 的 --expand 都不触发显示。内容重渲染后类会丢,DOM 变更时补挂。
    // ------------------------------------------------------------------
    var shownHeading = null;    // 当前显示标记的标题元素

    function headingMarkerShown(el) {
        return !!(el && el.classList && el.classList.contains('ms-show-marker'));
    }

    function showHeadingMarker(el) {
        if (shownHeading === el) return;
        hideHeadingMarker();
        if (!el || !/^H[1-6]$/.test(el.tagName)) return;
        el.classList.add('ms-show-marker');
        shownHeading = el;
    }

    function hideHeadingMarker() {
        if (shownHeading && shownHeading.classList) {
            shownHeading.classList.remove('ms-show-marker');
        }
        shownHeading = null;
    }

    // 恢复显示状态:重渲染(IR 常态)会把 class 冲掉,记得重新挂上
    function restoreHeadingMarker() {
        if (!shownHeading) return;
        var root = editorRoot();
        if (!root || !shownHeading.isConnected || !root.contains(shownHeading)) {
            shownHeading = null;
            return;
        }
        shownHeading.classList.add('ms-show-marker');
    }

    // 点中哪个标题就显示哪个标题的标记(捕获阶段,先于 Vditor 处理)
    document.addEventListener('mousedown', function (e) {
        var el = e.target;
        while (el && el !== document.body) {
            if (/^H[1-6]$/.test(el.tagName)) {
                var root = editorRoot();
                if (!root || root.contains(el)) { showHeadingMarker(el); return; }
            }
            el = el.parentElement;
        }
        hideHeadingMarker();   // 点在标题外:收起标记
    }, true);

    // Lute 重渲染会替换/清掉 class:DOM 变更后把显示状态补回去
    var headingObserver = new MutationObserver(function (muts) {
        var onlyUi = muts.length > 0;
        for (var mi = 0; mi < muts.length; mi++) {
            var m = muts[mi];
            if (m.type !== 'childList') { onlyUi = false; break; }
            var pack = [];
            var a = m.addedNodes, r = m.removedNodes, pj;
            for (pj = 0; pj < a.length; pj++) pack.push(a[pj]);
            for (pj = 0; pj < r.length; pj++) pack.push(r[pj]);
            for (pj = 0; pj < pack.length; pj++) {
                var n = pack[pj];
                if (n.nodeType === 1 && n.getAttribute && n.getAttribute('data-ms-ui') === 'code')
                    continue;
                onlyUi = false;
            }
        }
        if (onlyUi) return;
        clearStaleCaretBookmarks();
        restoreCodeBlockBlankLine(muts);
        decorateMathBlocks(true);
        restoreHeadingMarker();
        scheduleCbEditSync();   // 代码块被重渲染换元素后,补挂编辑态 class(合并到帧)
        scheduleGutters();      // 块被换掉后行号列也随之失效,统一重排
    });
    // 两个代码块之间只剩一个空行时,Lute 把空行当纯分隔符吃掉(两个块直接
    // 相邻,"中间预设的空行没了");隔两行才有空段。在【段落变代码块】的
    // 精确时刻(同一 mutation 里移除 P + 新增 code-block)把空段还回去 ——
    // 只挂新增时机:用户随后删掉空行(纯移除)不会触发,不会打架
    function restoreCodeBlockBlankLine(muts) {
        var root = editorRoot();
        if (!root) return;
        for (var i = 0; i < muts.length; i++) {
            var m = muts[i];
            if (m.type !== 'childList') continue;
            var addedBlock = null, removedPara = false;
            for (var a = 0; a < m.addedNodes.length; a++) {
                var n = m.addedNodes[a];
                if (n.nodeType === 1 && n.getAttribute
                    && n.getAttribute('data-type') === 'code-block')
                    addedBlock = n;
            }
            if (!addedBlock || !addedBlock.parentNode) continue;
            for (var r = 0; r < m.removedNodes.length; r++) {
                var d = m.removedNodes[r];
                if (d.nodeType === 1 && d.tagName === 'P') removedPara = true;
            }
            if (!removedPara) continue;
            var prev = addedBlock.previousElementSibling;
            var next = addedBlock.nextElementSibling;
            var gap = document.createElement('p');
            gap.setAttribute('data-block', '0');
            if (prev && prev.getAttribute && prev.getAttribute('data-type') === 'code-block')
                addedBlock.parentNode.insertBefore(gap, addedBlock);
            else if (next && next.getAttribute && next.getAttribute('data-type') === 'code-block')
                addedBlock.parentNode.insertBefore(gap, next);
        }
    }
    // --expand 的盯守从全局 observer 拆出:全局 observer 只看 childList,
    // 结构上杜绝"我们写 class → observer → 重排 → 再写 class"的自环
    // (公式块点击卡死的根因)。expand 变更由这个专职观察器带 80ms 去抖处理,
    // 只重排布局,不写任何 class
    var expandObserver = new MutationObserver(function (muts) {
        for (var i = 0; i < muts.length; i++) {
            var m = muts[i];
            if (m.type !== 'attributes' || m.attributeName !== 'class') continue;
            var el = m.target;
            if (!el.classList || !el.getAttribute) continue;
            var dt = el.getAttribute('data-type');
            if (dt !== 'code-block' && dt !== 'math-block') continue;
            // 只关心 --expand 的进出;旧值里有没有它由 className 比对太脆,
            // 直接去抖重排即可 —— 布局重排是幂等的
            expandObserver._dirty = true;
            if (!expandObserver._timer) {
                expandObserver._timer = setTimeout(function () {
                    expandObserver._timer = 0;
                    if (!expandObserver._dirty) return;
                    expandObserver._dirty = false;
                    scheduleGutters();
                }, 80);
            }
        }
    });
    function startHeadingObserver() {
        var root = editorRoot();
        if (root && !headingObserver._on) {
            headingObserver._on = true;
            headingObserver.observe(root.parentElement || root, {
                childList: true, subtree: true
            });
            expandObserver.observe(root.parentElement || root, {
                attributes: true, attributeFilter: ['class'], subtree: true
            });
        } else if (!root) {
            setTimeout(startHeadingObserver, 300);
        }
    }
    startHeadingObserver();

    // Change the block tag while moving its children, preserving text-node
    // identity and both selection endpoints, including inline math and styles.
    function headingLevel(n) {
        var b = caretBlock();
        if (!b) { notice('光标不在文本块中'); return; }
        // 只对普通段落/标题生效:代码块、列表、表格里插 "#" 会把标记
        // 写进正文,破坏原块(Typora 里这些块上 Ctrl+1 本就无效)
        if (!/^(P|H[1-6])$/.test(b.tagName)) { notice('当前块不能设为标题'); return; }
        if (currentMode !== 'ir') return;
        var selection = getSelection();
        if (!selection.rangeCount) return;
        var range = selection.getRangeAt(0).cloneRange();
        if (range.collapsed && range.startContainer === editorRoot()) {
            var beginsHere = editorRoot().childNodes[range.startOffset] === b;
            range.selectNodeContents(b); range.collapse(beginsHere);
            selection.removeAllRanges(); selection.addRange(range);
        }
        if (!b.contains(range.startContainer) || !b.contains(range.endContainer)) return;
        // 段内换行的多行块(软换行,一个文本节点里带 \n):只把【光标所在行】
        // 转成目标块,行前/行后各自成段 —— 用户的预期是"标题只是那一行",
        // 不是把整段吞进标题(实测:标题+正文软换行同段时 Ctrl+1 全段变标题)
        if (b.textContent && b.textContent.indexOf('\n') >= 0
            && !b.querySelector('span.vditor-ir__node, img, [data-type]')) {
            var txtAll = b.textContent;
            var offAll = 0;
            var twL = document.createTreeWalker(b, NodeFilter.SHOW_TEXT);
            var nnL;
            while ((nnL = twL.nextNode())) {
                if (nnL === range.startContainer) { offAll += range.startOffset; break; }
                offAll += nnL.textContent.length;
            }
            var lineStart = txtAll.lastIndexOf('\n', Math.max(0, offAll - 1)) + 1;
            var lineEnd = txtAll.indexOf('\n', offAll);
            if (lineEnd < 0) lineEnd = txtAll.length;
            if (lineStart > 0 || lineEnd < txtAll.length) {
                hideHeadingMarker();
                clearStaleCaretBookmarks();
                try { vd.vditor.undo.addToUndoStack(vd.vditor); } catch (eL) {}
                var lineBlk = document.createElement(n > 0 ? 'h' + n : 'p');
                lineBlk.setAttribute('data-block', '0');
                if (n > 0) {
                    lineBlk.setAttribute('data-marker', '#');
                    lineBlk.classList.add('vditor-ir__node');
                    var msp = document.createElement('span');
                    msp.className = 'vditor-ir__marker vditor-ir__marker--heading';
                    msp.setAttribute('data-type', 'heading-marker');
                    msp.textContent = '#'.repeat(n) + ' ';
                    lineBlk.appendChild(msp);
                }
                var lineNode = document.createTextNode(txtAll.slice(lineStart, lineEnd));
                lineBlk.appendChild(lineNode);
                var fragL = document.createDocumentFragment();
                if (lineStart > 0) {
                    var bpL = document.createElement('p');
                    bpL.setAttribute('data-block', '0');
                    bpL.appendChild(document.createTextNode(txtAll.slice(0, lineStart - 1)));
                    fragL.appendChild(bpL);
                }
                fragL.appendChild(lineBlk);
                if (lineEnd < txtAll.length) {
                    var apL = document.createElement('p');
                    apL.setAttribute('data-block', '0');
                    apL.appendChild(document.createTextNode(txtAll.slice(lineEnd + 1)));
                    fragL.appendChild(apL);
                }
                b.replaceWith(fragL);
                var nrL = document.createRange();
                nrL.setStart(lineNode, Math.max(0, Math.min(offAll - lineStart, lineNode.textContent.length)));
                nrL.collapse(true);
                selection.removeAllRanges();
                selection.addRange(nrL);
                suppressUntil = 0;
                onInput(vd.getValue());
                editorRoot().focus({ preventScroll: true });
                scheduleGutters();
                return;
            }
            // 光标行就是整块:走下面的整块转换
        }
        function bookmark() {
            var anchor = document.createRange(), focus = document.createRange();
            anchor.setStart(selection.anchorNode, selection.anchorOffset); anchor.collapse(true);
            focus.setStart(selection.focusNode, selection.focusOffset); focus.collapse(true);
            return { anchor:anchor, focus:focus };
        }
        function restore(saved) {
            selection.setBaseAndExtent(saved.anchor.startContainer, saved.anchor.startOffset,
                saved.focus.startContainer, saved.focus.startOffset);
        }
        // Undo inserts a caret marker, splitting text nodes and resetting the
        // selection direction. Live endpoint ranges track those splits.
        var saved = bookmark();
        hideHeadingMarker();
        clearStaleCaretBookmarks();
        recordMathUndo();
        var anchorNode = saved.anchor.startContainer, anchorOffset = saved.anchor.startOffset;
        var focusNode = saved.focus.startContainer, focusOffset = saved.focus.startOffset;
        var heading = document.createElement(n > 0 ? 'h' + n : 'p'), marks = '#'.repeat(n);
        Array.prototype.forEach.call(b.attributes, function (attribute) {
            heading.setAttribute(attribute.name, attribute.value);
        });
        heading.setAttribute('data-block', '0');
        if (n > 0) heading.setAttribute('data-marker', '#');
        heading.classList.add('vditor-ir__node');
        var oldMarker = b.querySelector('span[data-type="heading-marker"]');
        // n=0 = 转回普通段落:无标记
        var marker = null;
        if (n > 0) {
            marker = document.createElement('span');
            marker.className = 'vditor-ir__marker vditor-ir__marker--heading';
            marker.setAttribute('data-type', 'heading-marker'); marker.textContent = marks + ' ';
            heading.appendChild(marker);
        }
        function childOffset(offset) {
            return (n > 0 ? 1 : 0) + Array.prototype.slice.call(b.childNodes, 0, offset).filter(function (child) {
                return child !== oldMarker;
            }).length;
        }
        var mappedAnchor = anchorNode === b ? childOffset(anchorOffset) : anchorOffset;
        var mappedFocus = focusNode === b ? childOffset(focusOffset) : focusOffset;
        while (b.firstChild) {
            if (b.firstChild === oldMarker) oldMarker.remove();
            else heading.appendChild(b.firstChild);
        }
        b.replaceWith(heading);
        if (anchorNode === b) {
            anchorNode = heading; anchorOffset = mappedAnchor;
        }
        if (focusNode === b) {
            focusNode = heading; focusOffset = mappedFocus;
        }
        if (!anchorNode.isConnected) { anchorNode = heading; anchorOffset = 1; }
        if (!focusNode.isConnected) { focusNode = heading; focusOffset = 1; }
        if ((n > 0 ? heading.childNodes.length === 1 : heading.childNodes.length === 0) && range.collapsed) {
            // A hidden marker alone is not a browser caret target. Match the
            // existing editor's zero-width anchors so typing stays in the block.
            var emptyAnchor = document.createTextNode('\u200b'); heading.appendChild(emptyAnchor);
            anchorNode = focusNode = emptyAnchor; anchorOffset = focusOffset = 1;
        }
        selection.setBaseAndExtent(anchorNode, anchorOffset, focusNode, focusOffset);
        suppressUntil = 0;
        saved = bookmark(); recordMathUndo(); restore(saved);
        onInput(vd.getValue());
        editorRoot().focus({ preventScroll:true });
    }
    function headingStep(delta) { // delta<0 升级(向 H1),>0 降级
        var b = caretBlock();
        var m = b ? /^H([1-6])$/.exec(b.tagName) : null;
        if (!m) {
            if (delta < 0) headingLevel(1);
            else notice('当前不是标题');
            return;
        }
        headingLevel(Math.min(6, Math.max(1, parseInt(m[1], 10) + delta)));
    }

    function docFind(dir) {
        // F3:查找栏未开则先开;开着就前后跳
        if (!findbar || findbar.style.display === 'none') {
            var selObj = window.getSelection();
            openFindbar(selObj ? selObj.toString() : '');
            return;
        }
        findbarJump(dir);
    }

    // ------------------------------------------------------------------
    // 顶部查找栏(Ctrl+F):Typora 式 —— 输入即定位、Enter/Shift+Enter 前后跳、
    // Esc 关闭。命中直接在文中"滚动+选中+高亮",当前命中深黄、其余淡黄。
    // 整条链在页面内闭环,C++ 只转发 Ctrl+F。
    // ------------------------------------------------------------------
    var findbar = null;
    var findbarInput = null;
    var findbarInfo = null;
    var findHits = [];          // [{node, start, len, mark}]
    var findIdx = -1;

    function buildFindbar() {
        if (findbar) return;
        findbar = document.createElement('div');
        findbar.id = 'ms-findbar';
        findbar.style.cssText =
            'position:fixed;top:0;right:24px;z-index:9999;display:none;' +
            'align-items:center;gap:6px;padding:6px 10px;' +
            'background:var(--panel-background-color,#fff);' +
            'border:1px solid var(--border-color,#d1d5da);border-top:none;' +
            'border-radius:0 0 8px 8px;box-shadow:0 3px 10px rgba(0,0,0,.14);' +
            'font-size:13px;';
        findbar.innerHTML =
            '<input id="ms-findbar-input" placeholder="查找…" ' +
            ' style="width:220px;border:1px solid var(--border-color,#d1d5da);' +
            ' border-radius:4px;padding:4px 8px;font-size:13px;outline:none;' +
            ' background:var(--textarea-background-color,#fff);' +
            ' color:var(--textarea-text-color,#24292e);">' +
            '<span id="ms-findbar-info" style="min-width:86px;text-align:center;' +
            ' color:var(--second-color,#586069);">0/0</span>' +
            '<button id="ms-findbar-prev" title="上一个(Shift+Enter)" ' +
            ' style="border:1px solid var(--border-color,#d1d5da);background:transparent;' +
            ' border-radius:4px;padding:3px 9px;cursor:pointer;font-size:13px;">↑</button>' +
            '<button id="ms-findbar-next" title="下一个(Enter)" ' +
            ' style="border:1px solid var(--border-color,#d1d5da);background:transparent;' +
            ' border-radius:4px;padding:3px 9px;cursor:pointer;font-size:13px;">↓</button>' +
            '<button id="ms-findbar-close" title="关闭(Esc)" ' +
            ' style="border:none;background:transparent;cursor:pointer;' +
            ' font-size:15px;color:var(--second-color,#586069);">✕</button>';
        document.body.appendChild(findbar);
        findbarInput = findbar.querySelector('#ms-findbar-input');
        findbarInfo = findbar.querySelector('#ms-findbar-info');

        var debounce = null;
        findbarInput.addEventListener('input', function () {
            clearTimeout(debounce);
            debounce = setTimeout(function () { runFind(); }, 180);
        });
        findbarInput.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') {
                e.preventDefault();
                findbarJump(e.shiftKey ? -1 : 1);
            } else if (e.key === 'Escape') {
                e.preventDefault();
                closeFindbar();
            }
            e.stopPropagation();   // 别把按键漏给编辑器
        });
        findbar.querySelector('#ms-findbar-next').addEventListener('click', function () { findbarJump(1); });
        findbar.querySelector('#ms-findbar-prev').addEventListener('click', function () { findbarJump(-1); });
        findbar.querySelector('#ms-findbar-close').addEventListener('click', closeFindbar);
    }

    function runFind() {
        var q = findbarInput.value;
        clearFindMarks();
        findHits = [];
        findIdx = -1;
        if (!q) { findbarInfo.textContent = '0/0'; return; }
        var root = editorRoot();
        if (!root) return;
        var lower = q.toLowerCase();
        // 边包边找:surroundContents 会把命中文本移进新 span,同一文本节点上
        // 后续命中的偏移全变 —— 包一个就在(新)文本流上重找下一个,引用永远有效
        // 两遍法:第一遍只收集(walker 一次线性扫完,不改动 DOM);
        // 第二遍从后往前包 span —— 从后往前插入不影响前面的偏移,
        // 且每个命中包完后其 node 引用不再被后续操作移动
        var walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
        var node = walker.nextNode();
        while (node && findHits.length < 500) {
            // 空内容才跳过。此前用 isMarkerNode 整节点过滤,IR 里 # 标记与
            // 标题文字常在同一个文本节点,"搜标题文字搜不到"就是它造成的
            var t = node.textContent || '';
            if (t.replace(/[\u200b\s]/g, '')) {
                var tl = t.toLowerCase();   // 只转一次,别在循环里反复转
                var at = tl.indexOf(lower);
                while (at >= 0 && findHits.length < 500) {
                    findHits.push({ node: node, start: at, len: lower.length, mark: null });
                    at = tl.indexOf(lower, at + lower.length);
                }
            }
            node = walker.nextNode();
        }
        for (var hi = findHits.length - 1; hi >= 0; hi--)
            markHit(findHits[hi], false);   // 从后往前:前面的偏移不被扰动
        findbarInfo.textContent = findHits.length ? '1/' + findHits.length : '无结果';
        if (findHits.length)
            findbarJump(0);
    }

    function markHit(hit, current) {
        try {
            var r = document.createRange();
            r.setStart(hit.node, hit.start);
            r.setEnd(hit.node, hit.start + hit.len);
            var sp = document.createElement('span');
            sp.setAttribute('data-ms-find', current ? 'cur' : 'hit');
            sp.setAttribute('style', current
                ? 'background:#f5c623;border-radius:2px;'
                : 'background:rgba(245,198,35,.32);border-radius:2px;');
            r.surroundContents(sp);
            hit.node = sp.firstChild;   // 包完后文本节点换新,更新引用
            hit.mark = sp;
        } catch (e) {}
    }

    function clearFindMarks() {
        var root = editorRoot();
        if (!root) return;
        var marks = root.querySelectorAll('span[data-ms-find]');
        for (var i = 0; i < marks.length; i++) {
            var sp = marks[i];
            if (!sp.parentNode) continue;
            while (sp.firstChild) sp.parentNode.insertBefore(sp.firstChild, sp);
            sp.remove();
        }
    }

    function findbarJump(dir) {
        if (!findHits.length) return;
        findIdx = dir === 0 ? 0
                 : ((findIdx + dir) % findHits.length + findHits.length) % findHits.length;
        var hit = findHits[findIdx];
        // 当前命中深黄 + 属性 "cur"(其余淡黄 + "hit")
        for (var i = 0; i < findHits.length; i++) {
            if (!findHits[i].mark || !findHits[i].mark.parentNode) continue;
            findHits[i].mark.setAttribute('data-ms-find', i === findIdx ? 'cur' : 'hit');
            findHits[i].mark.setAttribute('style', i === findIdx
                ? 'background:#f5c623;border-radius:2px;'
                : 'background:rgba(245,198,35,.32);border-radius:2px;');
        }
        try {
            var el = hit.mark || (hit.node.nodeType === 3 ? hit.node.parentElement : hit.node);
            if (el && el.scrollIntoView)
                el.scrollIntoView({ block: 'center', behavior: 'smooth' });
        } catch (e) {}
        findbarInfo.textContent = (findIdx + 1) + '/' + findHits.length;
    }

    function openFindbar(prefill) {
        buildFindbar();
        if (typeof prefill === 'string' && prefill)
            findbarInput.value = prefill.slice(0, 120);
        findbar.style.display = 'flex';
        findbarInput.focus();
        findbarInput.select();
        if (findbarInput.value)
            runFind();
    }

    function closeFindbar() {
        if (!findbar) return;
        findbar.style.display = 'none';
        clearFindMarks();
        findHits = [];
        findIdx = -1;
        if (vd) vd.focus();
    }

    document.addEventListener('keydown', function (e) {
        var k = e.key, c = e.code; // code=物理键位:Shift 不改变(符号键必须用 code 判断)
        var mod = e.ctrlKey || e.metaKey;
        // 代码块里禁用全部正文格式化/插入键:它们会把 **、> 、表格线等
        // 标记写进代码正文;而且不 preventDefault 的话 Chromium 原生
        // Ctrl+B/I 还会给代码套 <b>/<i>。Typora 里这些键在代码块本就无效
        if (currentCodeBlock()) {
            var blockedInCode =
                (mod && !e.altKey && !e.shiftKey
                    && (c === 'KeyB' || c === 'KeyI' || c === 'KeyU' || c === 'KeyT'
                        || c === 'KeyK')) ||
                // Ctrl+Shift+K 一并拦截(Typora 语义:代码块内无效)。此前放行
                // 会在当前块后面新建空块并弹语言候选 —— 用户看到的就是"按了
                // 先空一行、然后弹个关不掉的语言框"
                (mod && !e.altKey && e.shiftKey
                    && (c === 'KeyM' || c === 'KeyQ' || c === 'KeyK'
                        || c === 'BracketLeft' || c === 'BracketRight'
                        || c === 'Backquote')) ||
                (mod && e.altKey && !e.shiftKey && (c === 'Equal' || c === 'Minus')) ||
                (e.altKey && e.shiftKey && c === 'Digit5');
            if (blockedInCode) { e.preventDefault(); return; }
        }
        if (mod && !e.altKey) {
            if (e.shiftKey) {
                if (k === 'K' || k === 'k') {
                    var codeRoot = editorRoot();
                    if (!codeRoot || !codeRoot.contains(e.target) || e.defaultPrevented
                        || codeInputComposing || e.isComposing || e.keyCode === 229) return;
                    e.preventDefault(); e.stopImmediatePropagation(); insertCodeBlock(); return;
                }
                if (k === 'M' || k === 'm') { e.preventDefault(); insertMathBlock(); return; }
                if (k === 'E' || k === 'e') { e.preventDefault(); insertMathInline(); return; }
                if (k === 'Q' || k === 'q') { e.preventDefault(); ins('\n> '); return; }
                if (c === 'BracketLeft') { e.preventDefault(); ins('\n1. '); return; }
                if (c === 'BracketRight') { e.preventDefault(); ins('\n- '); return; }
                if (k === 'C' || k === 'c') { e.preventDefault(); copyAsMarkdown(); return; }
                if (k === 'I' || k === 'i') { e.preventDefault(); post({ t: 'pickImage' }); return; }
                if (k === 'D' || k === 'd') { e.preventDefault(); deleteWord(); return; }
                if (c === 'Backquote') { e.preventDefault(); wrapInline('`'); return; }
            } else {
                if (c === 'KeyB') { e.preventDefault(); wrapInline('**'); return; }
                if (c === 'KeyI') { e.preventDefault(); wrapInline('*'); return; }
                if (c === 'KeyK') {
                    // 代码块内的 Ctrl+K 已在上方统一拦截
                    e.preventDefault();
                    var selK = window.getSelection();
                    post({ t: 'linkDialog',
                           sel: (selK ? selK.toString() : '').slice(0, 200) });
                    return;
                }
                if (c === 'KeyU') { e.preventDefault(); toggleUnderline(); return; }
                if (c === 'KeyT') { e.preventDefault(); ins('\n| 表头1 | 表头2 |\n| --- | --- |\n| 内容 | 内容 |\n'); return; }
                if (c === 'Backslash') { e.preventDefault(); clearFormat(); return; }
                if (c === 'KeyL') { e.preventDefault(); selectBlockText(); return; }
                if (c === 'KeyE') { e.preventDefault(); selectBlockText(); return; }
                if (c === 'KeyD') { e.preventDefault(); selectWord(); return; }
                if (c === 'KeyJ') { e.preventDefault(); jumpToSelection(); return; }
                if (c === 'KeyH') {
                    e.preventDefault();
                    var selNow = window.getSelection();
                    post({ t: 'replace', sel: (selNow ? selNow.toString() : '').slice(0, 80) });
                    return;
                }
                if (c === 'Digit0') {
                    // Typora 语义:标题里 = 转普通段落;否则转发 C++ 重置整页缩放
                    var b0 = caretBlock();
                    if (b0 && /^H[1-6]$/.test(b0.tagName)) { e.preventDefault(); headingLevel(0); return; }
                    e.preventDefault();
                    post({ t: 'zoomReset' });
                    return;
                }
                var hd = /^Digit([1-6])$/.exec(c);
                if (hd) { e.preventDefault(); headingLevel(parseInt(hd[1], 10)); return; }
            }
        }
        // 标题升降级:Ctrl+Shift+↑/↓。Ctrl+Alt+=/- 已被 C++ 收走做字号,
        // 原先绑在那组键上等于死键
        if (mod && e.shiftKey && !e.altKey && (c === 'ArrowUp' || c === 'ArrowDown')) {
            e.preventDefault();
            headingStep(c === 'ArrowUp' ? -1 : 1);
            return;
        }
        if (e.altKey && e.shiftKey && c === 'Digit5') { e.preventDefault(); wrapInline('~~'); return; }
        if (k === 'F3') { e.preventDefault(); docFind(e.shiftKey ? -1 : 1); return; }
        if (k === 'F9') {
            // 仅裸 F9 打开 AI 面板:带 Ctrl/Alt/Shift 的 F9 属于输入法或其它
            // 软件的习惯键位,不能误触面板(用户反馈"没按 AI 它自己开了")
            if (e.ctrlKey || e.altKey || e.shiftKey) return;
            e.preventDefault(); post({ t: 'aiPanel' }); return;
        }
        if (k === 'F11') { e.preventDefault(); post({ t: 'fullscreen' }); return; }
        if (k === 'F12' && e.shiftKey) { e.preventDefault(); post({ t: 'devtools' }); return; }
    }, true);

    // Tab 键接管:Chromium 对可编辑区的 Tab 默认行为是"跳到下一个可聚焦
    // 元素",焦点虽然留在编辑器,但 selectionchange 链会把滚动容器带跑
    // (用户看到的就是"页面自己往上/乱翻"),而且什么都不插入。
    // Vditor 的 IR 模式实测不处理 Tab(放行=丢键),所以全部自己接管:
    //   Tab      -> 插入 4 空格
    //   Shift+Tab-> 行首有缩进就删一层(4 或 2 空格),没有就收掉
    //   源码模式 -> 放行(textarea 原生行为,不抢)
    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Tab' || e.ctrlKey || e.altKey || e.metaKey)
            return;
        if (!vd || currentMode === 'sv')
            return;
        var root = editorRoot();
        if (!root || !root.contains(e.target))
            return;   // 光标不在编辑正文里(比如外部输入框)
        e.preventDefault();
        var NB = String.fromCharCode(160);
        if (!e.shiftKey) {
            // 缩进字符选型(实测):普通空格在 IR 里会被 Lute 折半/吞掉(代码块
            // 只剩 2 个、段落全吞),真实 	 也被吞;4 个 NBSP 在段落和代码块里
            // 都完整存活,渲染时视觉宽度等同 4 空格。列表项里 insertValue 同样
            // 被吞,改从行首直接插入
            var block = caretBlock();
            var inList = false;
            var el = block;
            while (el && el !== root) {
                if (/^(UL|OL)$/.test(el.tagName)) { inList = true; break; }
                el = el.parentElement;
            }
            if (inList) {
                try {
                    var selL = window.getSelection();
                    var nodeL = selL.getRangeAt(0).startContainer;
                    if (nodeL.nodeType !== 3) { vd.insertValue(NB.repeat(2), true); return; }
                    var tL = nodeL.textContent;
                    var offL = selL.getRangeAt(0).startOffset;
                    var lsL = tL.lastIndexOf(String.fromCharCode(10), offL - 1) + 1;
                    nodeL.textContent = tL.slice(0, lsL) + NB.repeat(2) + tL.slice(lsL);
                    var rL = document.createRange();
                    rL.setStart(nodeL, offL + 2);
                    rL.collapse(true);
                    selL.removeAllRanges();
                    selL.addRange(rL);
                    rerender();
                    return;
                } catch (eL) { /* 退到通用路径 */ }
            }
            try { vd.insertValue(NB.repeat(4), true); } catch (e2) {}
            return;
        }
        // Shift+Tab:光标所在行行首删一层缩进(NBSP 或普通空格,4 优先 2 其次)
        try {
            var sel = window.getSelection();
            if (!sel || !sel.rangeCount) return;
            var node = sel.getRangeAt(0).startContainer;
            if (node.nodeType !== 3) return;   // 只处理文本节点内的光标
            var text = node.textContent;
            var off = sel.getRangeAt(0).startOffset;
            var NB = String.fromCharCode(160);
            var IND = '(?:' + NB + '{1,4}| {1,4})';
            // 行首 = 光标前最近换行之后;找从行首开始的最长缩进段
            var lineStart = text.lastIndexOf(String.fromCharCode(10), off - 1) + 1;
            var seg = text.slice(lineStart, off);
            var m = new RegExp('^' + IND).exec(seg);
            if (!m || !m[0]) return;
            var n = m[0].length;
            var cut = n >= 4 ? 4 : (n >= 2 ? 2 : n);
            // 从行首删 cut 个(而不是从光标回数 —— 光标可能不在缩进末尾)
            var rr = document.createRange();
            rr.setStart(node, lineStart);
            rr.setEnd(node, lineStart + cut);
            sel.removeAllRanges();
            sel.addRange(rr);
            var okDel = false;
            try { okDel = document.execCommand('delete'); } catch (e4) {}
            if (!okDel) {
                // 回退:直接改文本节点(execCommand 在部分场景静默失败)
                node.textContent = text.slice(0, lineStart) + text.slice(lineStart + cut);
                var r2 = document.createRange();
                r2.setStart(node, Math.max(0, off - cut));
                r2.collapse(true);
                sel.removeAllRanges();
                sel.addRange(r2);
                rerender();
            }
        } catch (e3) { /* 光标结构异常时静默,至少已阻止焦点跳转 */ }
    }, true);

    // 双击/三击选词选段:Chromium 的默认选择会把 IR 行内标记(**、``、
    // <font> 标签、行首 # 等)一起圈进来。等浏览器默认选择完成,再把选区
    // 首尾收缩到"内容"上 —— 复用 Ctrl+L 整块选中的 trimMarkerRange。
    // 标记插在选区【中间】的保留不动:浏览器选区是连续区间,跳过中间标记
    // 需要多段选区,Chromium 下不可靠;而双击词/三击段的标记几乎总在两端,
    // 首尾收缩已覆盖。e.detail>=2:第 2 击(选词)与第 3 击(选段)都生效,
    // 单击不受影响。
    document.addEventListener('click', function (e) {
        if (e.detail < 2) return;
        if (currentMode === 'sv' || !vd)
            return;
        var root = editorRoot();
        if (!root || !root.contains(e.target))
            return;
        setTimeout(function () {
            try {
                var sel = window.getSelection();
                if (!sel || !sel.rangeCount) return;
                var r = sel.getRangeAt(0);
                if (r.collapsed) return;
                trimMarkerRange(r);
                if (!r.toString()) return;    // 收缩后为空:宁可保留原选区
                sel.removeAllRanges();
                sel.addRange(r);
            } catch (e2) {}
        }, 0);
    }, true);

    // 空行修复:IR 模式空段落上按 Enter 默认无动作。
    // 直接调用 Vditor 官方 API insertEmptyBlock(工具栏"段后插入"同款,
    // 内部会插入带 ZWSP+wbr 的空段并走完整渲染流程)
    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Enter' || e.isComposing || e.ctrlKey || e.altKey || e.metaKey || e.shiftKey)
            return;
        if (currentMode !== 'ir' || !vd)
            return;
        // 踩钢丝微推:只在换行时判断,跳变与新行引发的自然滚动合为一体
        tailNudgeOnEnter();
        var block = caretBlock();
        // 代码块内回车:①自动缩进(语言感知的通用规则)②新行可能落在
        // 视口边缘,浏览器不补滚(再打一个字才跳);而且光标常停在
        // "元素级边界"位置 —— 算不出矩形,浏览器会把视口直接滚到 0。
        // 两步兜底:光标规范化进文本节点 + 确保块底可见
        if (block && block.getAttribute && block.getAttribute('data-type') === 'code-block') {
            var autoIndentCode = function () {
                try {
                    var b = currentCodeBlock();
                    if (!b) return;
                    var codeEl = activeCodeEl(b);
                    var s = window.getSelection();
                    if (!codeEl || !s || !s.rangeCount) return;
                    var r = s.getRangeAt(0);
                    var node = r.startContainer, off = r.startOffset;
                    if (node.nodeType !== 3) {
                        var walker = document.createTreeWalker(codeEl, NodeFilter.SHOW_TEXT);
                        var last = null, tn;
                        while ((tn = walker.nextNode())) last = tn;
                        if (!last) return;
                        node = last;
                        off = last.textContent.length;
                    }
                    var text = node.textContent;
                    var NB = String.fromCharCode(160);
                    var lineStart = text.lastIndexOf('\n', off - 1) + 1;
                    var prevLineStart = text.lastIndexOf('\n', lineStart - 2) + 1;
                    var prevLine = text.slice(prevLineStart, Math.max(prevLineStart, lineStart - 1));
                    var mi = /^[ \t\u00a0]*/.exec(prevLine);
                    var baseIndent = mi ? mi[0] : '';
                    var trimmed = prevLine.replace(/[ \t\u00a0]+$/, '');
                    var extra = /[:{(\[]$/.test(trimmed) ? 4 : 0;
                    var indent = baseIndent.replace(/ /g, NB) + (extra ? NB.repeat(4) : '');
                    if (!indent) return;
                    // 幂等守卫(0ms/120ms 双趟重试的时序差曾把 def 回车变成 8 格):
                    // ① 光标前还有非空白 = Vditor 还没把换行落定,这趟不插,留给下一趟
                    var beforeCaret = text.slice(lineStart, off);
                    if (/[^ \t\u00a0]/.test(beforeCaret)) return;
                    // ② 补差量语义:光标前已有的纯空白先抵掉目标,只插缺的部分
                    //    (整段重插会叠加,第二趟见 4 格再插 4 格 = 8 格事故)
                    var done = beforeCaret.replace(/ /g, NB);
                    if (done.length >= indent.length) return;
                    indent = indent.slice(done.length);
                    var nr = document.createRange();
                    nr.setStart(node, off);
                    nr.collapse(true);
                    s.removeAllRanges();
                    s.addRange(nr);
                    var ok = false;
                    try { ok = document.execCommand('insertText', false, indent); } catch (e2) {}
                    if (!ok) {
                        node.textContent = text.slice(0, off) + indent + text.slice(off);
                        var r2 = document.createRange();
                        r2.setStart(node, off + indent.length);
                        r2.collapse(true);
                        s.removeAllRanges();
                        s.addRange(r2);
                        rerender();
                    }
                } catch (e) {}
            };
            var assistCodeScroll = function () {
                try {
                    var b = currentCodeBlock();
                    if (!b) return;
                    var codeEl = activeCodeEl(b);
                    var s = window.getSelection();
                    if (codeEl && s && s.rangeCount) {
                        var r0 = s.getRangeAt(0);
                        var c = r0.startContainer;
                        if (c === codeEl || c === codeEl.parentElement) {
                            // 元素级边界:移进最后一个文本节点的末尾
                            var walker = document.createTreeWalker(codeEl, NodeFilter.SHOW_TEXT);
                            var last = null, tn;
                            while ((tn = walker.nextNode())) last = tn;
                            if (last) {
                                var nr = document.createRange();
                                nr.setStart(last, last.textContent.length);
                                nr.collapse(true);
                                s.removeAllRanges();
                                s.addRange(nr);
                            }
                        }
                    }
                    var margin = 24, vh = window.innerHeight;
                    var caretRect = null;
                    if (s && s.rangeCount)
                        caretRect = s.getRangeAt(0).getBoundingClientRect();
                    if (caretRect && caretRect.bottom > vh - margin)
                        window.scrollBy(0, Math.ceil(caretRect.bottom - (vh - margin)) + 4);
                } catch (e) {}
            };
            setTimeout(function () { autoIndentCode(); assistCodeScroll(); }, 0);
            setTimeout(function () { autoIndentCode(); assistCodeScroll(); }, 120);
            return;   // 其余交给 Vditor 原生行为
        }
        // 空块回车只对普通段落有意义:标题/列表里的空块上
        // 插 <p> 会破坏原结构,交给 Vditor 原生行为
        if (block && block.tagName !== 'P')
            return;
        // 段落内容恰好是 ``` / ```lang(含被转义的 \` 形态):回车直接转真代码块。
        // 必须抢在 Vditor 前面:它转不成时会把反引号转义落盘,围栏永久失效
        if (block) {
            var fenceRaw = (block.textContent || '').replace(/\u200b/g, '').replace(/\\([`~])/g, '$1');
            if (/^\s*(`{3,}|~{3,})[\w+#.-]*\s*$/.test(fenceRaw)) {
                e.preventDefault();
                e.stopPropagation();
                convertFenceParagraph(block);
                return;
            }
        }
        var text = block ? block.textContent : '';
        var onBlankLine = false;
        if (text.replace(/\u200b/g, '').trim()) {
            // 非空块:判断光标是否停在段内末尾的空行
            // (打字后首次回车产生软换行,光标在其后:前字符是换行且后面无内容)
            try {
                var selE = window.getSelection();
                if (selE.rangeCount && selE.isCollapsed) {
                    var rE = selE.getRangeAt(0);
                    var preE = document.createRange();
                    preE.selectNodeContents(block);
                    preE.setEnd(rE.startContainer, rE.startOffset);
                    var beforeE = preE.toString();
                    var postE = document.createRange();
                    postE.selectNodeContents(block);
                    postE.setStart(rE.startContainer, rE.startOffset);
                    var afterE = postE.toString();
                    if (/[\n\u00a0]\s*$/.test(beforeE) && /^\s*$/.test(afterE))
                        onBlankLine = true;
                }
            } catch (eE) {}
            if (!onBlankLine)
                return; // 普通位置:交给 Vditor 默认行为
        }
        e.preventDefault();
        e.stopPropagation();
        try {
            // Keep an actual editable empty paragraph. Browser insertParagraph
            // followed by SpinVditorIRDOM merges it into the preceding code block.
            if (block) {
                var anchor = document.createRange();
                anchor.selectNodeContents(block); anchor.collapse(false);
                var selection = getSelection(); selection.removeAllRanges(); selection.addRange(anchor);
                vd.insertEmptyBlock('afterend');
            } else {
                document.execCommand('insertParagraph');
            }
        } catch (err) { /* 失败则回退默认 */ }
    }, true);

    // ------------------------------------------------------------------
    // 文档图片加载:
    //   C:\...\xxx.png(图片池,Typora 风格绝对路径)
    //     -> https://<imgpoolN>.local/xxx(C++ 已把各池目录映射为虚拟主机)
    //   ./assets/xx(旧文档相对路径)-> https://<docHost>/assets/xx
    // ------------------------------------------------------------------
    var imgDirs = [];  // [{host, prefix}] 由 C++ setImgDirs 下发
    var savedImageHosts = [], savedImageFiles = {}, savedImageHostSignature = '';

    function rewriteImg(img) {
        var src = img.getAttribute('src') || '';
        if (/^[A-Za-z]:[\\/]/.test(src)) {
            var norm = String(src).replace(/\\/g, '/');
            for (var i = 0; i < imgDirs.length; i++) {
                var p = imgDirs[i].prefix;
                if (p && norm.toLowerCase().indexOf(p.toLowerCase()) === 0) {
                    var rest = norm.slice(p.length).replace(/^\/+/, '');
                    img.setAttribute('src', 'https://' + imgDirs[i].host + '/' + rest);
                    return;
                }
            }
            return; // 不在任何池中:不改写
        }
        if (!docHost) return;
        for (var hi = 0; hi < savedImageHosts.length; hi++) {
            var oldPrefix = 'https://' + savedImageHosts[hi] + '/';
            if (src.indexOf(oldPrefix) === 0 && savedImageFiles[src.slice(oldPrefix.length)]) {
                var nextSrc = 'https://' + docHost + '/' + src.slice(oldPrefix.length);
                if (nextSrc !== src) img.setAttribute('src', nextSrc);
                return;
            }
        }
        var m = /^(\.\/|\/)?(assets\/[^?#]+)$/.exec(src);
        if (m)
            img.setAttribute('src', 'https://' + docHost + '/' + m[2]);
    }

    function rewriteAllImgs(root) {
        var scope = (root && root.querySelectorAll) ? root : document;
        var imgs = scope.querySelectorAll('img');
        for (var i = 0; i < imgs.length; i++) rewriteImg(imgs[i]);
    }

    var imgObserver = new MutationObserver(function (muts) {
        for (var i = 0; i < muts.length; i++) {
            var m = muts[i];
            if (m.type === 'childList') {
                for (var j = 0; j < m.addedNodes.length; j++) {
                    var n = m.addedNodes[j];
                    if (n.nodeType !== 1) continue;
                    if (n.tagName === 'IMG') rewriteImg(n);
                    else if (n.querySelectorAll) rewriteAllImgs(n);
                }
            } else if (m.type === 'attributes' && m.target.tagName === 'IMG') {
                rewriteImg(m.target);
            }
        }
    });

    // ------------------------------------------------------------------
    // 图片粘贴:统一入口。截图(剪贴板)与拖拽文件都走这里
    // ------------------------------------------------------------------
    function captureImagePaste(rid) {
        var root = editorRoot(), selection = window.getSelection();
        var range = selection && selection.rangeCount ? selection.getRangeAt(0) : null;
        var active = document.activeElement;
        pendingImages[rid] = { root: root,
            blocked: active && /^(INPUT|TEXTAREA)$/.test(active.tagName) && (!root || !root.contains(active)),
            range: range && root && root.contains(range.commonAncestorContainer)
            ? range.cloneRange() : null };
    }
    function sendImage(file) {
        if (!file || !Number.isFinite(file.size) || file.size <= 0 || file.size > 32 * 1024 * 1024) {
            notice('图片为空或超过 32 MB，无法粘贴');
            return;
        }
        var rid = ++imgSeq;
        captureImagePaste(rid);
        var reader = new FileReader();
        reader.onload = function () {
            if (!pendingImages[rid]) return; // The document was replaced while reading.
            var res = String(reader.result);
            var b64 = res.split(',')[1] || '';
            // 带上真实 MIME:C++ 侧按它决定落盘扩展名(截图/拖入的 jpg、webp
            // 不再一律存成 .png)
            var mime = res.slice(5, res.indexOf(';')) || file.type || '';
            post({
                t: 'saveImage', rid: rid,
                name: file.name || '',
                mime: mime,
                base64: b64
            });
        };
        reader.onerror = function () {
            delete pendingImages[rid];
            notice('无法读取剪贴板图片，请重新截图后粘贴');
        };
        reader.readAsDataURL(file);
    }

    // Ctrl+滚轮 = 整页缩放:滚轮事件被 Chromium 子窗口吃掉,Qt 收不到,
    // 所以在页面里拦下来上报给 C++,由 C++ 调 WebView2 的 ZoomFactor
    // (等比缩放整页,与 Chrome 行为一致)
    document.addEventListener('wheel', function (e) {
        if (!e.ctrlKey || currentMode === 'sv') return;
        e.preventDefault();
        post({ t: 'zoom', dir: e.deltaY < 0 ? 1 : -1 });
    }, { passive: false, capture: true });

    // 链接文字点击委托:Alt+点击 = 在系统浏览器打开(渲染层没有真 <a>,
    // href 存在 data-ms-href 上)
    document.addEventListener('click', function (e) {
        var el = e.target;
        while (el && el !== document.body) {
            if (el.getAttribute && el.getAttribute('data-ms-href')) {
                if (e.altKey) {
                    e.preventDefault();
                    e.stopPropagation();
                    post({ t: 'openUrl', url: el.getAttribute('data-ms-href') });
                }
                return;
            }
            el = el.parentElement;
        }
    }, true);

    // 右键菜单:编辑区内右键 -> 原生菜单(颜色等)
    document.addEventListener('contextmenu', function (e) {
        var root = editorRoot();
        if (!root || !root.contains(e.target)) return;
        e.preventDefault();
        e.stopPropagation();
        contextMathBlock = mathBlockAt(e.target) || selectedMathBlock();
        var source = contextMathBlock && contextMathBlock.querySelector('pre.vditor-ir__marker--pre > code');
        post({ t: 'contextMenu', math: !!source, mathBold: !!source && mathFormat(source.textContent).bold });
    }, true);

    // 捕获阶段拦截:Ctrl+Shift+V 纯文本 / 图片粘贴
    document.addEventListener('paste', function (e) {
        var root = editorRoot();
        if (!root || !root.contains(e.target)) return;
        suppressUntil = 0;
        // Screenshots may also advertise HTML/text. Prefer actual image bytes
        // before rich-text or LaTeX paste handling can consume the event.
        var imageItems = e.clipboardData && e.clipboardData.items;
        for (var ii = 0; imageItems && ii < imageItems.length; ii++) {
            if (imageItems[ii].kind !== 'file' || !/^image\//.test(imageItems[ii].type || '')) continue;
            var imageFile = imageItems[ii].getAsFile();
            if (imageFile) {
                e.preventDefault(); e.stopPropagation(); sendImage(imageFile); return;
            }
        }
        if (e.clipboardData && Array.prototype.indexOf.call(e.clipboardData.types, 'text/plain') !== -1 &&
            (insertMathSource(e.clipboardData.getData('text/plain')) ||
             insertCodeSource(e.clipboardData.getData('text/plain')))) {
            e.preventDefault();
            e.stopPropagation();
            return;
        }
        if (e.clipboardData && !currentCodeBlock()) {
            var raw = e.clipboardData.getData('text/plain');
            // Browser copies carry HTML. Convert it first so escaped dollar
            // markers and inline-code formulas take the same path as Markdown.
            var html = e.clipboardData.getData('text/html');
            if (html) {
                var holder = document.createElement('template'); holder.innerHTML = html;
                holder.content.querySelectorAll('script,style,iframe,object').forEach(function (node) { node.remove(); });
                raw = vd.vditor.lute.HTML2Md(holder.innerHTML);
            }
            var normalized = html && window.msLatexPreference
                ? window.msLatexPreference.normalizeClipboard(raw, preferLatex) : preferredText(raw);
            if (normalized !== raw) {
                e.preventDefault(); e.stopPropagation();
                suppressUntil = 0;
                recordMathUndo(); vd.insertMD(normalized); rerender(); recordMathUndo();
                onInput(vd.getValue()); return;
            }
        }
        var items = e.clipboardData && e.clipboardData.items;
        if (e.ctrlKey && e.shiftKey && items) {
            for (var pi = 0; pi < items.length; pi++) {
                if (items[pi].kind === 'string' && items[pi].type === 'text/plain') {
                    e.preventDefault();
                    e.stopPropagation();
                    items[pi].getAsString(function (txt) {
                        if (vd) { vd.insertValue(txt, true); vd.focus(); }
                    });
                    return;
                }
            }
        }
        // 纯文本/富文本粘贴:不拦截,交给 vditor
    }, true);

    // vditor 的上传入口(拖拽文件/工具栏上传走这里)
    function uploadHandler(files) {
        var file = files && files[0];
        if (file) sendImage(file);
        return null;
    }

    // ------------------------------------------------------------------
    // 编辑器创建 / 重建(切换模式需要)
    // ------------------------------------------------------------------
    function createEditor(mode, value) {
        if (['ir', 'sv', 'wysiwyg'].indexOf(mode) < 0) throw new Error('Invalid editor mode');
        var generation = ++editorGeneration;
        vdReady = false;
        contextMathBlock = null;
        codeInputComposing = false;
        resetLangPicker();
        codeLangBlock = null;
        if (vd) { try { vd.destroy(); } catch (e) {} vd = null; }
        currentMode = mode;
        activeBlock = null;
        window._msCbEdit = null;
        gutterLayer = null;
        hiCol = null;
        hiPre = null;
        hideLangSuggest();
        if (codeLangBtn) codeLangBtn.style.display = 'none';

        vd = new Vditor('vd', {
            mode: mode,
            lang: 'zh_CN',
            theme: themePending === 'dark' ? 'dark' : 'classic',
            cdn: 'vditor',
            value: value || '',
            height: '100%',
            cache: { enable: false },
            counter: { enable: false },
            toolbar: [],
            toolbarConfig: { hide: true },
            placeholder: '',
            resize: { enable: false },
            link: { isOpen: false },
            preview: {
                theme: {
                    current: themePending === 'dark' ? 'dark' : (themePending === 'paper' ? 'wechat' : 'light'),
                    path: 'vditor/dist/css/content-theme'
                },
                // 这些语法 Vditor 默认全关,不打开就会原样显示标记文本:
                // [toc] 目录 / ==高亮== / H~2~O 下标 / x^2^ 上标 / 脚注 / 图片题注
                markdown: {
                    toc: true,
                    mark: true,
                    sub: true,
                    sup: true,
                    footnotes: true,
                    imageCaption: true
                },
                math: { engine: 'KaTeX', inlineDigit: true },
                hljs: {
                    // 语言选择由 ms-lang-suggest 负责，禁止内核再生成第二份列表。
                    langs: [],
                    style: themePending === 'dark' ? 'dark' : 'github',
                    lineNumber: false,   // 行号由我们的行号列画,不再让 Vditor 塞行号 DOM
                    enable: true
                },
                delay: 400
            },
            upload: {
                accept: 'image/*',
                multiple: false,
                linkToImgUrl: '',
                handler: uploadHandler
            },
            input: function (value) { if (generation === editorGeneration) onInput(value); },
            after: function () {
                if (generation !== editorGeneration) return;
                clearStaleCaretBookmarks();
                vd.clearStack();
                document.documentElement.style.setProperty('--ms-font-size', fontSize + 'px');
                document.documentElement.style.setProperty('--ms-line-height', String(lineHeight));
                // 初始文档自带 front matter 时标记为合法(输入路径不得回退)
                syncFrontMatterFlag();
                observeFrontMatter();   // 直盯 DOM:IME 合成输入也逃不过
                // 重建编辑器后 DOM 树是全新的:观察器若还盯着旧的(已分离的)
                // .vditor-ir,一切变更驱动的补挂全部失灵 —— 必须改盯新树
                (function reobserveHeading() {
                    if (generation !== editorGeneration) return;
                    var root = editorRoot();
                    if (!root) {
                        setTimeout(reobserveHeading, 300);
                        return;
                    }
                    headingObserver.disconnect();
                    headingObserver.observe(root.parentElement || root, {
                        childList: true, subtree: true
                    });
                    expandObserver.disconnect();
                    expandObserver.observe(root.parentElement || root, {
                        attributes: true, attributeFilter: ['class'], subtree: true
                    });
                })();
                if (docHost) rewriteAllImgs(document);
                renderColorTags();     // 重开文档时把 <font> 标签还原成颜色
                ensureHljs();
                decorateAllCodeBlocks();
                pushStats();
                pushOutline();
                pushFirstLine();       // 初始内容(含 msInitialState 路径)的文件名候选
                if (!vdReady) {
                    vdReady = true;
                    while (vdReady && generation === editorGeneration && initQueue.length) {
                        try { initQueue.shift()(); } catch (e) { console.error(e); }
                    }
                    if (generation !== editorGeneration) return;
                    post({ t: 'ready' });
                    window.msWaitForRender(document.getElementById('vd'), function () {
                        return Array.from(document.querySelectorAll('.vditor-ir__preview .language-math'))
                            .some(function (el) { return el.textContent.trim() && !el.querySelector('.katex,.katex-error')
                                && !el.classList.contains('vditor-reset--error'); })
                            || Array.from(document.querySelectorAll('.vditor-ir__preview .language-mermaid'))
                            .some(function (el) { return el.textContent.trim() && !el.querySelector('svg')
                                && !el.classList.contains('vditor-reset--error'); });
                    }).then(function () {
                        if (generation === editorGeneration) post({ t: 'editorRendered' });
                    }, function () {
                        if (generation === editorGeneration) post({ t: 'editorLoadError' });
                    });
                }
            }
        });
    }

    // ------------------------------------------------------------------
    // Export from a detached snapshot, using the same local renderers/options.
    // getHTML alone contains raw LaTeX and unhighlighted code.
    function renderExportHtml() {
        var snapshot = document.createElement('div');
        snapshot.innerHTML = stripColorSpans(vd ? vd.getHTML() : '');
        var mathOptions = vd ? vd.vditor.options.preview.math : {};
        snapshot.querySelectorAll('.language-math').forEach(function (element) {
            var source = element.textContent.replace(/\u00a0/g, ' ');
            element.setAttribute('data-math', source);
            try {
                element.innerHTML = window.katex.renderToString(source, {
                    displayMode: element.tagName === 'DIV',
                    output: 'html',
                    macros: mathOptions.macros || {}
                });
            } catch (error) {
                element.textContent = error.message;
                element.classList.add('vditor-reset--error');
            }
        });
        var root = editorRoot();
        var sourceBlocks = root ? Array.prototype.slice.call(root.querySelectorAll(
            '[data-block="0"][data-type="code-block"]')) : [];
        snapshot.querySelectorAll('pre > code').forEach(function (code) {
            var match = /(?:^|\s)language-([^\s]+)/.exec(code.className);
            var lang = match ? match[1] : '';
            if (RENDERED_LANGS[lang]) {
                var original = sourceBlocks.find(function (block) {
                    var source = block.querySelector('pre.vditor-ir__marker--pre code');
                    return source && source.textContent.trim() === code.textContent.trim();
                });
                var preview = original && original.querySelector('.vditor-ir__preview');
                var visual = preview && preview.querySelector('code');
                if (visual && visual.querySelector('svg,canvas,img')) {
                    var copy = visual.cloneNode(true);
                    var canvases = visual.querySelectorAll('canvas');
                    copy.querySelectorAll('canvas').forEach(function (canvas, index) {
                        var image = document.createElement('img');
                        image.src = canvases[index].toDataURL('image/png');
                        canvas.replaceWith(image);
                    });
                    code.replaceWith(copy);
                }
                return;
            }
            var html = highlightCode(code.textContent, resolveLang(lang));
            if (html) { code.innerHTML = html; code.classList.add('hljs'); }
        });
        rewriteAllImgs(snapshot);
        var images = Array.prototype.map.call(snapshot.querySelectorAll('img'), function (img) {
            var src = img.getAttribute('src') || '';
            if (/^[A-Za-z]:[\\/]/.test(src)) {
                img.src = new URL('file:///' + src.replace(/\\/g, '/')).href;
                return Promise.resolve();
            }
            if (src && !/^(?:[a-z][a-z\d+.-]*:|\/\/)/i.test(src) && docHost)
                img.src = new URL(src, 'https://' + docHost + '/').href;
            var url = new URL(img.src || src, location.href);
            if (url.origin !== location.origin && !/^(?:doc\d+|imgpool\d+)\.local$/.test(url.hostname))
                return Promise.resolve();
            return fetch(url.href).then(function (response) {
                if (!response.ok) throw new Error('Image unavailable');
                return response.blob();
            }).then(function (blob) {
                return new Promise(function (resolve, reject) {
                    var reader = new FileReader();
                    reader.onload = function () { img.src = reader.result; resolve(); };
                    reader.onerror = reject;
                    reader.readAsDataURL(blob);
                });
            }).catch(function () { /* Keep the resolved URL for the host fallback. */ });
        });
        return Promise.all(images).then(function () { return snapshot.innerHTML; });
    }

    // C++ -> JS 暴露的桥接口(全部经过初始化门)
    // ------------------------------------------------------------------
    var impl = {
        repairLatex: function () {
            var source=vd.getValue(), converted=window.msLatexPreference.normalize(source);
            if(source===converted) { notice('未发现需要转换的公式');return; }
            if (!recordCodeUndo()) { notice('无法记录撤销状态，已取消转换'); return; }
            suppressUntil=0;vd.setValue(converted,false);clearStaleCaretBookmarks();
            decorateMathBlocks();recordMathUndo();onInput(vd.getValue());
            notice('公式已修复，可撤销本次转换');
        },
        setPreferLatex: function (enabled) {
            preferLatex = !!enabled;
            latexBefore = null; clearTimeout(latexTimer);
        },
        setContent: function (md) {
            pendingImages = Object.create(null);
            contextMathBlock = null;
            if (!vd) return;
            codeInputComposing = false;
            resetLangPicker();
            window._msCbEdit = null;
            codeLangBlock = null;
            if (codeLangBtn) codeLangBtn.style.display = 'none';
            suppressUntil = Date.now() + 200; // 覆盖 input 事件的触发窗口
            rev++;
            lastValue = (md == null ? '' : String(md));
            try { vd.setValue(lastValue, false); }
            catch (e) { /* 忽略 */ }
            // 加载自带 front matter 的文档:标记为合法,输入路径不得回退
            fmHadBefore = /^---[ \t]*\r?\n[\s\S]*\r?\n---[ \t]*(\r?\n|$)/.test(lastValue);
            clearStaleCaretBookmarks();
            vd.clearStack();
            setTimeout(function () {
                pushStats();
                pushOutline();
                pushFirstLine();   // 程序设值也更新默认文件名候选
                if (docHost) rewriteAllImgs(document);
                renderColorTags();   // 正文里的 <font> 标签渲染成真颜色
                decorateAllCodeBlocks();   // setValue 重渲染后重挂行号
            }, 50);
        },

        restoreContent: function (md, revision) {
            impl.setContent(md);
            if (Number.isInteger(revision) && revision >= 0) rev = revision;
        },

        requestContent: function (request) {
            post({ t: 'content', md: stripColorSpans(vd ? vd.getValue() : ''), rev: rev, request: request || 0 });
        },

        // 导出用:渲染后的 HTML(含公式/图表的最终形态)
        requestHtml: function (request) {
            var titleSource = window.mswTitleSource();
            renderExportHtml().then(function (html) {
                post({ t: 'html', html: html, titleSource: titleSource, request: request || 0 });
            }).catch(function (error) {
                post({ t: 'exportError', request: request || 0 });
                post({ t: 'jserror', msg: String(error), src: 'export' });
                notice('导出渲染失败，请重试');
            });
        },

        // 切换标签时由 C++ 调用:刷新统计与大纲
        refresh: function () {
            // 切标签时必须重发大纲:签名是页面级缓存,B 文档标题若碰巧
            // 和刚离开的 A 一样,pushOutline 会直接 return,侧栏就停在 A 上
            lastOutlineSig = null;
            lastStatsValue = null;
            pushStats();
            pushOutline();
        },

        // 设置文档目录虚拟主机,并把现有图片 src 改写过去
        setDocHost: function (name) {
            if (docHost !== String(name || '')) {
                savedImageHosts = []; savedImageFiles = {}; savedImageHostSignature = '';
            }
            docHost = String(name || '');
            rewriteAllImgs(document);
            if (!imgObserverStarted && document.body) {
                imgObserverStarted = true;
                imgObserver.observe(document.body, {
                    childList: true, subtree: true,
                    attributes: true, attributeFilter: ['src']
                });
            }
        },

        // 设置图片池目录映射表 [{host, prefix}],并把现有图片改写过去
        setImgDirs: function (list) {
            try {
                imgDirs = (list && list.map) ? list : [];
            } catch (e) {
                imgDirs = [];
            }
            rewriteAllImgs(document);
        },

        captureImagePaste: captureImagePaste,

        rebindImageHosts: function (hosts, name, files) {
            // Called only after assets and Markdown were successfully saved.
            files = files || [];
            var signature = name + ':' + hosts.join(',') + ':' + files.join(',');
            if (signature === savedImageHostSignature) return;
            savedImageHostSignature = signature;
            savedImageFiles = {};
            files.forEach(function (file) { savedImageFiles[file] = true; });
            savedImageHosts = hosts; docHost = name;
            rewriteAllImgs(document);
        },

        imageSaved: function (rid, rel) {
            if (!vd) return;
            var pending = pendingImages[rid];
            if (rid && !pending) return; // Ignore stale/duplicate replies.
            delete pendingImages[rid];
            if (pending && pending.blocked) { notice('请先将光标放到文档中，再粘贴图片'); return; }
            if (pending && pending.root !== editorRoot()) {
                notice('编辑视图已切换，请重新粘贴图片'); return;
            }
            vd.focus();
            if (pending && pending.range && pending.range.startContainer.isConnected && pending.range.endContainer.isConnected) {
                var selection = window.getSelection();
                selection.removeAllRanges(); selection.addRange(pending.range);
            }
            // ![image-20260916100852457](./assets/image-20260916100852457.png)
            var base = String(rel).split('/').pop().replace(/\.[^.]+$/, '');
            // Explicit user paste must be dirty even immediately after loading.
            suppressUntil = 0;
            recordMathUndo();
            vd.insertMD('\n![' + base + '](' + rel + ')\n');
            rerender();
            // Resolve the document host before the snapshot, so the observer
            // does not create an extra undo step containing only a URL change.
            rewriteAllImgs(editorRoot());
            recordMathUndo();
            onInput(vd.getValue());
        },

        imageRejected: function (rid, err) {
            delete pendingImages[rid];
            notice('图片粘贴失败：' + err);
            console.warn('图片保存失败:', err);
        },

        scrollToHeading: function (index) {
            if (!vd) return;
            if (currentMode === 'ir' || currentMode === 'wysiwyg') {
                var heads = document.querySelectorAll(
                    '.vditor-ir h1,.vditor-ir h2,.vditor-ir h3,.vditor-ir h4,.vditor-ir h5,.vditor-ir h6,'
                    + '.vditor-wysiwyg h1,.vditor-wysiwyg h2,.vditor-wysiwyg h3,.vditor-wysiwyg h4,.vditor-wysiwyg h5,.vditor-wysiwyg h6');
                if (heads[index]) {
                    heads[index].scrollIntoView({ behavior: 'smooth', block: 'start' });
                    return;
                }
            }
            if (currentMode === 'sv') {
                var lines = vd.getValue().split('\n');
                var fence = false, seen = 0, lineNo = 0;
                for (var i = 0; i < lines.length; i++) {
                    if (/^\s*(```|~~~)/.test(lines[i])) { fence = !fence; continue; }
                    if (fence) continue;
                    if (/^#{1,6}\s+/.test(lines[i])) {
                        if (seen === index) { lineNo = i; break; }
                        seen++;
                    }
                }
                var ta = document.querySelector('.vditor-sv textarea');
                if (ta && lineNo > 0) {
                    var lh = parseFloat(getComputedStyle(ta).lineHeight) || 22;
                    ta.scrollTop = Math.max(0, (lineNo - 1) * lh);
                }
            }
        },

        // 工作区搜索跳转:滚动到包含指定文本的块
        scrollToText: function (text) {
            if (!text) return;
            var root = editorRoot();
            if (!root) return;
            var lower = String(text).toLowerCase();
            var blocks = root.children;
            for (var i = 0; i < blocks.length; i++) {
                var t = blocks[i].textContent || '';
                if (t.toLowerCase().indexOf(lower) >= 0) {
                    blocks[i].scrollIntoView({ behavior: 'smooth', block: 'center' });
                    return;
                }
            }
        },

        // 当前文档内搜索:返回匹配块列表
        searchInDoc: function (query) {
            var items = [];
            lastFind.query = String(query || '');
            lastFind.idx = -1;
            if (!query) { post({ t: 'docSearch', items: items }); return; }
            var root = editorRoot();
            if (!root) { post({ t: 'docSearch', items: items }); return; }
            var lower = String(query).toLowerCase();
            for (var i = 0; i < root.children.length && items.length < 200; i++) {
                var t = (root.children[i].textContent || '').replace(/\s+/g, ' ').trim();
                if (t && t.toLowerCase().indexOf(lower) >= 0) {
                    items.push({ i: i, text: t.slice(0, 120) });
                }
            }
            post({ t: 'docSearch', items: items });
        },

        scrollToBlock: function (index) {
            var root = editorRoot();
            if (root && root.children[index])
                root.children[index].scrollIntoView({ behavior: 'smooth', block: 'center' });
        },

        setTheme: function (theme) {
            themePending = String(theme);
            document.body.dataset.msTheme = themePending;
            var link = document.getElementById('msThemeCss');
            if (link) link.href = 'themes/' + themePending + '.css';
            if (vd) {
                var map = { light: ['classic', 'light', 'github'],
                            dark:  ['dark', 'dark', 'dark'],
                            paper: ['classic', 'wechat', 'github'] };
                var m = map[themePending] || map.light;
                try { vd.setTheme(m[0], m[1], m[2]); } catch (e) { /* 旧参数容错 */ }
            }
            scheduleGutters();
        },

        // 宿主在缩放/字号等引起布局级联的变化后调用:重排行号与高亮
        resyncDecor: function () {
            decorateAllCodeBlocks();
            scheduleGutters();
        },

        setFontSize: function (px) {
            var v = parseInt(px, 10);
            if (v >= 12 && v <= 28) fontSize = v;
            document.documentElement.style.setProperty('--ms-font-size', fontSize + 'px');
            // 字号变了,代码行高随之变 —— 行号/高亮列必须重排
            scheduleGutters();
        },

        // 正文行距(偏好设置可调)。只作用正文:代码块/公式行距固定,不随动
        setLineHeight: function (v) {
            var n = parseFloat(v);
            if (!(n >= 1.0 && n <= 2.2)) return;
            lineHeight = n;
            document.documentElement.style.setProperty('--ms-line-height', String(lineHeight));
            scheduleGutters();
        },

        // 代码块行号开关。开=自研行号系统;关=MarkText 式纯代码卡片:
        // 行号列完全不生成,左槽 16px(CSS 按 body.ms-no-lineno 切换)
        setLineNumbers: function (on) {
            showLineNumbers = !!on;
            document.body.classList.toggle('ms-no-lineno', !showLineNumbers);
            scheduleGutters();
        },

        // 本文档默认代码语言(C++ 按路径持久化,标签页各自独立)
        setDocLang: function (lang) {
            docLang = String(lang || '');
            if (langSuggestEl && !langSuggestEl.hidden) syncLangSuggest();
        },

        setFocusMode: function (on) {
            focusMode = !!on;
            document.body.classList.toggle('ms-focus', focusMode);
            if (!focusMode && activeBlock) {
                activeBlock.classList.remove('ms-active-block');
                activeBlock = null;
            }
            if (focusMode) onSelectionChange();
        },

        setTypewriter: function (on) {
            typewriter = !!on;
        },

        toggleMode: function () {
            var next = currentMode === 'ir' ? 'sv' : 'ir';
            // 切换前从 DOM 提取一次真实内容(此时 src 可能已被改写,
            // 落盘时由 C++ 还原为绝对路径);上色 span 要剥掉,否则会被
            // 当成正文写进 markdown
            var value = '';
            try {
                value = stripColorSpans(vd ? vd.getValue() : '');
            } catch (e) {
                post({ t: 'jserror', msg: String(e), src: 'toggleMode' });
                notice('无法读取当前内容，已取消切换');
                return;
            }
            lastValue = value;
            createEditor(next, value);
            post({ t: 'mode', value: next });
        },

        // Ctrl+F / F3 由 C++ 转发到这里:打开顶部查找栏(可选预填当前选中文本)
        openFind: function (prefill) {
            openFindbar(prefill);
        },

        closeFind: function () {
            closeFindbar();
        },

        selectAll: function () {
            try {
                if (vd) vd.focus();
                document.execCommand('selectAll');
            } catch (e) { /* 忽略 */ }
        },

        focus: function () {
            if (vd) vd.focus();
        },

        insertText: function (text) {
            suppressUntil = 0;
            if (insertMathSource(String(text))) return;
            if (insertCodeSource(String(text))) return;
            var codeTarget = preferLatex && currentMode === 'ir' ? currentCodeBlock() : null;
            var beforeCode = codeTarget ? blockMarkdown(codeTarget) : null;
            if (vd) {
                var normalized = currentCodeBlock() ? String(text) : preferredText(String(text));
                // AI supplies Markdown even when no delimiter needed rewriting.
                // insertValue treats complete blocks as literal editor text.
                if (!currentCodeBlock()) { recordMathUndo(); vd.insertMD(normalized); }
                else vd.insertValue(String(text), true);
            }
            if (codeTarget && convertEditedLatexBlock(beforeCode, codeTarget)) return;
            // AI 插入后强制重渲染:insertValue 对 **x** 围栏等 markdown
            // 的 IR 解析有光标位置依赖,补一拍 input 事件确保生效
            rerender();
            recordMathUndo();
            if (vd) onInput(vd.getValue());
        },

        // 菜单/右键"粘贴":剪贴板由 C++ 读(Qt 侧有完整权限),这里只负责
        // 落进编辑器。execCommand('insertText') 会替换选区,与系统粘贴一致
        pasteText: function (text) {
            var t = String(text == null ? '' : text);
            if (!t) return;
            suppressUntil = 0;
            if (vd) vd.focus();
            if (insertMathSource(t)) return;
            if (insertCodeSource(t)) return;
            var codeTarget = preferLatex && currentMode === 'ir' ? currentCodeBlock() : null;
            var beforeCode = codeTarget ? blockMarkdown(codeTarget) : null;
            var normalized = currentCodeBlock() ? t : preferredText(t);
            if (normalized !== t) {
                recordMathUndo(); vd.insertMD(normalized); rerender(); recordMathUndo(); onInput(vd.getValue()); return;
            }
            var ok = false;
            try { ok = document.execCommand('insertText', false, t); } catch (e) {}
            if (!ok) {
                try { if (vd) vd.insertValue(t, true); } catch (e2) {}
            }
            if (codeTarget && convertEditedLatexBlock(beforeCode, codeTarget)) return;
            rerender();
            if (vd) vd.focus();
        },

        // 文字颜色:把 <font color> 标记写进正文(文件格式,重开不丢);
        // 显示层由 renderColorTags 隐藏标签并给内容上色
        applyColor: function (hex) {
            if (formatMathBlock('color', hex)) return;
            var sel = window.getSelection();
            if (!sel.rangeCount) return;
            // 代码块里不能上色:<font> 标记会写进代码正文
            if (currentCodeBlock()) { notice('代码块内不能设置文字颜色'); return; }
            var r = sel.getRangeAt(0);
            var b = caretBlock();
            if (!b) { notice('光标不在文本块中'); return; }

            if (hex === 'clear') {
                // 找到覆盖选区的 <font>…</font> 标记对:删标记 + 内容脱色
                var tags = b.querySelectorAll('span.vditor-ir__node');
                var open = null, close = null;
                var s = r.startContainer, e = r.endContainer;
                for (var i = 0; i < tags.length && !open; i++) {
                    if (!INLINE_TAGS[0].open.test((tags[i].textContent || '').trim()))
                        continue;
                    for (var j = i + 1; j < tags.length; j++) {
                        if (tags[j].parentNode !== tags[i].parentNode)
                            break;
                        if (INLINE_TAGS[0].close.test((tags[j].textContent || '').trim())) {
                            if (nodeBefore(tags[i], s) && nodeBefore(e, tags[j])) {
                                open = tags[i];
                                close = tags[j];
                            }
                            break;
                        }
                    }
                }
                if (!open || !close) { notice('选中文字没有颜色'); return; }
                var colored = open.parentNode.querySelectorAll('span[data-ms-color]');
                for (var k = 0; k < colored.length; k++) {
                    var sp = colored[k];
                    if (nodeBefore(open, sp) && nodeBefore(sp, close)) {
                        while (sp.firstChild) sp.parentNode.insertBefore(sp.firstChild, sp);
                        sp.remove();
                    }
                }
                open.remove();
                close.remove();
                rerender();
                return;
            }

            var txt = selectionTextClean(r).replace(/<\/?font[^>]*>/gi, '');
            if (!txt) {
                if (b.tagName !== 'P') { notice('请先选中要变色的文字'); return; }
                txt = (b.textContent || '').replace(/\u200b/g, '')
                                           .replace(/<\/?font[^>]*>/gi, '').trim();
                if (!txt) return;
                var wr = document.createRange();
                wr.selectNodeContents(b);
                sel.removeAllRanges();
                sel.addRange(wr);
                r = wr;
            }
            // 标记直接写进正文;Vditor 重渲染后 renderColorTags 会把标签藏起来上色
            var marked = '<font color="' + hex + '">' + txt + '</font>';
            var ok = false;
            try { ok = document.execCommand('insertText', false, marked); } catch (e) {}
            if (!ok) {
                try {
                    r.deleteContents();
                    r.collapse(true);
                    sel.removeAllRanges();
                    sel.addRange(r);
                    vd.insertValue(marked, true);
                } catch (e2) {}
            }
            rerender();
            setTimeout(renderColorTags, 150);
            vd.focus();
        },

        // Ctrl+K 超链接:C++ 弹地址输入框后调这里;插 [文字](链接),
        // execCommand 走可撤销编辑路径,IR 原生识别成链接
        insertLink: function (url, text) {
            if (!vd || !url) return;
            vd.focus();
            var marked = '[' + (text || '链接') + '](' + url + ')';
            var ok = false;
            try { ok = document.execCommand('insertText', false, marked); } catch (e) {}
            if (!ok) { try { vd.insertValue(marked, true); } catch (e2) {} }
            rerender();
            vd.focus();
        },

        // 右键菜单编辑命令
        toggleMathBold: function () {
            formatMathBlock('bold');
        },

        editCmd: function (cmd) {
            try { document.execCommand(cmd); } catch (e) {}
            if (vd) vd.focus();
        },

        // 菜单"撤销/重做":走 Vditor 自己的撤销栈(execCommand('undo') 在 IR
        // 里静默失败,真实按键也是被 Vditor 的 keydown 接管走这里)
        editUndo: function () {
            suppressUntil = 0;
            if (vd && vd.vditor && vd.vditor.undo) {
                try { vd.vditor.undo.undo(vd.vditor); return; } catch (e) {}
            }
            document.execCommand('undo');
        },

        editRedo: function () {
            suppressUntil = 0;
            if (vd && vd.vditor && vd.vditor.undo) {
                try { vd.vditor.undo.redo(vd.vditor); return; } catch (e) {}
            }
            document.execCommand('redo');
        },

        replaceAll: function (find, repl) {
            if (!vd || typeof find !== 'string' || !find || typeof repl !== 'string') {
                post({ t: 'replaced', count: 0 }); return;
            }
            var v = vd.getValue();
            var n = 0, pos = 0, at;
            while ((at = v.indexOf(find, pos)) !== -1) { n++; pos = at + find.length; }
            if (v.length + n * (repl.length - find.length) > 64 * 1024 * 1024) {
                notice('替换结果超过 64 MB，已取消替换');
                post({ t: 'replaced', count: 0 }); return;
            }
            if (n > 0) {
                // 先把替换前状态压进撤销栈,再用"不清栈"的 setValue —— 全部替换
                // 必须能整单 Ctrl+Z。旧实现 setValue(…,true) 直接清空撤销栈,
                // 替换错了回不去,2 秒后还会被自动保存落盘。
                // 不设 suppressUntil:这是用户编辑,必须标脏,否则关窗不提示。
                if (!recordCodeUndo()) {
                    notice('无法记录撤销状态，已取消替换');
                    post({ t: 'replaced', count: 0 }); return;
                }
                suppressUntil = 0;
                vd.setValue(v.replaceAll(find, function () { return repl; }), false);
                recordCodeUndo();
                onInput(vd.getValue());
            }
            post({ t: 'replaced', count: n });
        }
    };

    window.msbridge = {};
    Object.keys(impl).forEach(function (k) {
        window.msbridge[k] = function () {
            var args = arguments;
            whenReady(function () { impl[k].apply(null, args); });
        };
    });


    // ------------------------------------------------------------------
    // 启动
    // ------------------------------------------------------------------
    document.body.dataset.msTheme = themePending;
    document.getElementById('msThemeCss').href = 'themes/' + themePending + '.css';
    createEditor('ir', lastValue);
    window.msbridge.setTheme(themePending);
    window.msbridge.setFontSize(fontSize);
    ensureLangIcons();
    // 行号默认态的 CSS 类要在启动就挂上:宿主指令到达前(或独立打开页面时)
    // 左槽宽度才会按默认"无行号"模式排版,否则首帧是 40px 再跳 16px
    document.body.classList.toggle('ms-no-lineno', !showLineNumbers);
})();
