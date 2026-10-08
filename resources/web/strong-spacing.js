(function () {
    'use strict';
    // Extend only unparsed prose in Lute's IR DOM. Code, escaped markers and
    // existing inline nodes retain the engine's normal interpretation.
    function renderStrong(html) {
        if (!html.includes('**')) return html;
        const template = document.createElement('template');
        template.innerHTML = html;
        let changed = false;
        function renderRun(nodes) {
            const texts = nodes.filter(node => node.nodeType === Node.TEXT_NODE);
            const text = texts.map(node => node.data).join('');
            const pattern = /(?<![\\*])\*\*(?!\*)([^*\r\n]+?)\*\*(?!\*)/g;
            const matches = [...text.matchAll(pattern)].filter(match =>
                match[1].trim() && /^(?:\s)|(?:\s)$/.test(match[1]));
            function point(offset) {
                for (const node of texts) {
                    if (offset <= node.length) return [node, offset];
                    offset -= node.length;
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
                const wrapper = document.createElement('span');
                wrapper.className = 'vditor-ir__node'; wrapper.dataset.type = 'strong';
                const marker = document.createElement('span');
                marker.className = 'vditor-ir__marker vditor-ir__marker--bi'; marker.textContent = '**';
                const strong = document.createElement('strong'); strong.dataset.newline = '1';
                strong.appendChild(body);
                wrapper.append(marker, strong, marker.cloneNode(true));
                range.insertNode(wrapper); changed = true;
            }
        }
        function visit(parent) {
            let run = [];
            for (const node of Array.from(parent.childNodes)) {
                if (node.nodeType === Node.TEXT_NODE || node.nodeName === 'WBR') { run.push(node); continue; }
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
                const ir = toIR.call(lute, markdown), formatted = renderStrong(ir);
                if (formatted !== ir) return lute.VditorIRDOM2HTML(formatted);
            }
            return toHTML.apply(lute, arguments);
        };
        return lute;
    };
})();
