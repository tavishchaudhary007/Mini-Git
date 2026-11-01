const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

// Test B2: Graph lanes (item 20)
// In node, build a graph with 8+ branches running at the same time, with forks and merges.
// Check that no two edges share a lane over overlapping rows.

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
        DAG_PALETTE: ['#3B82F6', '#10B981', '#F59E0B', '#EF4444', '#8B5CF6', '#EC4899', '#06B6D4', '#84CC16'],
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

// Build a graph with 10 branches running concurrently with forks and merges
const objects = {};
const branches = {};

// Root commit
objects['root'] = {
    type: 'commit',
    data: { message: 'root commit', timestamp: '2026-01-01T00:00:00Z', parent: null }
};

// 10 concurrent branches forking from root
const NUM_BRANCHES = 10;
const branchTips = [];

for (let b = 1; b <= NUM_BRANCHES; ++b) {
    const bName = `branch_${b}`;
    let prev = 'root';
    for (let step = 1; step <= 3; ++step) {
        const hash = `c_b${b}_s${step}`;
        const timeSec = 1000 + b * 100 + step * 10;
        objects[hash] = {
            type: 'commit',
            data: {
                message: `${bName} step ${step}`,
                timestamp: new Date(timeSec * 1000).toISOString(),
                parent: prev
            }
        };
        prev = hash;
    }
    branches[bName] = prev;
    branchTips.push(prev);
}

// Perform pairwise merges between adjacent branches:
// branch 2 merges into branch 1
// branch 4 merges into branch 3
// branch 6 merges into branch 5
// branch 8 merges into branch 7
// branch 10 merges into branch 9
const mergedTips = [];
for (let b = 1; b <= NUM_BRANCHES; b += 2) {
    const mHash = `merge_${b}_${b+1}`;
    objects[mHash] = {
        type: 'commit',
        data: {
            message: `Merge branch ${b+1} into branch ${b}`,
            timestamp: new Date(2500000000 + b * 1000).toISOString(),
            parent: branchTips[b - 1],
            parent2: branchTips[b]
        }
    };
    branches[`branch_${b}`] = mHash;
    mergedTips.push(mHash);
}

// Final top-level merge combining all branches into main
let currentHead = mergedTips[0];
for (let i = 1; i < mergedTips.length; ++i) {
    const topHash = `top_merge_${i}`;
    objects[topHash] = {
        type: 'commit',
        data: {
            message: `Top merge ${i}`,
            timestamp: new Date(3000000000 + i * 1000).toISOString(),
            parent: currentHead,
            parent2: mergedTips[i]
        }
    };
    currentHead = topHash;
}
branches['main'] = currentHead;

const env = createEnv();
setRepoState(env, objects, branches, 'main');

const graphRes = vm.runInContext('getGraphCommits(true)', env);
const commits = graphRes.commits;
assert(commits && commits.length > 0, "Graph commits must not be empty");

console.log(`Rendered graph with ${commits.length} commits and ${NUM_BRANCHES} branches.`);

// Map commit hash to row index
const rowIndex = new Map();
commits.forEach((c, idx) => {
    rowIndex.set(c.hash, idx);
});

// Extract all edges and their row spans and assigned lanes
// Each edge between child and parent occupies:
// - If child.lane === parent.lane: that lane over (minRow, maxRow)
// - If child.lane !== parent.lane: child.lane over [minRow, maxRow - 1] and parent.lane over [minRow + 1, maxRow]
const laneSegments = []; // { lane, rStart, rEnd, child: hash, parent: hash }

for (const node of commits) {
    const rChild = rowIndex.get(node.hash);

    const checkEdge = (pHash, isParent2) => {
        if (!pHash || !rowIndex.has(pHash)) return;
        const pNode = commits[rowIndex.get(pHash)];
        const rParent = rowIndex.get(pHash);
        const minR = Math.min(rChild, rParent);
        const maxR = Math.max(rChild, rParent);

        if (node.lane === pNode.lane) {
            laneSegments.push({
                lane: node.lane,
                rStart: minR,
                rEnd: maxR,
                child: node.hash,
                parent: pHash
            });
        } else {
            // Fork or merge curve:
            // For first parent: vertical spine runs in node.lane from minR to maxR - 1
            // For second parent: vertical spine runs in pNode.lane from minR + 1 to maxR
            if (!isParent2) {
                if (maxR - 1 > minR) {
                    laneSegments.push({
                        lane: node.lane,
                        rStart: minR,
                        rEnd: maxR - 1,
                        child: node.hash,
                        parent: pHash
                    });
                }
            } else {
                if (maxR > minR + 1) {
                    laneSegments.push({
                        lane: pNode.lane,
                        rStart: minR + 1,
                        rEnd: maxR,
                        child: node.hash,
                        parent: pHash
                    });
                }
            }
        }
    };

    checkEdge(node.parent, false);
    checkEdge(node.parent2, true);
}

// Verify that no two edges share a lane over overlapping rows
// Overlap condition for two open intervals (a, b) and (c, d): max(a, c) < min(b, d)
let conflictCount = 0;
for (let i = 0; i < laneSegments.length; ++i) {
    for (let j = i + 1; j < laneSegments.length; ++j) {
        const s1 = laneSegments[i];
        const s2 = laneSegments[j];

        if (s1.lane === s2.lane) {
            // Check row overlap
            const overlapStart = Math.max(s1.rStart, s2.rStart);
            const overlapEnd = Math.min(s1.rEnd, s2.rEnd);

            if (overlapStart < overlapEnd) {
                // Same edge can't conflict with itself
                if (s1.child === s2.child && s1.parent === s2.parent) continue;

                console.error(`Conflict detected in lane ${s1.lane} between rows ${overlapStart} and ${overlapEnd}:`);
                console.error(`  Edge 1: ${s1.child} -> ${s1.parent} (rows ${s1.rStart}..${s1.rEnd})`);
                console.error(`  Edge 2: ${s2.child} -> ${s2.parent} (rows ${s2.rStart}..${s2.rEnd})`);
                conflictCount++;
            }
        }
    }
}

assert.strictEqual(conflictCount, 0, `Expected 0 lane conflicts over overlapping rows, found ${conflictCount}`);
console.log(`Passed: Verified ${laneSegments.length} edge segments across 8+ concurrent branches with 0 lane collisions.`);
