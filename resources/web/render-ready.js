/* A renderer can be initialized before CSS, fonts or diagrams have finished.
   Resolve only after those dependencies and two actual presentation frames. */
(() => {
    'use strict';
    window.msWaitForRender = (root, pending = () => false) => new Promise((resolve, reject) => {
        const started = performance.now();
        function unsettled() {
            if ([...document.querySelectorAll('link[rel="stylesheet"]')].some(link => !link.disabled && !link.sheet)) return true;
            if (document.fonts && document.fonts.status !== 'loaded') return true;
            if ([...root.querySelectorAll('img')].some(img => {
                if (img.complete) return false;
                const rect = img.getBoundingClientRect();
                return rect.height > 0 && rect.bottom > 0 && rect.top < innerHeight;
            })) return true;
            return pending();
        }
        function check() {
            if (!root.isConnected) { reject(Error('Render cancelled')); return; }
            if (performance.now() - started > 14000) { reject(Error('Render timed out')); return; }
            // Reading layout also starts font loads for newly inserted math.
            root.getBoundingClientRect();
            if (unsettled()) { setTimeout(check, 30); return; }
            requestAnimationFrame(() => requestAnimationFrame(() => {
                if (unsettled()) check(); else resolve();
            }));
        }
        check();
    });
})();
