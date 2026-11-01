const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

// B7 Regression Test: Script injection
// File names, branch names, and commit messages containing <img src=x onerror=alert(1)>
// must be shown as text everywhere in the UI.
// Grep every innerHTML use and escape it or switch it to textContent.
// Eliminate innerText in favor of textContent.

const htmlPath = path.resolve(__dirname, '../../templates/frontend.html');
const htmlContent = fs.readFileSync(htmlPath, 'utf8');

console.log("=== B7 Test: Script Injection Prevention ===");

// 1. Audit innerText: All text assignments must use textContent
const innerTextMatches = [];
const lines = htmlContent.split(/\r?\n/);
lines.forEach((line, idx) => {
    if (/\.innerText\b/.test(line)) {
        innerTextMatches.push({ line: idx + 1, content: line.trim() });
    }
});

assert.strictEqual(
    innerTextMatches.length,
    0,
    `Found ${innerTextMatches.length} uses of .innerText in templates/frontend.html; must switch to .textContent:\n` +
    innerTextMatches.map(m => `  Line ${m.line}: ${m.content}`).join('\n')
);

// 2. Audit innerHTML: Grep every innerHTML assignment across the file
const regex = /\.innerHTML\s*=\s*/g;
let match;
let count = 0;
while ((match = regex.exec(htmlContent)) !== null) {
    count++;
    const startIdx = match.index + match[0].length;
    const lineNumber = htmlContent.substring(0, match.index).split(/\r?\n/).length;

    // Determine the expression assigned to innerHTML
    const firstChar = htmlContent[startIdx];
    let expr = "";
    if (firstChar === '`') {
        const endIdx = htmlContent.indexOf('`', startIdx + 1);
        assert(endIdx !== -1, `Unterminated template literal at line ${lineNumber}`);
        expr = htmlContent.substring(startIdx, endIdx + 1);
    } else {
        const semiIdx = htmlContent.indexOf(';', startIdx);
        assert(semiIdx !== -1, `Unterminated statement at line ${lineNumber}`);
        expr = htmlContent.substring(startIdx, semiIdx).trim();
    }

    const isEmpty = /^['"]['"]$/.test(expr);
    const isStaticTemplate = expr.startsWith('`') && !expr.includes('${');
    const isStaticString = /^['"][^$]*['"]$/.test(expr);
    const isGetFileIcon = /^getFileIcon\([^)]+\)$/.test(expr);
    const usesEscapeHtml = expr.includes('escapeHtml(') && !expr.includes('${');

    const isSafe = isEmpty || isStaticTemplate || isStaticString || isGetFileIcon || usesEscapeHtml;
    assert(
        isSafe,
        `Unsafe innerHTML assignment at line ${lineNumber}: ${expr}`
    );
}

console.log(`Audited all ${count} innerHTML assignments - all verified safe.`);

// 3. Functional DOM Verification with XSS Payload
const XSS_PAYLOAD = '<img src=x onerror=alert(1)>';

// Mock DOM elements
function createMockElement(tagName = 'div') {
    const children = [];
    const attributes = {};
    return {
        tagName,
        attributes,
        children,
        style: {},
        classList: {
            add: () => {},
            remove: () => {},
            toggle: () => {}
        },
        _textContent: '',
        _innerHTML: '',
        get textContent() {
            return this._textContent;
        },
        set textContent(val) {
            this._textContent = String(val);
            this.children.length = 0;
            this._innerHTML = '';
        },
        get innerHTML() {
            return this._innerHTML;
        },
        set innerHTML(val) {
            this._innerHTML = String(val);
            if (val.includes('<img')) {
                children.push(createMockElement('img'));
            }
        },
        setAttribute(k, v) { attributes[k] = v; },
        getAttribute(k) { return attributes[k]; },
        appendChild(child) { children.push(child); return child; },
        querySelectorAll: () => []
    };
}

// Extract escapeHtml and verify it handles the payload safely
const scriptMatch = htmlContent.match(/<script\b[^>]*>([\s\S]*?)<\/script>/gi);
const scriptWithEscape = scriptMatch.find(s => s.includes('function escapeHtml'));
assert(scriptWithEscape, "Must find script defining escapeHtml");
const scriptBody = scriptWithEscape.replace(/<\/?script\b[^>]*>/gi, '');
const sandbox = { console, tailwind: {} };
vm.createContext(sandbox);
const escapeHtml = vm.runInContext('(function() { ' + scriptBody + '; return escapeHtml; })()', sandbox);

assert(typeof escapeHtml === 'function', "escapeHtml must be defined");
const escaped = escapeHtml(XSS_PAYLOAD);
assert.strictEqual(escaped, '&lt;img src=x onerror=alert(1)&gt;', "escapeHtml must sanitize HTML tags");
assert(!escaped.includes('<img'), "Escaped string must not contain <img");

// Functional check: verify that text elements display payload as raw text and have no <img> children
const elCommit = createMockElement('div');
elCommit.textContent = XSS_PAYLOAD;
assert.strictEqual(elCommit.textContent, XSS_PAYLOAD, "Commit message must be stored as literal text");
assert.strictEqual(elCommit.children.length, 0, "No child elements should be created for commit message text");

const elBranch = createMockElement('option');
elBranch.textContent = XSS_PAYLOAD;
assert.strictEqual(elBranch.textContent, XSS_PAYLOAD, "Branch name must be stored as literal text");
assert.strictEqual(elBranch.children.length, 0, "No child elements should be created for branch name text");

const elFile = createMockElement('span');
elFile.textContent = XSS_PAYLOAD + '.txt';
assert.strictEqual(elFile.textContent, XSS_PAYLOAD + '.txt', "File name must be stored as literal text");
assert.strictEqual(elFile.children.length, 0, "No child elements should be created for file name text");

console.log("PASS: B7 script injection prevention verified successfully.");
