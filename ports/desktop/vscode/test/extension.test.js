// node --test test/extension.test.js: the extension against a fake vscode module.
const test = require('node:test');
const assert = require('node:assert');
const Module = require('module');

function fakeVscode() {
    const registered = {};
    const store = new Map();
    const fake = {
        picks: [],                  // answers for showQuickPick, in order
        shownPicks: [],
        errors: [],
        started: [],
        status: { show() { this.shown = true; } },
        window: {
            createStatusBarItem: () => fake.status,
            showQuickPick: async (items) => { fake.shownPicks.push(items); const want = fake.picks.shift(); return items.find((i) => i.label === want); },
            showErrorMessage: (m) => fake.errors.push(m),
            showInformationMessage: (m) => { (fake.infos = fake.infos || []).push(m); },
            showWarningMessage: (m) => { (fake.warnings = fake.warnings || []).push(m); },
            showInputBox: async () => fake.input,
            withProgress: async (options, task) => task({ report() {} }),
            onDidCloseTerminal: (f) => { registered.closeTerminal = f; return {}; },
            createOutputChannel: (name) => {
                fake.channel = { name, text: '', shown: 0, append(t) { this.text += t; }, appendLine(t) { this.text += t + '\n'; }, show() { this.shown++; }, dispose() {} };
                return fake.channel;
            },
            createTerminal: (opts) => { fake.terminal = Object.assign({ show() {}, exitStatus: undefined }, opts); return fake.terminal; },
            registerTerminalLinkProvider: (p) => { registered.links = p; return {}; },
            showTextDocument: async (doc, options) => { fake.opened = { path: doc.uri.fsPath, line: options.selection.start.line }; },
        },
        languages: {
            createDiagnosticCollection: (name) => {
                fake.problems = { name, entries: new Map(), set(uri, list) { this.entries.set(uri.fsPath, list); }, clear() { this.entries.clear(); }, dispose() {} };
                return fake.problems;
            },
        },
        Uri: { file: (p) => ({ fsPath: p }) },
        Position: function (line, character) { this.line = line; this.character = character; },
        Range: function (a, b, c, d) { this.start = { line: a, character: b }; this.end = { line: c, character: d }; },
        Selection: function (a, b, c, d) { this.start = { line: a, character: b }; this.end = { line: c, character: d }; },
        Diagnostic: function (range, message, severity) { this.range = range; this.message = message; this.severity = severity; },
        DiagnosticSeverity: { Error: 0 },
        workspace: {
            getConfiguration: () => ({ get: (k, d) => (k === 'program' ? '' : d) }),
            workspaceFolders: [{ uri: { fsPath: '/w' } }],
            openTextDocument: async (uri) => ({ uri }),
        },
        commands: { registerCommand: (n, f) => { registered[n] = f; return {}; } },
        debug: {
            activeDebugSession: undefined,
            registerDebugConfigurationProvider: (t, p) => { registered.provider = p; return {}; },
            registerDebugAdapterDescriptorFactory: (t, f) => { registered.factory = f; return {}; },
            registerDebugAdapterTrackerFactory: (t, f) => { registered.tracker = f; return {}; },
            onDidTerminateDebugSession: (f) => { registered.terminate = f; return {}; },
            startDebugging: async (folder, config) => { fake.started.push(config); return true; },
        },
        StatusBarAlignment: { Left: 1 },
        ProgressLocation: { Notification: 15 },
        DebugAdapterServer: function (port, host) { this.port = port; this.host = host; },
        registered,
        context: { subscriptions: [], workspaceState: { get: (k) => store.get(k), update: async (k, v) => store.set(k, v) } },
    };
    return fake;
}

function load(fake) {
    const orig = Module._load;
    Module._load = function (request, ...rest) {
        if (request === 'vscode') {
            return fake;
        }
        if (request === './libinstall' && fake.libinstall) {
            return fake.libinstall;
        }
        return orig.call(this, request, ...rest);
    };
    delete require.cache[require.resolve('../extension.js')];
    const ext = require('../extension.js');
    Module._load = orig;
    ext.activate(fake.context);
    return ext;
}

test('a board in launch.json is used as it is', async () => {
    const fake = fakeVscode(); load(fake);
    const config = await resolve(fake, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'mcp2221' });
    assert.strictEqual(config.board, 'mcp2221');
    assert.strictEqual(fake.shownPicks.length, 0);
});

test('ask: the first launch asks, the next ones reuse the choice, the status bar shows it', async () => {
    const fake = fakeVscode(); load(fake);
    assert.match(fake.status.text, /virtual/);
    fake.picks.push('ft232h');
    const first = await resolve(fake, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'ask' });
    assert.strictEqual(first.board, 'ft232h');
    assert.match(fake.status.text, /ft232h/);
    const second = await resolve(fake, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'ask' });
    assert.strictEqual(second.board, 'ft232h');
    assert.strictEqual(fake.shownPicks.length, 1);
});

test('a remembered board the program no longer has (desktop, linux) is asked again', async () => {
    const fake = fakeVscode();
    await fake.context.workspaceState.update('circuitpython.board', 'desktop');
    load(fake);
    assert.match(fake.status.text, /virtual/);
    fake.picks.push('virtual');
    const config = await resolve(fake, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'ask' });
    assert.strictEqual(config.board, 'virtual');
    assert.strictEqual(fake.shownPicks.length, 1);
});

test('the command changes the board; a dismissed pick cancels the launch', async () => {
    const fake = fakeVscode(); load(fake);
    fake.picks.push('mcp2221');
    await fake.registered['circuitpython.selectBoard']();
    assert.match(fake.status.text, /mcp2221/);
    const fake2 = fakeVscode(); load(fake2);
    fake2.picks.push(undefined);
    const config = await resolve(fake2, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'ask' });
    assert.strictEqual(config, undefined);
});

test('an unknown board is an error, no launch', async () => {
    const fake = fakeVscode(); load(fake);
    const config = await resolve(fake, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'pico' });
    assert.strictEqual(config, undefined);
    assert.match(fake.errors[0], /unknown board "pico"/);
});

test('the terminal gets --board and the board in its name', async () => {
    const fake = fakeVscode(); load(fake);
    const net = require('net');
    const server = net.createServer((s) => s.destroy()).listen(0, '127.0.0.1');
    await new Promise((r) => server.once('listening', r));
    const port = server.address().port;
    const descriptor = await fake.registered.factory.createDebugAdapterDescriptor({ configuration: { request: 'launch', program: '/x/cp', board: 'ft232h', port, drive: '/w', traceBus: false } });
    server.close();
    assert.deepStrictEqual(fake.terminal.shellArgs, ['--drive', '/w', '--debug', String(port), '--board', 'ft232h']);
    assert.strictEqual(fake.terminal.name, 'CircuitPython (ft232h)');
    assert.strictEqual(descriptor.port, port);
});

test('reconnect: after the program ended yes, after Stop no', async () => {
    const fake = fakeVscode(); load(fake);
    const net = require('net');
    const server = net.createServer((s) => s.destroy()).listen(0, '127.0.0.1');
    await new Promise((r) => server.once('listening', r));
    const port = server.address().port;
    const config = { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'virtual', port, drive: '/w' };
    await fake.registered.factory.createDebugAdapterDescriptor({ configuration: config });

    const ended = { id: 'a', type: 'circuitpython', configuration: config };
    const t1 = fake.registered.tracker.createDebugAdapterTracker(ended);
    t1.onDidSendMessage({ type: 'event', event: 'terminated' });
    t1.onWillReceiveMessage({ type: 'request', command: 'disconnect' });
    await fake.registered.terminate(ended);
    assert.strictEqual(fake.started.length, 1);

    const stopped = { id: 'b', type: 'circuitpython', configuration: config };
    const t2 = fake.registered.tracker.createDebugAdapterTracker(stopped);
    t2.onWillReceiveMessage({ type: 'request', command: 'terminate' });
    await fake.registered.terminate(stopped);
    assert.strictEqual(fake.started.length, 1);
    server.close();
});

// VS Code calls both hooks; the board is chosen once the program's path has its variables.
async function resolve(fake, config) {
    const provider = fake.registered.provider;
    const first = await provider.resolveDebugConfiguration(undefined, config);
    return first && provider.resolveDebugConfigurationWithSubstitutedVariables(undefined, first);
}

test('ask: a board chosen in the status bar is used without asking', async () => {
    const fake = fakeVscode(); load(fake);
    fake.picks.push('mcp2221');
    await fake.registered['circuitpython.selectBoard']();
    const config = await resolve(fake, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'ask' });
    assert.strictEqual(config.board, 'mcp2221');
    assert.strictEqual(fake.shownPicks.length, 1);
});

test('the board is chosen after variables are substituted, not before', async () => {
    const fake = fakeVscode(); load(fake);
    const raw = { type: 'circuitpython', request: 'launch', program: '${workspaceFolder}/cp', board: 'ask' };
    const first = await fake.registered.provider.resolveDebugConfiguration(undefined, raw);
    assert.strictEqual(fake.shownPicks.length, 0);
    fake.picks.push('virtual');
    const second = await fake.registered.provider.resolveDebugConfigurationWithSubstitutedVariables(undefined, Object.assign({}, first, { program: '/w/cp' }));
    assert.strictEqual(second.board, 'virtual');
    assert.strictEqual(fake.shownPicks.length, 1);
});

test('reconnect: the ended session still being the active one does not cancel it', async () => {
    const fake = fakeVscode(); load(fake);
    const net = require('net');
    const server = net.createServer((s) => s.destroy()).listen(0, '127.0.0.1');
    await new Promise((r) => server.once('listening', r));
    const port = server.address().port;
    const config = { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'virtual', port, drive: '/w' };
    await fake.registered.factory.createDebugAdapterDescriptor({ configuration: config });
    const ended = { id: 'a', type: 'circuitpython', configuration: config };
    fake.debug.activeDebugSession = ended;
    const t = fake.registered.tracker.createDebugAdapterTracker(ended);
    t.onDidSendMessage({ type: 'event', event: 'terminated' });
    const pending = fake.registered.terminate(ended);
    setTimeout(() => { fake.debug.activeDebugSession = undefined; }, 300);
    await pending;
    assert.strictEqual(fake.started.length, 1);

    // Another kind of session (Python) running alongside does not cancel it either.
    fake.debug.activeDebugSession = { id: 'p', type: 'python' };
    const ended2 = { id: 'c', type: 'circuitpython', configuration: config };
    fake.registered.tracker.createDebugAdapterTracker(ended2).onDidSendMessage({ type: 'event', event: 'exited' });
    await fake.registered.terminate(ended2);
    assert.strictEqual(fake.started.length, 2);
    server.close();
});

test('the bus trace: the program writes a file, the panel shows its lines', async () => {
    const fake = fakeVscode(); load(fake);
    const fs = require('fs');
    const net = require('net');
    const server = net.createServer((s) => s.destroy()).listen(0, '127.0.0.1');
    await new Promise((r) => server.once('listening', r));
    const port = server.address().port;
    await fake.registered.factory.createDebugAdapterDescriptor({ configuration: { request: 'launch', program: '/x/cp', board: 'mcp2221', port, drive: '/w' } });
    server.close();
    const args = fake.terminal.shellArgs;
    const at = args.indexOf('--trace-bus');
    assert.ok(at > 0, 'the program gets --trace-bus');
    const file = args[at + 1];
    assert.strictEqual(fs.readFileSync(file, 'utf8'), '', 'an empty file to start');
    assert.strictEqual(fake.channel.name, 'CircuitPython bus');
    fs.appendFileSync(file, '    1.000 I2C 0x68 W 00 (no stop)\n');
    await new Promise((r) => setTimeout(r, 500));
    assert.match(fake.channel.text, /I2C 0x68 W 00 \(no stop\)/);
    assert.strictEqual(fake.channel.shown, 1, 'shown once, when the first line came');
    await fake.registered['circuitpython.showBusTrace']();
    assert.strictEqual(fake.channel.shown, 2);
    fake.registered.closeTerminal(fake.terminal);
});

test('an uncaught exception goes to the Problems panel; the next session clears it', async () => {
    const fake = fakeVscode(); load(fake);
    const session = { id: 'a', type: 'circuitpython', configuration: { request: 'launch' } };
    const t = fake.registered.tracker.createDebugAdapterTracker(session);
    t.onDidSendMessage({ type: 'event', event: 'circuitpython.uncaught', body: { path: '/w/lib/helper.py', line: 2, text: 'ZeroDivisionError: division by zero' } });
    const list = fake.problems.entries.get('/w/lib/helper.py');
    assert.strictEqual(list.length, 1);
    assert.strictEqual(list[0].message, 'ZeroDivisionError: division by zero');
    assert.strictEqual(list[0].range.start.line, 1);
    assert.strictEqual(list[0].severity, fake.DiagnosticSeverity.Error);
    fake.registered.tracker.createDebugAdapterTracker({ id: 'b', type: 'circuitpython', configuration: { request: 'launch' } });
    assert.strictEqual(fake.problems.entries.size, 0);
});

test('traceback lines in the CircuitPython terminal are links to the file on the drive', async () => {
    const fake = fakeVscode(); load(fake);
    const links = fake.registered.links;
    const terminal = { name: 'CircuitPython (mcp2221)' };
    const line = '  File "/lib/helper.py", line 2, in boom';
    const found = links.provideTerminalLinks({ line, terminal });
    assert.strictEqual(found.length, 1);
    assert.strictEqual(line.substr(found[0].startIndex, found[0].length), 'File "/lib/helper.py", line 2');
    await links.handleTerminalLink(found[0]);
    assert.deepStrictEqual(fake.opened, { path: require('path').join('/w', 'lib', 'helper.py'), line: 1 });
    const top = links.provideTerminalLinks({ line: '  File "code.py", line 3, in <module>', terminal });
    await links.handleTerminalLink(top[0]);
    assert.deepStrictEqual(fake.opened, { path: require('path').join('/w', 'code.py'), line: 2 });
    assert.deepStrictEqual(links.provideTerminalLinks({ line, terminal: { name: 'bash' } }), []);
});

test('the bus trace file is in a fresh private directory, not a name others can guess', async () => {
    const fake = fakeVscode(); load(fake);
    const fs = require('fs');
    const path = require('path');
    const net = require('net');
    const server = net.createServer((s) => s.destroy()).listen(0, '127.0.0.1');
    await new Promise((r) => server.once('listening', r));
    const port = server.address().port;
    const files = [];
    for (let i = 0; i < 2; i++) {
        await fake.registered.factory.createDebugAdapterDescriptor({ configuration: { request: 'launch', program: '/x/cp', board: 'mcp2221', port, drive: '/w' } });
        const args = fake.terminal.shellArgs;
        files.push(args[args.indexOf('--trace-bus') + 1]);
        fake.registered.closeTerminal(fake.terminal);
    }
    server.close();
    assert.notStrictEqual(path.dirname(files[0]), path.dirname(files[1]));
    assert.ok(!files[0].includes(String(port)), 'the port is not in the name');
    assert.ok(!fs.existsSync(path.dirname(files[0])), 'the directory goes with the terminal');
});

test('a port that is not a number is refused', async () => {
    const fake = fakeVscode(); load(fake);
    const config = await resolve(fake, { type: 'circuitpython', request: 'launch', program: '/x/cp', board: 'virtual', port: '../../x' });
    assert.strictEqual(config, undefined);
    assert.match(fake.errors[0], /port/);
});

function fakeInstaller(calls, result) {
    return {
        importsOfDrive: (drive) => { calls.push(['imports', drive]); return ['adafruit_bme280', 'adafruit_no_such']; },
        install: async (drive, names, progress, options) => { calls.push(['install', drive, names, options]); return result; },
    };
}

test('Install library: the name asked for goes into the drive of the workspace', async () => {
    const fake = fakeVscode();
    const calls = [];
    fake.libinstall = fakeInstaller(calls, { installed: [{ name: 'adafruit-circuitpython-bme280', version: '2.6.4', files: [] }, { name: 'adafruit-circuitpython-busdevice', version: '5.2.6', files: [] }], missing: [] });
    load(fake);
    fake.input = 'bme280';
    await fake.registered['circuitpython.installLibrary']();
    assert.deepStrictEqual(calls, [['install', '/w', ['bme280'], { trustNames: true }]]);
    assert.match(fake.infos[0], /adafruit-circuitpython-bme280 2\.6\.4/);
    assert.match(fake.infos[0], /busdevice/);
    fake.input = undefined;
    await fake.registered['circuitpython.installLibrary']();
    assert.strictEqual(calls.length, 1, 'a dismissed box installs nothing');
});

test('Install libraries used by code.py: its imports, and what PyPI does not have is named', async () => {
    const fake = fakeVscode();
    const calls = [];
    fake.libinstall = fakeInstaller(calls, { installed: [{ name: 'adafruit-circuitpython-bme280', version: '2.6.4', files: [] }], missing: ['adafruit-circuitpython-no-such'], refused: ['adafruit-circuitpython-squat'] });
    load(fake);
    await fake.registered['circuitpython.installFromCode']();
    assert.deepStrictEqual(calls, [['imports', '/w'], ['install', '/w', ['adafruit_bme280', 'adafruit_no_such'], { trustNames: false }]]);
    assert.match(fake.warnings[0], /adafruit-circuitpython-no-such/);
    assert.match(fake.warnings[1], /not published by Adafruit.*adafruit-circuitpython-squat/);
});
