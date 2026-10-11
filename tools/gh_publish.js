#!/usr/bin/env node
/*
 * Publish the resource-island working tree to GitHub in a single commit,
 * using only the GitHub REST API (no local git needed).
 *
 * Why: this machine has no git.exe, and Windows schannel (curl.exe / .NET) is
 * unavailable in this session, so HTTPS has to go through Node's bundled
 * OpenSSL. The Git Data API then builds blobs -> tree -> commit -> ref update.
 *
 * Auth: read a token from the GH_TOKEN environment variable. The token is
 * never written to disk or printed.
 *
 * Usage (from the project root):
 *   set GH_TOKEN=...        (PowerShell: $env:GH_TOKEN = '...')
 *   node tools/gh_publish.js [--dry-run]
 */

const fs = require('fs');
const path = require('path');

const REPO = process.env.GH_REPO || 'Machinezx/-pc-status-dynamic-island';
const BRANCH = process.env.GH_BRANCH || 'main';
const ROOT = process.env.GH_ROOT || path.resolve(__dirname, '..');
const DRY = process.argv.includes('--dry-run');
const TOKEN = process.env.GH_TOKEN || '';

// What goes into the repository: the clean buildable source tree.
// Deliberately excluded: build/ (artifacts), ref/ (third-party source snapshots),
// probe/ (development-only probes), and any compiler output anywhere.
const INCLUDE_ROOTS = [
  'build.bat',
  'CMakeLists.txt',
  '.gitignore',
  'README.md',
  'shot-strip-final.png',
  'src',
  'tools/make_avatar.py',
];
const SKIP_DIRS = new Set(['build', 'build_md', 'ref', 'probe', '.git', 'out', '__pycache__', '.vs']);
const SKIP_EXT = new Set(['.obj', '.exe', '.pdb', '.ilk', '.exp', '.lib', '.bak', '.png.tmp']);
const API = 'https://api.github.com';

function collect() {
  const out = [];
  const walk = (abs, rel, inRoot) => {
    const st = fs.statSync(abs);
    if (st.isDirectory()) {
      if (!inRoot && SKIP_DIRS.has(path.basename(abs))) return;
      for (const name of fs.readdirSync(abs).sort()) walk(path.join(abs, name), rel ? `${rel}/${name}` : name, false);
      return;
    }
    if (!inRoot) {
      if (SKIP_EXT.has(path.extname(abs).toLowerCase())) return;
      if (path.basename(abs).startsWith('.') && rel !== '.gitignore') return; // stray dotfiles
    }
    out.push({ abs, rel });
  };
  for (const entry of INCLUDE_ROOTS) {
    const abs = path.join(ROOT, entry);
    if (!fs.existsSync(abs)) { console.log(`  ! missing (skipped): ${entry}`); continue; }
    walk(abs, entry, true);
  }
  return out;
}

async function api(method, endpoint, body) {
  const res = await fetch(`${API}${endpoint}`, {
    method,
    signal: AbortSignal.timeout(60000),
    headers: {
      'User-Agent': 'resource-island-publisher',
      Accept: 'application/vnd.github+json',
      ...(TOKEN ? { Authorization: `Bearer ${TOKEN}` } : {}),
      ...(body ? { 'Content-Type': 'application/json' } : {}),
    },
    ...(body ? { body: JSON.stringify(body) } : {}),
  });
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch { /* non-JSON */ }
  if (!res.ok) {
    const msg = (json && (json.message || JSON.stringify(json))) || text.slice(0, 400);
    throw new Error(`${method} ${endpoint} -> ${res.status} ${msg}`);
  }
  return json;
}

(async () => {
  console.log(`repo   ${REPO}  branch ${BRANCH}`);
  console.log(`root   ${ROOT}`);
  console.log(`token  ${TOKEN ? `${TOKEN.length} chars, present` : 'NOT SET'}`);

  const files = collect();
  console.log(`\n--- ${files.length} files to publish ---`);
  let bytes = 0;
  for (const f of files) { const n = fs.statSync(f.abs).size; bytes += n; console.log(`  ${String(n).padStart(9)}  ${f.rel}`); }
  console.log(`  total ${bytes} bytes`);
  if (DRY) { console.log('\n--dry-run: stopping before any write'); return; }
  if (!TOKEN) { console.error('\nGH_TOKEN is not set.'); process.exit(2); }

  const me = await api('GET', '/user');
  console.log(`\nauthenticated as ${me.login}`);
  const repoInfo = await api('GET', `/repos/${REPO}`);
  console.log(`repo access: ${repoInfo.full_name} (permissions: ${JSON.stringify(repoInfo.permissions || null)})`);

  const head = await api('GET', `/repos/${REPO}/git/ref/heads/${BRANCH}`);
  const parentSha = head.object.sha;
  const parent = await api('GET', `/repos/${REPO}/git/commits/${parentSha}`);
  console.log(`parent commit ${parentSha.slice(0, 12)}  tree ${parent.tree.sha.slice(0, 12)}`);

  console.log('\n--- uploading blobs ---');
  const tree = [];
  for (const f of files) {
    const content = fs.readFileSync(f.abs).toString('base64');
    const blob = await api('POST', `/repos/${REPO}/git/blobs`, { content, encoding: 'base64' });
    tree.push({ path: f.rel, mode: '100644', type: 'blob', sha: blob.sha });
    console.log(`  ok ${f.rel}  ${blob.sha.slice(0, 10)}`);
  }

  console.log('\n--- creating tree ---');
  const newTree = await api('POST', `/repos/${REPO}/git/trees`, { base_tree: parent.tree.sha, tree });
  console.log(`  tree ${newTree.sha}`);

  console.log('\n--- creating commit ---');
  const commit = await api('POST', `/repos/${REPO}/git/commits`, {
    message: 'Add full source tree: Win32 + C++20 PC status dynamic island\n\n'
      + 'CPU/memory/GPU/FPS sampling, edge docking with hover peek, layered-window\n'
      + 'rendering (capsule / strip / morph), dsh-pet-compatible config section,\n'
      + 'dynamically loaded NVML, RTSS shared-memory reader, baked avatar bitmap.\n'
      + 'See README.md for building and usage.',
    tree: newTree.sha,
    parents: [parentSha],
  });
  console.log(`  commit ${commit.sha}`);

  console.log('\n--- moving ref ---');
  await api('PATCH', `/repos/${REPO}/git/refs/heads/${BRANCH}`, { sha: commit.sha, force: false });

  const verify = await api('GET', `/repos/${REPO}/git/ref/heads/${BRANCH}`);
  console.log(`  ${BRANCH} is now ${verify.object.sha}`);
  console.log(`\nDONE  https://github.com/${REPO}/commit/${commit.sha}`);
})().catch((e) => { console.error(`\nFAILED: ${e.message}`); process.exit(1); });
