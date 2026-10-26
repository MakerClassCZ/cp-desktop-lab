// Runs inside VS Code (--extensionTestsPath): a breakpoint, a variable, a reconnect after a saved
// file reloads the program, and no reconnect after Stop.
const vscode = require('vscode');
const assert = require('assert');
const fs = require('fs');
const path = require('path');

const program = process.env.CP_E2E_PROGRAM;
const drive = process.env.CP_E2E_DRIVE;
const port = 5799;

const events = [];          // [sessionId, event name, body] from every circuitpython session
vscode.debug.registerDebugAdapterTrackerFactory('circuitpython', {
    createDebugAdapterTracker: (session) => ({
        onDidSendMessage: (m) => {
            if (m.type === 'event') {
                events.push([session.id, m.event, m.body]);
            }
        },
    }),
});

function sleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}

async function until(what, test, timeoutMs) {
    const end = Date.now() + timeoutMs;
    for (;;) {
        const value = test();
        if (value) {
            return value;
        }
        if (Date.now() > end) {
            throw new Error('timed out waiting for ' + what);
        }
        await sleep(100);
    }
}

function stoppedIn(sessionId) {
    return events.find((e) => e[0] === sessionId && e[1] === 'stopped');
}

async function topFrame(session, threadId) {
    const trace = await session.customRequest('stackTrace', { threadId, startFrame: 0, levels: 1 });
    return trace.stackFrames[0];
}

async function run() {
    const folder = vscode.workspace.workspaceFolders[0];
    const uri = vscode.Uri.file(path.join(drive, 'code.py'));
    vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(uri, new vscode.Position(1, 0)))]);
    const config = {
        type: 'circuitpython', request: 'launch', name: 'e2e',
        program, drive, port, board: 'virtual',
    };

    // A breakpoint and a variable.
    assert.ok(await vscode.debug.startDebugging(folder, config), 'startDebugging');
    const first = await until('a session', () => vscode.debug.activeDebugSession, 30000);
    const stop1 = await until('the breakpoint', () => stoppedIn(first.id), 30000);
    const frame = await topFrame(first, stop1[2].threadId);
    assert.strictEqual(frame.line, 2, 'stopped on line 2');
    const x = await first.customRequest('evaluate', { expression: 'x', frameId: frame.id, context: 'watch' });
    assert.strictEqual(x.result, '41');

    // code.py ends, a file is saved: the program reloads and the extension connects again by
    // itself and stops at the breakpoint again.
    await first.customRequest('continue', { threadId: stop1[2].threadId });
    await until('the session to end', () => events.find((e) => e[0] === first.id && e[1] === 'terminated'), 30000);
    fs.writeFileSync(path.join(drive, 'code.py'), 'x = 41\nx += 1\nprint("e2e again", x)\n');
    const second = await until('a new session', () => {
        const s = vscode.debug.activeDebugSession;
        return s && s.id !== first.id ? s : undefined;
    }, 60000);
    const stop2 = await until('the breakpoint again', () => stoppedIn(second.id), 30000);
    assert.strictEqual((await topFrame(second, stop2[2].threadId)).line, 2, 'stopped on line 2 again');

    // Stop: no new session follows.
    await vscode.debug.stopDebugging(second);
    await until('the session to end', () => vscode.debug.activeDebugSession === undefined, 15000);
    await sleep(4000);
    assert.strictEqual(vscode.debug.activeDebugSession, undefined, 'no reconnect after Stop');

    // The board command is there.
    const commands = await vscode.commands.getCommands(true);
    assert.ok(commands.includes('circuitpython.selectBoard'), 'circuitpython.selectBoard registered');

    // An exception that ends code.py: the Problems panel shows it on its line.
    vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    fs.writeFileSync(path.join(drive, 'code.py'), 'def boom():\n    return 1 / 0\nboom()\n');
    const pending = until('a session', () => vscode.debug.activeDebugSession, 30000);
    await vscode.debug.startDebugging(folder, config);
    await pending;
    const problem = await until('the exception in Problems', () => {
        const list = vscode.languages.getDiagnostics(uri);
        return list.length > 0 ? list[0] : undefined;
    }, 30000);
    assert.strictEqual(problem.range.start.line, 1);
    assert.match(problem.message, /ZeroDivisionError/);

    for (const terminal of vscode.window.terminals) {
        terminal.dispose();
    }
    await sleep(1000);
    let checks = 7;

    // The libraries code.py imports, from PyPI into lib/ (skipped offline: CP_E2E_OFFLINE=1).
    if (process.env.CP_E2E_OFFLINE !== '1') {
        fs.writeFileSync(path.join(drive, 'code.py'), 'import board\nimport adafruit_pcf8523.pcf8523\n');
        await vscode.commands.executeCommand('circuitpython.installFromCode');
        for (const name of ['adafruit_pcf8523', 'adafruit_register', 'adafruit_bus_device']) {
            assert.ok(fs.existsSync(path.join(drive, 'lib', name)), name + ' in lib/');
        }
        checks += 1;
    }

    // With an MCP2221 plugged in (CP_E2E_MCP2221=1): its I2C scan reaches the bus panel's file.
    if (process.env.CP_E2E_MCP2221 === '1') {
        vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
        fs.writeFileSync(path.join(drive, 'code.py'),
            'import board, busio\ni2c = busio.I2C(board.SCL, board.SDA)\nwhile not i2c.try_lock():\n    pass\nprint("scan", i2c.scan())\n');
        const trace = path.join(require('os').tmpdir(), 'circuitpython-bus-' + port + '.txt');
        assert.ok(await vscode.debug.startDebugging(folder, Object.assign({}, config, { board: 'mcp2221' })), 'start on mcp2221');
        const text = await until('the scan in the bus trace', () => {
            const t = fs.existsSync(trace) ? fs.readFileSync(trace, 'utf8') : '';
            return t.split('\n').filter((l) => l.includes(' probe')).length >= 100 ? t : undefined;
        }, 30000);
        assert.match(text, /I2C 0x08 probe/);
        checks += 1;
        for (const terminal of vscode.window.terminals) {
            terminal.dispose();
        }
        await sleep(1000);
    }
    console.log('e2e: ' + checks + ' checks passed');
}

module.exports = { run };
