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
    const reg = new RegExp(`(?:async\\s+)?function ${name}\\b[\\s\\S]*?\\n {8}\\}`);
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

    // Test 5: executeLanPull error shows exactly 1 error toast, divergence shows exactly 1 warning toast with Merge action
    {
        console.log("-> Test 5: executeLanPull divergence and error toast handling");
        const lanPullSrc = extractFunction('executeLanPull');

        // 5a: Divergence (peerRefs non-empty, ok=false)
        {
            const toasts = [];
            const sandbox = {
                console,
                IS_SERVER_MODE: true,
                IS_SYNCING_LAN: false,
                document: {
                    getElementById: (id) => ({ value: 'http://192.168.1.50:8080', disabled: false })
                },
                localStorage: { setItem: () => {}, getItem: () => '' },
                cleanPeerUrl: u => u,
                getPeerSessionToken: () => 'token',
                closeLanSyncModal: () => {},
                fetch: async () => ({ ok: true, status: 200, json: async () => ({ branches: {}, objects: {} }) }),
                apiPost: async () => ({ ok: false, error: 'Diverged', peerRefs: ['peer-main'] }),
                showToast: (msg, type = 'info', action = null) => {
                    toasts.push({ msg, type, action });
                }
            };
            vm.createContext(sandbox);
            vm.runInContext(lanPullSrc, sandbox);
            await sandbox.executeLanPull();

            const actionToasts = toasts.filter(t => t.action && t.action.label === 'Merge peer-main');
            assert.strictEqual(actionToasts.length, 1, `Expected exactly 1 toast with 'Merge peer-main' action, got: ${JSON.stringify(toasts)}`);
            assert.strictEqual(actionToasts[0].type, 'warning', `Expected warning toast, got: ${actionToasts[0].type}`);
        }

        // 5b: Divergence (peerRefs non-empty, ok=true)
        {
            const toasts = [];
            const sandbox = {
                console,
                IS_SERVER_MODE: true,
                IS_SYNCING_LAN: false,
                document: {
                    getElementById: (id) => ({ value: 'http://192.168.1.50:8080', disabled: false })
                },
                localStorage: { setItem: () => {}, getItem: () => '' },
                cleanPeerUrl: u => u,
                getPeerSessionToken: () => 'token',
                closeLanSyncModal: () => {},
                fetch: async () => ({ ok: true, status: 200, json: async () => ({ branches: {}, objects: {} }) }),
                apiPost: async () => ({ ok: true, peerRefs: ['peer-main'] }),
                showToast: (msg, type = 'info', action = null) => {
                    toasts.push({ msg, type, action });
                }
            };
            vm.createContext(sandbox);
            vm.runInContext(lanPullSrc, sandbox);
            await sandbox.executeLanPull();

            const actionToasts = toasts.filter(t => t.action && t.action.label === 'Merge peer-main');
            assert.strictEqual(actionToasts.length, 1, `Expected exactly 1 toast with 'Merge peer-main' action, got: ${JSON.stringify(toasts)}`);
            assert.strictEqual(actionToasts[0].type, 'warning', `Expected warning toast, got: ${actionToasts[0].type}`);
        }

        // 5c: Error (peerRefs empty, ok=false)
        {
            const toasts = [];
            const sandbox = {
                console,
                IS_SERVER_MODE: true,
                IS_SYNCING_LAN: false,
                document: {
                    getElementById: (id) => ({ value: 'http://192.168.1.50:8080', disabled: false })
                },
                localStorage: { setItem: () => {}, getItem: () => '' },
                cleanPeerUrl: u => u,
                getPeerSessionToken: () => 'token',
                closeLanSyncModal: () => {},
                fetch: async () => ({ ok: true, status: 200, json: async () => ({ branches: {}, objects: {} }) }),
                apiPost: async () => ({ ok: false, error: 'Corrupt bundle' }),
                showToast: (msg, type = 'info', action = null) => {
                    toasts.push({ msg, type, action });
                }
            };
            vm.createContext(sandbox);
            vm.runInContext(lanPullSrc, sandbox);
            await sandbox.executeLanPull();

            const errToasts = toasts.filter(t => t.type === 'error');
            assert.strictEqual(errToasts.length, 1, `Expected exactly 1 error toast, got: ${JSON.stringify(toasts)}`);
            assert(errToasts[0].msg.includes('Corrupt bundle'));
        }
    }

    // Test 6: uploadRepositoryBundle error shows exactly 1 error toast, divergence shows exactly 1 warning toast with Merge action
    {
        console.log("-> Test 6: uploadRepositoryBundle divergence and error toast handling");
        const uploadBundleSrc = extractFunction('uploadRepositoryBundle');

        async function runUpload(apiResult) {
            const toasts = [];
            let onloadCb = null;
            class MockFileReader {
                readAsText() {
                    if (onloadCb) {
                        onloadCb({ target: { result: JSON.stringify({ minigit_bundle: 1 }) } });
                    }
                }
                set onload(cb) { onloadCb = cb; }
                get onload() { return onloadCb; }
            }

            const sandbox = {
                console,
                IS_SERVER_MODE: true,
                FileReader: MockFileReader,
                apiPost: async () => apiResult,
                closeLanSyncModal: () => {},
                showToast: (msg, type = 'info', action = null) => {
                    toasts.push({ msg, type, action });
                }
            };
            vm.createContext(sandbox);
            vm.runInContext(uploadBundleSrc, sandbox);

            sandbox.uploadRepositoryBundle({ target: { files: [{ name: 'bundle.json' }], value: 'foo' } });
            await new Promise(r => setTimeout(r, 10));
            return toasts;
        }

        // 6a: Divergence (peerRefs non-empty, ok=true)
        {
            const toasts = await runUpload({ ok: true, peerRefs: ['peer-feature'] });
            const actionToasts = toasts.filter(t => t.action && t.action.label === 'Merge peer-feature');
            assert.strictEqual(actionToasts.length, 1, `Expected exactly 1 toast with 'Merge peer-feature', got: ${JSON.stringify(toasts)}`);
            assert.strictEqual(actionToasts[0].type, 'warning');
        }

        // 6b: Divergence (peerRefs non-empty, ok=false)
        {
            const toasts = await runUpload({ ok: false, error: 'Diverged', peerRefs: ['peer-feature'] });
            const actionToasts = toasts.filter(t => t.action && t.action.label === 'Merge peer-feature');
            assert.strictEqual(actionToasts.length, 1, `Expected exactly 1 toast with 'Merge peer-feature', got: ${JSON.stringify(toasts)}`);
            assert.strictEqual(actionToasts[0].type, 'warning');
        }

        // 6c: Error (peerRefs empty, ok=false)
        {
            const toasts = await runUpload({ ok: false, error: 'Corrupt bundle' });
            const errToasts = toasts.filter(t => t.type === 'error');
            assert.strictEqual(errToasts.length, 1, `Expected exactly 1 error toast, got: ${JSON.stringify(toasts)}`);
            assert(errToasts[0].msg.includes('Corrupt bundle'));
        }
    }

    console.log("ALL A3 REGRESSION TESTS PASSED!");
}

runTests().catch((err) => {
    console.error("TEST FAILED:");
    console.error(err);
    process.exit(1);
});
