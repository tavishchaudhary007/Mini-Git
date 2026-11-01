const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

// Test A1: Save & commit / Save & stash in the unsaved-changes dialog
// Verifies that dirty buffers are copied before saving so that all files are staged,
// that apiPost('stage') results are checked, and that demo mode commit/stash failure aborts.

const htmlPath = path.resolve(__dirname, '../../templates/frontend.html');
const htmlContent = fs.readFileSync(htmlPath, 'utf8');

// Extract the script containing confirmDirtyBuffersBefore and saveFileBuffer
const scriptMatch = htmlContent.match(/<script\b[^>]*>([\s\S]*?)<\/script>/gi);
assert(scriptMatch && scriptMatch.length >= 4, "Must find script tags in frontend.html");
const mainScript = scriptMatch[3].replace(/<\/?script\b[^>]*>/gi, '');

// Extract confirmDirtyBuffersBefore function source
const funcMatch = mainScript.match(/async function confirmDirtyBuffersBefore[\s\S]*?\n {8}\}/);
assert(funcMatch, "Must find confirmDirtyBuffersBefore in frontend.html");
const confirmDirtyBuffersBeforeSrc = funcMatch[0];

function createSandbox(overrides = {}) {
    const stagedFiles = [];
    const toasts = [];
    let commitCalled = false;
    let stashCalled = false;

    const sandbox = {
        console,
        IS_SERVER_MODE: true,
        DIRTY_BUFFERS: {
            'file1.txt': 'hello dirty 1',
            'file2.txt': 'hello dirty 2'
        },
        hasUnsavedEdits: () => true,
        promptDirtySwitchChoice: async () => 'commit',
        prompt: (msg, def) => 'Auto commit test',
        showToast: (msg, type) => {
            toasts.push({ msg, type });
        },
        saveFileBuffer: async (fp) => {
            delete sandbox.DIRTY_BUFFERS[fp];
            return true;
        },
        apiPost: async (endpoint, data) => {
            if (endpoint === 'stage') {
                stagedFiles.push(data.path);
                return { ok: true };
            }
            if (endpoint === 'commit') {
                commitCalled = true;
                return { ok: true, hash: '1234567890' };
            }
            if (endpoint === 'stash/save') {
                stashCalled = true;
                return { ok: true };
            }
            return { ok: true };
        },
        refreshStashList: async () => {},
        executeQuickStash: async () => true,
        executeGitCommit: async () => true,
        STAGING_INDEX: {},
        VFS: {},
        sha256: async (s) => 'hash_' + s.length,
        OBJECTS: {},
        ...overrides
    };

    sandbox._captures = { stagedFiles, toasts, getCommitCalled: () => commitCalled, getStashCalled: () => stashCalled };
    vm.createContext(sandbox);
    vm.runInContext(confirmDirtyBuffersBeforeSrc, sandbox);
    return sandbox;
}

async function runTests() {
    console.log("Running A1 regression tests...");

    // Test 1: Commit in server mode stages all dirty files
    {
        const sb = createSandbox();
        const res = await sb.confirmDirtyBuffersBefore("merge");
        assert.strictEqual(sb._captures.stagedFiles.length, 2, `Expected 2 staged files, got ${sb._captures.stagedFiles.length}`);
        assert(sb._captures.stagedFiles.includes('file1.txt'));
        assert(sb._captures.stagedFiles.includes('file2.txt'));
        assert.strictEqual(sb._captures.getCommitCalled(), true);
        assert.strictEqual(res, true);
    }

    // Test 2: If stage fails on one file, abort with a toast naming the file
    {
        const sb = createSandbox({
            apiPost: async (endpoint, data) => {
                if (endpoint === 'stage') {
                    if (data.path === 'file2.txt') return { ok: false, error: 'lock held' };
                    return { ok: true };
                }
                if (endpoint === 'commit') return { ok: true };
                return { ok: true };
            }
        });
        const res = await sb.confirmDirtyBuffersBefore("merge");
        assert.strictEqual(res, false, "Must return false when staging fails");
        const foundToast = sb._captures.toasts.some(t => t.msg && t.msg.includes('file2.txt') && t.type === 'error');
        assert(foundToast, `Expected toast naming 'file2.txt', got: ${JSON.stringify(sb._captures.toasts)}`);
    }

    // Test 3: Demo mode commit uses executeGitCommit return value
    {
        const sb = createSandbox({
            IS_SERVER_MODE: false,
            executeGitCommit: async () => false
        });
        const res = await sb.confirmDirtyBuffersBefore("checkout");
        assert.strictEqual(res, false, "Must return false when executeGitCommit fails in demo mode");
    }

    // Test 4: Demo mode stash uses executeQuickStash return value
    {
        const sb = createSandbox({
            IS_SERVER_MODE: false,
            promptDirtySwitchChoice: async () => 'stash',
            executeQuickStash: async () => false
        });
        const res = await sb.confirmDirtyBuffersBefore("revert");
        assert.strictEqual(res, false, "Must return false when executeQuickStash fails in demo mode");
    }

    console.log("ALL A1 REGRESSION TESTS PASSED!");
}

runTests().catch((err) => {
    console.error("TEST FAILED as expected on unpatched code:");
    console.error(err.message);
    process.exit(1);
});
