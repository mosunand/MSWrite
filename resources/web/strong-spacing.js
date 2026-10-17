(function () {
    'use strict';
    // Extend only unparsed prose in Lute's IR DOM. Treat inline formulas/code
    // as opaque content so their source is never interpreted as delimiters.
    function renderStrong(html, forHTML = false) {
        if (!html.includes('**')) return html;
        const template = document.createElement('template');
        template.innerHTML = html;
        let changed = false;
        function renderRun(nodes) {
            const segments = nodes.filter(node => node.nodeName !== 'WBR');
            const text = segments.map(node => node.nodeType === Node.TEXT_NODE ? node.data : '\uFFFC').join('');
            const pattern = /(?<![\\*])\*\*(?!\*)([^*\r\n]+?)\*\*(?!\*)/g;
            const matches = [...text.matchAll(pattern)].filter(match =>
                match[1].trim() && /^(?:\s)|(?:\s)$/.test(match[1]));
            function point(offset) {
                for (const node of segments) {
                    if (node.nodeType === Node.TEXT_NODE) {
                        if (offset <= node.length) return [node, offset];
                        offset -= node.length;
                    } else {
                        if (offset <= 1) {
                            const index = Array.prototype.indexOf.call(node.parentNode.childNodes, node);
                            return [node.parentNode, index + offset];
                        }
                        offset--;
                    }
                }
            }
            for (const match of matches.reverse()) {
                const range = document.createRange();
                range.setStart(...point(match.index));
                range.setEnd(...point(match.index + match[0].length));
                const body = range.extractContents();
                const walker = document.createTreeWalker(body, NodeFilter.SHOW_TEXT);
                const content = [];
                while (walker.nextNode()) content.push(walker.currentNode);
                // Remove the four delimiter characters, retaining whitespace
                // and any wbr caret bookmark inside the matched text.
                let remaining = 2;
                for (const node of content) {
                    const count = Math.min(remaining, node.length);
                    node.deleteData(0, count); remaining -= count;
                    if (!remaining) break;
                }
                remaining = 2;
                for (const node of content.reverse()) {
                    const count = Math.min(remaining, node.length);
                    node.deleteData(node.length - count, count); remaining -= count;
                    if (!remaining) break;
                }
                let leading = '', trailing = '';
                if (forHTML) {
                    // IR-to-HTML parses emphasis again. Keep boundary spaces
                    // outside this temporary wrapper so it stays valid Markdown;
                    // editable IR and its source retain the original whitespace.
                    const first = content.slice().reverse().find(node => node.length);
                    const prefix = first && /^\s+/.exec(first.data);
                    if (prefix) { leading = prefix[0]; first.deleteData(0, leading.length); }
                    const last = content.find(node => node.length);
                    const suffix = last && /\s+$/.exec(last.data);
                    if (suffix) { trailing = suffix[0]; last.deleteData(last.length - trailing.length, trailing.length); }
                }
                const wrapper = document.createElement('span');
                wrapper.className = 'vditor-ir__node'; wrapper.dataset.type = 'strong';
                const marker = document.createElement('span');
                marker.className = 'vditor-ir__marker vditor-ir__marker--bi'; marker.textContent = '**';
                const strong = document.createElement('strong'); strong.dataset.newline = '1';
                strong.appendChild(body);
                wrapper.append(marker, strong, marker.cloneNode(true));
                range.insertNode(wrapper);
                if (leading) wrapper.before(document.createTextNode(leading));
                if (trailing) wrapper.after(document.createTextNode(trailing));
                changed = true;
            }
        }
        function visit(parent) {
            let run = [];
            for (const node of Array.from(parent.childNodes)) {
                if (node.nodeType === Node.TEXT_NODE || node.nodeName === 'WBR' ||
                    (node.nodeType === Node.ELEMENT_NODE && node.matches(
                        '.vditor-ir__node[data-type="inline-node"],.vditor-ir__node[data-type="code"]'))) {
                    run.push(node); continue;
                }
                renderRun(run); run = [];
                if (node.nodeType !== Node.ELEMENT_NODE || node.matches(
                    'pre,code,script,style,[data-type],.vditor-ir__marker,.vditor-ir__preview')) continue;
                visit(node);
            }
            renderRun(run);
        }
        visit(template.content);
        return changed ? template.innerHTML : html;
    }
    const createLute = Lute.New;
    Lute.New = function () {
        const lute = createLute.apply(this, arguments);
        const toIR = lute.Md2VditorIRDOM, toHTML = lute.Md2HTML;
        for (const name of ['Md2VditorIRDOM', 'SpinVditorIRDOM', 'HTML2VditorIRDOM']) {
            const original = lute[name];
            lute[name] = function () { return renderStrong(original.apply(lute, arguments)); };
        }
        lute.Md2HTML = function (markdown) {
            if (/\*\*\s|\s\*\*/.test(markdown)) {
                const ir = toIR.call(lute, markdown), formatted = renderStrong(ir, true);
                if (formatted !== ir) return lute.VditorIRDOM2HTML(formatted);
            }
            return toHTML.apply(lute, arguments);
        };
        return lute;
    };
})();
