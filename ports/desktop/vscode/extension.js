// The editor's side of the CircuitPython host debugger. The debug adapter itself runs inside
// CircuitPython (python/debugadapter.py); this starts the program in a terminal, points VS Code
// at the adapter's port, lets the user pick the board, and reconnects after the program reloads.
const vscode = require('vscode');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const { execFile } = require('child_process');
const boards = require('./boards');
const { FileTail } = require('./tail');
const libinstall = require('./libinstall');

const BOARD_KEY = 'circuitpython.board';

// Connect and hang up until the adapter answers; it treats a silent client as a probe.
function waitForPort(port, timeoutMs, cancelled) {
    return new Promise((resolve, reject) => {
        const started = Date.now();
        const attempt = () => {
            if (cancelled && cancelled()) {
                reject(new Error('cancelled'));
                return;
            }
            const socket = net.connect(port, '127.0.0.1');
            socket.once('connect', () => { socket.destroy(); resolve(); });
            socket.once('error', () => {
                socket.destroy();
                if (timeoutMs > 0 && Date.now() - started > timeoutMs) {
                    reject(new Error('CircuitPython did not open port ' + port));
                } else {
                    setTimeout(attempt, 250);
                }
            });
        };
        attempt();
    });
}

// The program started by the last launch. After a reload (a file saved on the drive) it waits
// for the editor again, so the next F5 connects to it instead of starting a second one.
let running = null;
let runningConfig = null;
let state = null;           // the workspace's memory: the chosen board
let statusItem = null;
let lastProgram = null;     // the program of the last launch, for --list-boards
let busChannel = null;      // the "CircuitPython bus" panel
let busTail = null;         // following the running program's --trace-bus file
let busDir = null;          // the directory that file is in
let problems = null;        // the Problems panel's entries: an exception that ended code.py

function isRunning() {
    return running !== null && running.exitStatus === undefined;
}

function settings() {
    return vscode.workspace.getConfiguration('circuitpython');
}

function programPath() {
    return settings().get('program') || lastProgram;
}

// `program --list-boards`: which boards there are and which adapters are plugged in.
function listBoards(program) {
    return new Promise((resolve) => {
        if (!program) {
            resolve(null);
            return;
        }
        execFile(program, ['--list-boards'], { timeout: 5000, windowsHide: true }, (error, stdout) => {
            const list = boards.parseListBoards(stdout || '');
            resolve(error && list.length === 0 ? null : list);
        });
    });
}

// A name saved before the boards were renamed (desktop, linux) counts as none: asked again.
function rememberedBoard() {
    const board = state ? state.get(BOARD_KEY) : undefined;
    return boards.BOARDS.includes(board) ? board : undefined;
}

function updateStatus() {
    if (!statusItem) {
        return;
    }
    const board = rememberedBoard() || 'virtual';
    statusItem.text = '$(circuit-board) ' + board;
    statusItem.tooltip = 'CircuitPython board for the next debug run (' + (boards.DESCRIPTIONS[board] || board) + '). Click to change.';
    statusItem.show();
}

// The quick pick of boards, the plugged-in adapters marked when the program can tell.
async function pickBoard() {
    const list = await listBoards(programPath());
    const current = rememberedBoard();
    const items = boards.BOARDS.map((name) => {
        const entry = list && list.find((b) => b.name === name);
        let detail = boards.DESCRIPTIONS[name];
        if (entry && !entry.always) {
            detail = (entry.present ? '$(plug) connected · ' : 'not connected · ') + detail;
        }
        return { label: name, description: name === current ? 'current' : '', detail };
    });
    const choice = await vscode.window.showQuickPick(items, {
        placeHolder: list ? 'The board CircuitPython runs as' : 'The board CircuitPython runs as (set circuitpython.program to see which adapters are plugged in)',
    });
    if (!choice) {
        return undefined;
    }
    await state.update(BOARD_KEY, choice.label);
    updateStatus();
    return choice.label;
}

// The board for a launch: the configuration's, else for "auto" the one plugged-in adapter, else
// (for "ask") the remembered choice, else asked.
async function resolveBoard(config) {
    const fixed = boards.configuredBoard(config);
    if (fixed) {
        return fixed;
    }
    if (config.board === 'auto') {
        const found = boards.chooseAuto((await listBoards(config.program)) || []);
        if (found) {
            return found;
        }
    }
    if (config.board !== 'auto' && rememberedBoard()) {
        return rememberedBoard();
    }
    return pickBoard();
}

// The board is resolved once VS Code has substituted ${workspaceFolder} in the program's path.
class Provider {
    resolveDebugConfiguration(folder, config) {
        return config;
    }

    async resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
        if (config.port !== undefined && !(Number.isInteger(config.port) && config.port > 0 && config.port < 65536)) {
            vscode.window.showErrorMessage('CircuitPython: "port" must be a number from 1 to 65535');
            return undefined;
        }
        if (config.request !== 'launch') {
            return config;
        }
        if (config.program) {
            lastProgram = config.program;
        }
        // A program already running keeps the board it was started with.
        if (isRunning()) {
            return config;
        }
        let board;
        try {
            board = await resolveBoard(config);
        } catch (e) {
            vscode.window.showErrorMessage(e.message);
            return undefined;
        }
        if (board === undefined) {
            return undefined;       // the pick was dismissed: no launch
        }
        return Object.assign({}, config, { board });
    }
}

// The running program's bus trace into the panel, shown (without taking focus) at its first line.
// The file is new, in a directory of its own that only this user can enter: a fixed name in a
// shared temp directory could be a link another user placed there. Returns the file.
function followTrace() {
    stopTrace();
    busDir = fs.mkdtempSync(path.join(os.tmpdir(), 'circuitpython-bus-'));
    const file = path.join(busDir, 'bus.txt');
    fs.writeFileSync(file, '', { flag: 'wx', mode: 0o600 });
    let shown = false;
    busChannel.appendLine('--- ' + new Date().toLocaleTimeString() + ' ---');
    busTail = new FileTail(file, (text) => {
        busChannel.append(text);
        if (!shown) {
            shown = true;
            busChannel.show(true);
        }
    });
    busTail.start();
    return file;
}

function stopTrace() {
    if (busTail !== null) {
        busTail.stop();
        busTail = null;
    }
    if (busDir !== null) {
        try {
            fs.rmSync(busDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
        } catch (e) {
            // The program still holds it open (Windows): the temp directory keeps it.
        }
        busDir = null;
    }
}

class Factory {
    async createDebugAdapterDescriptor(session) {
        const config = session.configuration;
        const port = config.port || 5678;
        if (config.request === 'launch') {
            if (!isRunning()) {
                const folders = vscode.workspace.workspaceFolders;
                const drive = config.drive || (folders && folders[0].uri.fsPath) || '.';
                let tracePath = null;
                if (boards.traceWanted(config, config.board)) {
                    tracePath = followTrace();
                }
                // The program is the terminal's shell, so its console is the terminal and no
                // quoting of paths is needed on any system.
                running = vscode.window.createTerminal({
                    name: 'CircuitPython' + (config.board && config.board !== 'virtual' ? ' (' + config.board + ')' : ''),
                    shellPath: config.program,
                    shellArgs: boards.buildArgs(config, config.board, drive, port, tracePath),
                });
                runningConfig = config;
            }
            running.show(true);
            await waitForPort(port, 30000);
        }
        return new vscode.DebugAdapterServer(port, '127.0.0.1');
    }
}

// Watches each session's messages to tell a program that ended (reconnect when it waits again)
// from a user who pressed Stop (leave it).
const watches = new Map();

class TrackerFactory {
    createDebugAdapterTracker(session) {
        const watch = new boards.SessionWatch();
        watches.set(session.id, watch);
        problems.clear();
        return {
            onDidSendMessage: (message) => {
                watch.fromAdapter(message);
                if (message.type === 'event' && message.event === 'circuitpython.uncaught') {
                    showUncaught(message.body);
                }
            },
            onWillReceiveMessage: (message) => watch.toAdapter(message),
        };
    }
}

// The exception that ended code.py, where it was raised, in the Problems panel.
function showUncaught(body) {
    if (!body || typeof body.path !== 'string' || !Number.isInteger(body.line)) {
        return;
    }
    const line = Math.max(body.line - 1, 0);
    const diagnostic = new vscode.Diagnostic(new vscode.Range(line, 0, line, 1000), String(body.text), vscode.DiagnosticSeverity.Error);
    diagnostic.source = 'CircuitPython';
    problems.set(vscode.Uri.file(body.path), [diagnostic]);
}

// The drive of the running program, else of the workspace: where traceback paths are.
function currentDrive() {
    if (runningConfig && runningConfig.drive) {
        return runningConfig.drive;
    }
    const folders = vscode.workspace.workspaceFolders;
    return folders && folders.length > 0 ? folders[0].uri.fsPath : null;
}

// Libraries from PyPI into the drive's lib/, with a notification while it runs and a summary.
// typed: the names are the user's own, so their publisher need not be Adafruit.
async function installInto(names, typed) {
    const drive = currentDrive();
    if (!drive) {
        vscode.window.showErrorMessage('CircuitPython: open the CIRCUITPY folder first');
        return;
    }
    if (names.length === 0) {
        vscode.window.showInformationMessage('CircuitPython: code.py imports no library');
        return;
    }
    let result;
    try {
        result = await vscode.window.withProgress(
            { location: vscode.ProgressLocation.Notification, title: 'CircuitPython: installing' },
            (progress) => libinstall.install(drive, names, (name) => progress.report({ message: name }), { trustNames: typed }));
    } catch (e) {
        vscode.window.showErrorMessage('CircuitPython: ' + e.message);
        return;
    }
    if (result.installed.length > 0) {
        vscode.window.showInformationMessage('CircuitPython: installed into lib/: ' +
            result.installed.map((p) => p.name + ' ' + p.version).join(', '));
    }
    if (result.missing.length > 0) {
        vscode.window.showWarningMessage('CircuitPython: not on PyPI as a CircuitPython library: ' + result.missing.join(', '));
    }
    if (result.refused && result.refused.length > 0) {
        vscode.window.showWarningMessage('CircuitPython: not published by Adafruit, so not installed: ' + result.refused.join(', ')
            + '. Install library... takes a name you trust from anyone.');
    }
}

async function installLibrary() {
    const name = await vscode.window.showInputBox({
        prompt: 'A CircuitPython library: bme280, adafruit_bme280 or adafruit-circuitpython-bme280',
        placeHolder: 'bme280',
    });
    if (name && name.trim()) {
        await installInto([name.trim()], true);
    }
}

async function installFromCode() {
    const drive = currentDrive();
    await installInto(drive ? libinstall.importsOfDrive(drive) : [], false);
}

// `File "code.py", line 3` and `File "/lib/x.py", line 2` in a CircuitPython terminal open that
// file at that line.
const TRACEBACK_LINE = /File "([^"]+)", line (\d+)/g;

class TracebackLinks {
    provideTerminalLinks(context) {
        if (!context.terminal.name.startsWith('CircuitPython')) {
            return [];
        }
        const links = [];
        for (const match of context.line.matchAll(TRACEBACK_LINE)) {
            links.push({
                startIndex: match.index, length: match[0].length, tooltip: 'Open ' + match[1],
                file: match[1], line: parseInt(match[2], 10),
            });
        }
        return links;
    }

    async handleTerminalLink(link) {
        // CircuitPython names files from the drive's root; a path leading out of it opens nothing.
        const drive = currentDrive();
        if (!drive) {
            return;
        }
        const file = path.join(drive, link.file.replace(/^[/\\]+/, ''));
        if (path.relative(drive, file).startsWith('..')) {
            return;
        }
        const line = Math.max(link.line - 1, 0);
        const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(file));
        await vscode.window.showTextDocument(doc, { selection: new vscode.Selection(line, 0, line, 0) });
    }
}

let reconnecting = false;

// A CircuitPython session other than the one that ended; VS Code may still report that one as
// active for a moment.
function anotherSession(ended) {
    const active = vscode.debug.activeDebugSession;
    return active !== undefined && active.type === 'circuitpython' && active.id !== ended.id;
}

async function reconnectLater(session) {
    const watch = watches.get(session.id);
    watches.delete(session.id);
    const config = session.configuration;
    if (!watch || config.request !== 'launch' || reconnecting
        || !watch.shouldReconnect(settings().get('reconnectAfterReload', true))) {
        return;
    }
    const terminal = running;
    if (terminal === null || terminal.exitStatus !== undefined) {
        return;
    }
    reconnecting = true;
    try {
        // The program listens again after its next reload; until then this waits quietly.
        await waitForPort(config.port || 5678, 0,
            () => running !== terminal || terminal.exitStatus !== undefined || anotherSession(session));
        await vscode.debug.startDebugging(session.workspaceFolder, runningConfig || config);
    } catch (e) {
        // The terminal closed or the user started a session: nothing to do.
    } finally {
        reconnecting = false;
    }
}

function activate(context) {
    state = context.workspaceState;
    statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 50);
    statusItem.command = 'circuitpython.selectBoard';
    context.subscriptions.push(statusItem);
    busChannel = vscode.window.createOutputChannel('CircuitPython bus');
    problems = vscode.languages.createDiagnosticCollection('CircuitPython');
    context.subscriptions.push(busChannel, problems,
        vscode.window.registerTerminalLinkProvider(new TracebackLinks()));
    updateStatus();

    context.subscriptions.push(
        vscode.commands.registerCommand('circuitpython.selectBoard', pickBoard),
        vscode.commands.registerCommand('circuitpython.showBusTrace', () => busChannel.show()),
        vscode.commands.registerCommand('circuitpython.installLibrary', installLibrary),
        vscode.commands.registerCommand('circuitpython.installFromCode', installFromCode),
        vscode.debug.registerDebugConfigurationProvider('circuitpython', new Provider()),
        vscode.debug.registerDebugAdapterDescriptorFactory('circuitpython', new Factory()),
        vscode.debug.registerDebugAdapterTrackerFactory('circuitpython', new TrackerFactory()),
        vscode.debug.onDidTerminateDebugSession((session) => {
            return session.type === 'circuitpython' ? reconnectLater(session) : undefined;
        }),
        vscode.window.onDidCloseTerminal((terminal) => {
            if (terminal === running) {
                running = null;
                runningConfig = null;
                stopTrace();
            }
        }));
}

function deactivate() {}

module.exports = { activate, deactivate };
