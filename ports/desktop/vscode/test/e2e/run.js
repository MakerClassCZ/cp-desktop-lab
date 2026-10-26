// node test/e2e/run.js PROGRAM: the extension in a real VS Code (the `code` on PATH, or $VSCODE),
// with a fresh profile, against PROGRAM on the virtual board. Exits with VS Code's status.
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const program = process.argv[2];
if (!program || !fs.existsSync(program)) {
    console.error('usage: node test/e2e/run.js PATH_TO_CIRCUITPYTHON');
    process.exit(2);
}
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'cp-e2e-'));
const drive = path.join(root, 'CIRCUITPY');
fs.mkdirSync(drive);
fs.writeFileSync(path.join(drive, 'code.py'), 'x = 41\nx += 1\nprint("e2e", x)\n');

const code = process.env.VSCODE || 'code';
const result = spawnSync(code, [
    '--extensionDevelopmentPath=' + path.resolve(__dirname, '..', '..'),
    '--extensionTestsPath=' + path.resolve(__dirname, 'suite.js'),
    '--user-data-dir=' + path.join(root, 'user'),
    '--extensions-dir=' + path.join(root, 'extensions'),
    '--disable-workspace-trust',
    '--skip-welcome', '--skip-release-notes',
    drive,
], {
    stdio: 'inherit',
    shell: process.platform === 'win32' && !code.toLowerCase().endsWith('.exe'),
    env: Object.assign({}, process.env, { CP_E2E_PROGRAM: path.resolve(program), CP_E2E_DRIVE: drive }),
});
fs.rmSync(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 500 });
process.exit(result.status === null ? 1 : result.status);
