// node --test test/
const test = require('node:test');
const assert = require('node:assert');
const b = require('../boards');

test('parseListBoards reads the program output', () => {
    const list = b.parseListBoards('virtual\talways\r\nft232h\tconnected\nmcp2221\tnot connected\nnoise\n');
    assert.deepStrictEqual(list, [
        { name: 'virtual', present: true, always: true },
        { name: 'ft232h', present: true, always: false },
        { name: 'mcp2221', present: false, always: false },
    ]);
});

test('chooseAuto picks the one plugged-in adapter', () => {
    assert.strictEqual(b.chooseAuto(b.parseListBoards('virtual\talways\nft232h\tconnected\nmcp2221\tnot connected')), 'ft232h');
    assert.strictEqual(b.chooseAuto(b.parseListBoards('virtual\talways\nft232h\tconnected\nmcp2221\tconnected')), null);
    assert.strictEqual(b.chooseAuto(b.parseListBoards('virtual\talways\nft232h\tnot connected\nmcp2221\tnot connected')), null);
});

test('configuredBoard: a name, or null for ask/auto/missing', () => {
    assert.strictEqual(b.configuredBoard({ board: 'mcp2221' }), 'mcp2221');
    assert.strictEqual(b.configuredBoard({}), null);
    assert.strictEqual(b.configuredBoard({ board: 'ask' }), null);
    assert.strictEqual(b.configuredBoard({ board: 'auto' }), null);
    assert.throws(() => b.configuredBoard({ board: 'pico' }), /unknown board "pico"/);
});

test('buildArgs adds --board unless args has one', () => {
    assert.deepStrictEqual(b.buildArgs({ args: ['--ram', '32'] }, 'ft232h', 'C:/p', 5678),
        ['--drive', 'C:/p', '--debug', '5678', '--board', 'ft232h', '--ram', '32']);
    assert.deepStrictEqual(b.buildArgs({ args: ['--board', 'mcp2221'] }, 'ft232h', '/p', 1),
        ['--drive', '/p', '--debug', '1', '--board', 'mcp2221']);
    assert.deepStrictEqual(b.buildArgs({}, null, '/p', 1), ['--drive', '/p', '--debug', '1']);
});

test('SessionWatch: reconnect after the program ended, not after the user stopped', () => {
    let w = new b.SessionWatch();
    w.fromAdapter({ type: 'event', event: 'terminated' });
    w.toAdapter({ type: 'request', command: 'disconnect' });     // VS Code answers terminated with disconnect
    assert.strictEqual(w.shouldReconnect(true), true);
    assert.strictEqual(w.shouldReconnect(false), false);

    w = new b.SessionWatch();
    w.toAdapter({ type: 'request', command: 'terminate' });      // the Stop button
    w.fromAdapter({ type: 'event', event: 'terminated' });
    assert.strictEqual(w.shouldReconnect(true), false);

    w = new b.SessionWatch();                                   // the connection simply dropped
    assert.strictEqual(w.shouldReconnect(true), true);
});

test('the bus trace: on for an adapter, off for the virtual board, as the configuration says', () => {
    assert.strictEqual(b.traceWanted({}, 'ft232h'), true);
    assert.strictEqual(b.traceWanted({}, 'virtual'), false);
    assert.strictEqual(b.traceWanted({ traceBus: true }, 'virtual'), true);
    assert.strictEqual(b.traceWanted({ traceBus: false }, 'mcp2221'), false);
    assert.deepStrictEqual(b.buildArgs({}, 'mcp2221', '/w', 5678, '/tmp/bus.txt'),
        ['--drive', '/w', '--debug', '5678', '--board', 'mcp2221', '--trace-bus', '/tmp/bus.txt']);
    assert.deepStrictEqual(b.buildArgs({ args: ['--trace-bus', '-'] }, 'mcp2221', '/w', 5678, '/tmp/bus.txt'),
        ['--drive', '/w', '--debug', '5678', '--board', 'mcp2221', '--trace-bus', '-']);
});

test('the gpiochip board is one of the boards, listed like an adapter', () => {
    assert.ok(b.BOARDS.includes('gpiochip'));
    assert.match(b.DESCRIPTIONS.gpiochip, /GPIO/);
    const list = b.parseListBoards('virtual\talways\nft232h\tnot connected\nmcp2221\tnot connected\ngpiochip\tconnected\n');
    assert.deepStrictEqual(list[3], { name: 'gpiochip', present: true, always: false });
    assert.strictEqual(b.chooseAuto(list), 'gpiochip');
});

test('boardFile becomes --board-file, unless args has one', () => {
    assert.deepStrictEqual(b.buildArgs({ boardFile: '/w/feather.toml' }, 'virtual', '/w', 5678),
        ['--drive', '/w', '--debug', '5678', '--board', 'virtual', '--board-file', '/w/feather.toml']);
    assert.deepStrictEqual(b.buildArgs({ boardFile: '/w/a.toml', args: ['--board-file', '/w/b.toml'] }, 'virtual', '/w', 5678),
        ['--drive', '/w', '--debug', '5678', '--board', 'virtual', '--board-file', '/w/b.toml']);
});
