(function (root) {
    'use strict';
    function validMath(body) {
        if (!body.trim()) return false;
        if (!root.katex) return true;
        try { root.katex.renderToString(body, { throwOnError: true }); return true; }
        catch (_) { return false; }
    }
    function edits(source, convertCode = true, decodeMath = false) {
        const changes = [];
        function display(body) {
            const formula = /^\$\$(?!\$)([\s\S]+?)\$\$$/.exec(body.trim());
            return formula && !formula[1].includes('$$') && validMath(formula[1])
                ? '$$\n' + formula[1].trim() + '\n$$' : null;
        }
        // Fences and inline code are literal unless a complete fence contains
        // only one valid display formula. Delimiter lengths must match.
        function inline(text, offset) {
            let masked = text.replace(/^[^\n]*/gm, line => /^(?: {4}|\t)/.test(quoted(line).text)
                ? ' '.repeat(line.length) : line);
            masked = masked.replace(/(?<!`)(`+)(?!`)([\s\S]*?)(?<!`)\1(?!`)/g, (match, ticks, body, index) => {
                const math = display(body);
                if (math && convertCode) changes.push({ start:offset+index, end:offset+index+match.length,
                    text:'\n\n'+math+'\n\n' });
                return ' '.repeat(match.length);
            });
            masked = masked.replace(/(?<![\\$])(\${1,2})(?!\$)[\s\S]*?(?<![\\$])\1(?!\$)/g,
                match => ' '.repeat(match.length));
            if (decodeMath) {
                const strong = /(?<![\\*])\\\*\\\*([^\r\n]+?)\\\*\\\*(?!\*)/g;
                let emphasis;
                while ((emphasis = strong.exec(masked))) {
                    const start = emphasis.index, end = start + emphasis[0].length;
                    const body = text.slice(start + 4, end - 4);
                    if (!body.trim()) continue;
                    // Change only the delimiters so math edits inside bold
                    // text remain independent and keep their original offsets.
                    changes.push({ start:offset+start,
                        end:offset+start+4, text:'**' });
                    changes.push({ start:offset+end-4,
                        end:offset+end, text:'**' });
                }
            }
            const pair = /(?<![\\$])(\\{1,2})\$([^$\r\n]+?)\1\$(?![$\w])/g;
            let match;
            while ((match = pair.exec(masked))) {
                // HTML2Md also escapes underscores and LaTeX backslashes.
                // Decode them only for rich-text math, never literal code.
                const body = decodeMath ? match[2].replace(/\\([!"#$%&'()*+,\-./:;<=>?@[\\\]^_`{|}~])/g, '$1') : match[2];
                if (validMath(body)) changes.push({ start: offset + match.index,
                    end: offset + match.index + match[0].length, text: '$' + body + '$' });
            }
        }
        const lines = source.match(/[^\n]*\n|[^\n]+$/g) || [];
        function quoted(line) {
            let prefix = '', marker;
            while ((marker = /^ {0,3}>[ \t]?/.exec(line))) {
                prefix += marker[0]; line = line.slice(marker[0].length);
            }
            return { prefix, depth:(prefix.match(/>/g) || []).length, text:line };
        }
        let offset = 0, proseStart = 0;
        for (let i = 0; i < lines.length; i++) {
            const first = quoted(lines[i]);
            const opening = /^ {0,3}(`{3,}|~{3,})[^\n]*\n?$/.exec(first.text);
            if (opening) {
                inline(source.slice(proseStart, offset), proseStart);
                const closing = new RegExp('^ {0,3}' + opening[1][0] + '{' + opening[1].length + ',}[ \\t]*\\n?$');
                let j = i + 1;
                while (j < lines.length) {
                    const line = quoted(lines[j]);
                    if (line.depth === first.depth && closing.test(line.text)) break;
                    j++;
                }
                if (j === lines.length) return changes.sort((a,b) => a.start-b.start);
                const length = lines.slice(i, j + 1).join('').length;
                const bodyLines = lines.slice(i + 1, j).map(quoted);
                const body = bodyLines.every(line => line.depth === first.depth)
                    ? bodyLines.map(line => line.text).join('').trim() : '';
                const formula = display(body);
                if (formula && convertCode)
                    changes.push({ start: offset, end: offset + length,
                        text: formula.split('\n').map(line => first.prefix + line).join('\n')
                            + (lines[j].endsWith('\n') ? '\n' : '') });
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
    function normalizeClipboard(source, preferLatex) {
        const pending = edits(source, !!preferLatex, true);
        for (let i = pending.length - 1; i >= 0; i--) {
            const change = pending[i];
            source = source.slice(0, change.start) + change.text + source.slice(change.end);
        }
        return source;
    }
    const api = { normalize, edits, changes, normalizeClipboard };
    root.msLatexPreference = api;
    if (typeof module !== 'undefined') module.exports = api;
})(typeof window === 'undefined' ? globalThis : window);
