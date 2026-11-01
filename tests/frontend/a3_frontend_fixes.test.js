const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

// Test A3: Frontend fixes (item 38)
// 1. Failed LAN pull and bundle upload show exactly one toast ({silent:true} or caller doesn't toast again)
// 2. DECISION_IN_FLIGHT flag set during /api/pair/decide, skips poll, never shows "timed out", cleared in finally
// 3. apiPost reads res.text() first, tries JSON.parse, returns {ok:false, error:'HTTP ' + res.status} and toasts that, never "Unexpected token"
// 4. discardFile keeps/restores buffer on failure and shows error

const htmlPath = path.resolve(__dirname, '../../templates/frontend.html');
const htmlContent = fs.readFileSync(htmlPath, 'utf8');

const scriptMatch = htmlContent.match(/<script\b[^>]*>([\s\S]*?)<\/script>/gi);
assert(scriptMatch && scriptMatch.length >= 4, "Must find script tags in frontend.html");
const mainScript = scriptMatch[3].replace(/<\/?script\b[^>]*>/gi, '');

function extractFunction(name) {
    const reg = new RegExp(`async function ${name}\\b[\\s\\S]*?\\n {8}\\}`);
    const match = mainScript.match(reg);
    assert(match, `Must find ${name} in frontend.html`);
    return match[0];
}

async function runTests() {
    console.log("Running A3 frontend regression tests...");

    // Test 1: apiPost non-JSON body returns {ok:false, error: 'HTTP ' + res.status}
    {
        console.log("-> Test 1: apiPost non-JSON response handling");
        const toasts = [];
        const sandbox = {
            console,
            IS_SERVER_MODE: true,
            STATE_SEQ: 0,
            LATEST_SNAPSHOT_ETAG: null,
            fetch: async (url, opts) => {
                return {
                    status: 502,
                    headers: { get: () => null },
                    text: async () => "Bad Gateway",
                    json: async () => { throw new SyntaxError("Unexpected token B in JSON at position 0"); }
                };
            },
            showToast: (msg, type) => {
                toasts.push({ msg, type });
            },
            window: { location: { protocol: 'http:' } },
            loadLiveRepo: () => {}
        };

        const apiPostSrc = extractFunction('apiPost');
        vm.createContext(sandbox);
        vm.runInContext(apiPostSrc, sandbox);

        const res = await sandbox.apiPost('status');
        assert(res !== null, "apiPost should not return null");
        assert.strictEqual(res.ok, false, "res.ok must be false on 502");
        assert.strictEqual(res.error, "HTTP 502", `Expected 'HTTP 502', got: ${res.error}`);
        const hasUnexpected = toasts.some(t => t.msg && t.msg.includes("Unexpected token"));
        assert(!hasUnexpected, "Must never toast 'Unexpected token'");
        const hasHttp502 = toasts.some(t => t.msg && t.msg.includes("HTTP 502"));
        assert(hasHttp502, `Expected toast containing 'HTTP 502', got: ${JSON.stringify(toasts)}`);
    }

    // Test 2: apiPost silent option suppresses toast
    {
        console.log("-> Test 2: apiPost silent option");
        const toasts = [];
        const sandbox = {
            console,
            IS_SERVER_MODE: true,
            STATE_SEQ: 0,
            LATEST_SNAPSHOT_ETAG: null,
            fetch: async (url, opts) => {
                return {
                    status: 500,
                    headers: { get: () => null },
                    text: async () => JSON.stringify({ ok: false, error: 'Internal Server Error' }),
                    json: async () => ({ ok: false, error: 'Internal Server Error' })
                };
            },
            showToast: (msg, type) => {
                toasts.push({ msg, type });
            },
            window: { location: { protocol: 'http:' } },
            loadLiveRepo: () => {}
        };

        const apiPostSrc = extractFunction('apiPost');
        vm.createContext(sandbox);
        vm.runInContext(apiPostSrc, sandbox);

        const res = await sandbox.apiPost('sync/import-bundle', {}, { silent: true });
        assert.strictEqual(res.ok, false);
        assert.strictEqual(toasts.length, 0, "When silent: true, apiPost must not show any toast");
    }

    // Test 3: Discard keeps buffer if /api/discard fails
    {
        console.log("-> Test 3: discardFile keeps edits if /api/discard fails");
        const toasts = [];
        const sandbox = {
            console,
            IS_SERVER_MODE: true,
            IS_STATIC_EXPORT: false,
            DIRTY_BUFFERS: { 'foo.txt': 'my unsaved edits' },
            VFS: { 'foo.txt': 'my unsaved edits' },
            ACTIVE_FILE: 'foo.txt',
            confirm: () => true,
            showToast: (msg, type) => {
                toasts.push({ msg, type });
            },
            apiPost: async (endpoint, data) => {
                if (endpoint === 'discard') {
                    return { ok: false, error: 'Disk write protected' };
                }
                return { ok: true };
            },
            window: { location: { protocol: 'http:' }, MINIGIT_LIVE: { files: { 'foo.txt': 'clean content' } } },
            loadLiveRepo: () => {},
            syncEditorWithActiveFile: () => {},
            updateUI: () => {}
        };

        const discardSrc = extractFunction('discardFile');
        vm.createContext(sandbox);
        vm.runInContext(discardSrc, sandbox);

        await sandbox.discardFile('foo.txt');
        assert.strictEqual(sandbox.DIRTY_BUFFERS['foo.txt'], 'my unsaved edits',
            "DIRTY_BUFFERS['foo.txt'] must be kept or restored when discard fails");
        const errorToast = toasts.some(t => t.type === 'error');
        assert(errorToast, "Must show an error toast when discard fails");
    }

    // Test 4: DECISION_IN_FLIGHT suppresses "timed out" in pairing poll
    {
        console.log("-> Test 4: DECISION_IN_FLIGHT flag during pairing decide");
        assert(mainScript.includes('DECISION_IN_FLIGHT'), "frontend.html must define and use DECISION_IN_FLIGHT");
        
        // Check that respondToPendingPairRequest sets and clears DECISION_IN_FLIGHT
        const respondSrc = extractFunction('respondToPendingPairRequest');
        assert(respondSrc.includes('DECISION_IN_FLIGHT = true'), "respondToPendingPairRequest must set DECISION_IN_FLIGHT = true");
        assert(respondSrc.includes('DECISION_IN_FLIGHT = false') || respondSrc.includes('finally'),
            "respondToPendingPairRequest must reset DECISION_IN_FLIGHT in finally");

        const pollSrc = extractFunction('pollPendingPairRequests');
        assert(pollSrc.includes('DECISION_IN_FLIGHT'), "pollPendingPairRequests must check DECISION_IN_FLIGHT");
    }

    console.log("ALL A3 REGRESSION TESTS PASSED!");
}

runTests().catch((err) => {
    console.error("TEST FAILED:");
    console.error(err);
    process.exit(1);
});
