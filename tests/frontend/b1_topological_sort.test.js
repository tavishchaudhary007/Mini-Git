const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

// Test B1: Time Travel and dashboard order (item 9)
// Verifies that getChronologicalCommits and getGraphCommits:
// 1. Only return commits reachable from HEAD (current branch)
// 2. Return commits in valid topological order (all parents precede children)
// 3. Exclude branches/commits not reachable from HEAD
// 4. Correctly break ties for commits with identical timestamps using parent order

const htmlPath = path.resolve(__dirname, '../../templates/frontend.html');
const htmlContent = fs.readFileSync(htmlPath, 'utf8');

const scriptMatch = htmlContent.match(/<script\b[^>]*>([\s\S]*?)<\/script>/gi);
assert(scriptMatch && scriptMatch.length >= 4, "Must find script tags in frontend.html");
const mainScript = scriptMatch[3].replace(/<\/?script\b[^>]*>/gi, '');

function createEnv() {
    const sandbox = {
        console,
        window: {
            location: { protocol: 'http:', host: 'localhost:8000' },
            addEventListener: () => {},
            removeEventListener: () => {}
        },
        document: {
            getElementById: () => null,
            createElement: () => ({ setAttribute: () => {}, appendChild: () => {}, style: {} }),
            createElementNS: () => ({ setAttribute: () => {}, appendChild: () => {}, style: {} }),
            addEventListener: () => {},
            removeEventListener: () => {}
        },
        DAG_PALETTE: ['#3B82F6', '#10B981', '#F59E0B', '#EF4444', '#8B5CF6', '#EC4899'],
        setTimeout: () => {},
        clearTimeout: () => {},
        setInterval: () => {},
        clearInterval: () => {}
    };
    sandbox.window.document = sandbox.document;
    vm.createContext(sandbox);
    vm.runInContext(mainScript, sandbox);
    return sandbox;
}

function setRepoState(env, objects, branches, currentBranch) {
    env.__test_objects = objects;
    env.__test_branches = branches;
    env.__test_currentBranch = currentBranch;
    vm.runInContext(`
        OBJECTS = { ...__test_objects };
        BRANCHES = { ...__test_branches };
        CURRENT_BRANCH = __test_currentBranch;
        IS_SERVER_MODE = true;
    `, env);
}

function verifyTopological(commits) {
    const seen = new Set();
    for (let i = 0; i < commits.length; ++i) {
        const c = commits[i];
        if (c.parent) {
            assert(seen.has(c.parent), `Commit ${c.hash} at index ${i} appears before its parent ${c.parent}`);
        }
        if (c.parent2) {
            assert(seen.has(c.parent2), `Commit ${c.hash} at index ${i} appears before its parent2 ${c.parent2}`);
        }
        seen.add(c.hash);
    }
}

// 1. Same timestamp test & unreachable branch exclusion
{
    const env = createEnv();
    const sameTime = "2026-01-01T12:00:00Z";

    // C1 -> C2 -> C3 (main)
    // C1 -> C_unreachable (feature-x)
    const objects = {
        'c1': { type: 'commit', data: { message: 'root', timestamp: sameTime, parent: null } },
        'c2': { type: 'commit', data: { message: 'c2', timestamp: sameTime, parent: 'c1' } },
        'c3': { type: 'commit', data: { message: 'c3', timestamp: sameTime, parent: 'c2' } },
        'c_unreachable': { type: 'commit', data: { message: 'unreachable', timestamp: sameTime, parent: 'c1' } }
    };
    const branches = {
        'main': 'c3',
        'feature-x': 'c_unreachable'
    };

    setRepoState(env, objects, branches, 'main');

    const chrono = vm.runInContext('getChronologicalCommits()', env);
    const hashes = chrono.map(c => c.hash);

    // Must only include reachable commits: c1, c2, c3
    assert.strictEqual(hashes.includes('c_unreachable'), false, "Unreachable branch commit must be excluded");
    assert.deepStrictEqual(JSON.parse(JSON.stringify(hashes)), ['c1', 'c2', 'c3'], "Must be in topological order c1 -> c2 -> c3");
    verifyTopological(chrono);
    console.log("Passed: Same timestamp and unreachable branch exclusion");
}

// 2. Merge commit with tie-breaking by parent order
{
    const env = createEnv();
    const t0 = "2026-01-01T10:00:00Z";
    const tSame = "2026-01-01T12:00:00Z";

    //        c_feat (parent2)
    //       /      \
    //   c1 --------- m1 (merge)
    //   \
    //    c_main (parent1)
    const objects = {
        'c1': { type: 'commit', data: { message: 'root', timestamp: t0, parent: null } },
        'c_main': { type: 'commit', data: { message: 'main commit', timestamp: tSame, parent: 'c1' } },
        'c_feat': { type: 'commit', data: { message: 'feat commit', timestamp: tSame, parent: 'c1' } },
        'm1': { type: 'commit', data: { message: 'merge', timestamp: "2026-01-01T13:00:00Z", parent: 'c_main', parent2: 'c_feat' } }
    };
    const branches = {
        'main': 'm1'
    };

    setRepoState(env, objects, branches, 'main');

    const chrono = vm.runInContext('getChronologicalCommits()', env);
    const hashes = chrono.map(c => c.hash);

    // Root must be first, merge must be last
    assert.strictEqual(hashes[0], 'c1');
    assert.strictEqual(hashes[3], 'm1');
    // Topological order must hold
    verifyTopological(chrono);

    // In getGraphCommits (newest to oldest):
    const graphRes = vm.runInContext('getGraphCommits(true)', env);
    const graphHashes = graphRes.commits.map(c => c.hash);
    assert.strictEqual(graphHashes[0], 'm1', "Graph must start at HEAD");
    // Under parentPriority, parent 1 (c_main) has priority 1, parent 2 (c_feat) has priority 2
    assert(graphHashes.indexOf('c_main') < graphHashes.indexOf('c_feat'), "Parent 1 must precede Parent 2 in graph traversal");

    console.log("Passed: Merge commit with parent order tie-breaking");
}

// 3. Complex diamond with multiple branches and identical timestamps
{
    const env = createEnv();
    const t = "2026-02-01T00:00:00Z";

    //          b1 -> b2
    //        /          \
    //   root              m (HEAD)
    //        \          /
    //          a1 -> a2
    const objects = {
        'root': { type: 'commit', data: { message: 'root', timestamp: t, parent: null } },
        'a1': { type: 'commit', data: { message: 'a1', timestamp: t, parent: 'root' } },
        'a2': { type: 'commit', data: { message: 'a2', timestamp: t, parent: 'a1' } },
        'b1': { type: 'commit', data: { message: 'b1', timestamp: t, parent: 'root' } },
        'b2': { type: 'commit', data: { message: 'b2', timestamp: t, parent: 'b1' } },
        'm': { type: 'commit', data: { message: 'm', timestamp: t, parent: 'a2', parent2: 'b2' } }
    };
    const branches = { 'main': 'm' };

    setRepoState(env, objects, branches, 'main');

    const chrono = vm.runInContext('getChronologicalCommits()', env);
    assert.strictEqual(chrono.length, 6);
    assert.strictEqual(chrono[0].hash, 'root');
    assert.strictEqual(chrono[5].hash, 'm');
    verifyTopological(chrono);
    console.log("Passed: Complex diamond with identical timestamps");
}

console.log("All B1 topological sort tests passed!");
