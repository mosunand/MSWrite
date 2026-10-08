/* Wait for resources and stable layout, even when WebView2 pauses occluded frames. */
(() => {
    'use strict';
    window.msWaitForRender = (root, pending = () => false) => new Promise((resolve, reject) => {
        let settled = false, pollTimer = null, frameTimer = null, frame = null;
        const deadline = setTimeout(() => finish(Error('Render timed out')), 14000);
        function finish(error) {
            if (settled) return;
            settled = true;
            clearTimeout(deadline); clearTimeout(pollTimer); clearTimeout(frameTimer);
            if (frame !== null) cancelAnimationFrame(frame);
            if (error) reject(error); else resolve();
        }
        function nextFrame(callback) {
            let delivered = false;
            const done = () => {
                if (delivered || settled) return;
                delivered = true;
                clearTimeout(frameTimer);
                if (frame !== null) cancelAnimationFrame(frame);
                frame = null;
                callback();
            };
            // Native loading windows can occlude the browser and stop rAF.
            frameTimer = setTimeout(done, 100);
            frame = requestAnimationFrame(done);
        }
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
            if (settled) return;
            if (!root.isConnected) { finish(Error('Render cancelled')); return; }
            // Reading layout also starts font loads for newly inserted math.
            root.getBoundingClientRect();
            if (unsettled()) { pollTimer = setTimeout(check, 30); return; }
            nextFrame(() => nextFrame(() => {
                root.getBoundingClientRect();
                if (!root.isConnected) finish(Error('Render cancelled'));
                else if (unsettled()) check(); else finish();
            }));
        }
        check();
    });
})();
