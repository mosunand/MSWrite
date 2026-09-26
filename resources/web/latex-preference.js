(function (root) {
    'use strict';
    function validMath(body) {
        if (!body.trim()) return false;
        if (!root.katex) return true;
        try { root.katex.renderToString(body, { throwOnError: true }); return true; }
        catch (_) { return false; }
    }
    function edits(source) {
        const changes = [];
        function display(body) {
            const formula = /^\$\$(?!\$)([\s\S]+?)\$\$$/.exec(body.trim());
            return formula && !formula[1].includes('$$') && validMath(formula[1])
                ? '$$\n' + formula[1].trim() + '\n$$' : null;
        }
        // Fences and inline code are literal unless a complete fence contains
        // only one valid display formula. Delimiter lengths must match.
        function inline(text, offset) {
            let masked = text.replace(/^(?: {4}|\t)[^\n]*/gm, match => ' '.repeat(match.length));
            masked = masked.replace(/(?<!`)(`+)(?!`)([\s\S]*?)(?<!`)\1(?!`)/g, (match, ticks, body, index) => {
                const math = display(body);
                if (math) changes.push({ start:offset+index, end:offset+index+match.length,
                    text:'\n\n'+math+'\n\n' });
                return ' '.repeat(match.length);
            });
            masked = masked.replace(/(?<![\\$])(\${1,2})(?!\$)[\s\S]*?(?<![\\$])\1(?!\$)/g,
                match => ' '.repeat(match.length));
            const pair = /(?<![\\$])(\\{1,2})\$([^$\r\n]+?)\1\$(?![$\w])/g;
            let match;
            while ((match = pair.exec(masked))) {
                const body = match[2];
                if (validMath(body)) changes.push({ start: offset + match.index,
                    end: offset + match.index + match[0].length, text: '$' + body + '$' });
            }
        }
        const lines = source.match(/[^\n]*\n|[^\n]+$/g) || [];
        let offset = 0, proseStart = 0;
        for (let i = 0; i < lines.length; i++) {
            const opening = /^ {0,3}(`{3,}|~{3,})[^\n]*\n?$/.exec(lines[i]);
            if (opening) {
                inline(source.slice(proseStart, offset), proseStart);
                const closing = new RegExp('^ {0,3}' + opening[1][0] + '{' + opening[1].length + ',}[ \\t]*\\n?$');
                let j = i + 1;
                while (j < lines.length && !closing.test(lines[j])) j++;
                if (j === lines.length) return changes.sort((a,b) => a.start-b.start);
                const length = lines.slice(i, j + 1).join('').length;
                const body = lines.slice(i + 1, j).join('').trim();
                const formula = display(body);
                if (formula)
                    changes.push({ start: offset, end: offset + length,
                        text: formula + (lines[j].endsWith('\n') ? '\n' : '') });
                offset += length; proseStart = offset; i = j;
            } else {
                offset += lines[i].length;
            }
        }
        inline(source.slice(proseStart), proseStart);
        return changes.sort((a,b) => a.start-b.start);
    }
    function changes(source, previous) {
        let start = 0, end = source.length;
        if (typeof previous === 'string') {
            while (start < previous.length && start < source.length && previous[start] === source[start]) start++;
            let a = previous.length, b = source.length;
            while (a > start && b > start && previous[a - 1] === source[b - 1]) { a--; b--; }
            end = b;
            if (start === end) return [];
        }
        return edits(source).filter(change => change.start < end && change.end > start);
    }
    function normalize(source, previous) {
        const pending = changes(source, previous);
        for (let i = pending.length - 1; i >= 0; i--) {
            const change = pending[i];
            source = source.slice(0, change.start) + change.text + source.slice(change.end);
        }
        return source;
    }
    const api = { normalize, edits, changes };
    root.msLatexPreference = api;
    if (typeof module !== 'undefined') module.exports = api;
})(typeof window === 'undefined' ? globalThis : window);
