// The input of tal's web engines (E6e), the same in each: what the program sends as clicks, the
// wheel, keys and text, done in the page by its own means, with the page's scripts on or off. The
// engine runs it once a document is made (in a world of its own where the engine has one) and then
// calls tal.click, tal.wheel, tal.key and tal.type; each returns "" or "upload" when the page asked
// for a file to send, which the engine refuses and tells. Works in the main frame only.
(() => {
    if (globalThis.tal) return;

    const doc = document;
    const fromPoint = doc.elementFromPoint.bind(doc);
    const exec = doc.execCommand.bind(doc);
    const click = HTMLElement.prototype.click;
    const focus = HTMLElement.prototype.focus;
    let pointX = 0;
    let pointY = 0;

    const at = (x, y) => {
        let one = fromPoint(x, y);

        while (one && one.shadowRoot) {
            const inner = one.shadowRoot.elementFromPoint(x, y);

            if (!inner || inner === one) break;

            one = inner;
        }

        return one;
    };

    const editable = (one) => {
        if (!one) return false;

        if (one.isContentEditable) return true;

        if (one.tagName === "TEXTAREA") return !one.readOnly && !one.disabled;

        if (one.tagName !== "INPUT") return false;

        const plain = ["text", "search", "url", "tel", "email", "password", "number", ""];

        return plain.includes((one.getAttribute("type") || "").toLowerCase()) && !one.readOnly && !one.disabled;
    };

    const scrollable = (one, vertical) => {
        for (; one && one !== doc.documentElement; one = one.parentElement) {
            const style = getComputedStyle(one);
            const flow = vertical ? style.overflowY : style.overflowX;
            const more = vertical ? one.scrollHeight > one.clientHeight : one.scrollWidth > one.clientWidth;

            if (more && (flow === "auto" || flow === "scroll")) return one;
        }

        return doc.scrollingElement || doc.documentElement;
    };

    const tabbable = () =>
        Array.from(doc.querySelectorAll("a[href], button, input, select, textarea, [tabindex], [contenteditable]")).filter(
            (one) => one.tabIndex >= 0 && !one.disabled && one.getClientRects().length > 0 && !(one.tagName === "INPUT" && one.type === "hidden"),
        );

    const tabbed = (back) => {
        const all = tabbable().sort((a, b) => (a.tabIndex || Infinity) - (b.tabIndex || Infinity));

        if (all.length === 0) return;

        const now = all.indexOf(doc.activeElement);
        const next = now < 0 ? (back ? all.length - 1 : 0) : (now + (back ? all.length - 1 : 1)) % all.length;

        focus.call(all[next]);
    };

    const scrolled = (dx, dy) => scrollable(at(pointX, pointY), dy !== 0).scrollBy(dx, dy);

    globalThis.tal = {
        click(x, y) {
            pointX = x;
            pointY = y;

            const one = at(x, y);

            if (!one) return "";

            const target = one.closest("a, button, input, select, textarea, label, [onclick], [tabindex]") || one;

            if (target.tagName === "INPUT" && target.type === "file") return "upload";

            if (editable(target) || target.tagName === "SELECT") {
                focus.call(target);
            } else {
                click.call(target);

                if (target.tabIndex >= 0) focus.call(target);
            }

            return "";
        },

        wheel(dx, dy) {
            scrolled(dx, dy);

            return "";
        },

        key(name, letter, mods) {
            const one = doc.activeElement;
            const shift = (mods & 1) !== 0;

            if (name === "tab") {
                tabbed(shift);
            } else if (name === "enter") {
                if (one && one.tagName === "A") click.call(one);
                else if (one && (one.tagName === "TEXTAREA" || one.isContentEditable)) exec("insertLineBreak");
                else if (one && one.tagName === "INPUT" && one.form) one.form.requestSubmit();
                else if (one && one.tagName === "BUTTON") click.call(one);
            } else if (name === "backspace" || name === "delete") {
                if (editable(one)) exec(name === "backspace" ? "delete" : "forwardDelete");
            } else if (name === "escape") {
                if (one && one.blur) one.blur();
            } else if (one && one.tagName === "SELECT" && (name === "up" || name === "down")) {
                const index = Math.max(0, Math.min(one.options.length - 1, one.selectedIndex + (name === "up" ? -1 : 1)));

                if (index !== one.selectedIndex) {
                    one.selectedIndex = index;
                    one.dispatchEvent(new Event("input", { bubbles: true }));
                    one.dispatchEvent(new Event("change", { bubbles: true }));
                }
            } else if (editable(one) && ["left", "right", "up", "down", "home", "end"].includes(name)) {
                const how = { left: ["backward", "character"], right: ["forward", "character"], up: ["backward", "line"],
                              down: ["forward", "line"], home: ["backward", "lineboundary"], end: ["forward", "lineboundary"] }[name];

                getSelection().modify(shift ? "extend" : "move", how[0], how[1]);
            } else {
                const page = innerHeight * 0.9;
                const step = { up: [0, -40], down: [0, 40], left: [-40, 0], right: [40, 0], pageup: [0, -page], pagedown: [0, page] }[name];

                if (step) scrolled(step[0], step[1]);
                else if (name === "home") (doc.scrollingElement || doc.documentElement).scrollTo(0, 0);
                else if (name === "end") (doc.scrollingElement || doc.documentElement).scrollTo(0, 1e9);
            }

            return "";
        },

        type(text) {
            if (editable(doc.activeElement)) exec("insertText", false, text);

            return "";
        },
    };
})();
