// node --test test/tail.test.js: following a file as a program appends to it.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { FileTail } = require('../tail');

function sleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}

test('whole lines as they are appended, a partial line when it is finished', async () => {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'tail-')), 't.txt');
    fs.writeFileSync(file, 'old line\n');
    const got = [];
    const tail = new FileTail(file, (text) => got.push(text), 20);
    tail.start();
    await sleep(60);
    assert.deepStrictEqual(got, [], 'what was there before start is skipped');
    fs.appendFileSync(file, '1.000 I2C 0x76 W f7\n2.000 I2C 0x76 R');
    await sleep(80);
    assert.deepStrictEqual(got, ['1.000 I2C 0x76 W f7\n']);
    fs.appendFileSync(file, ' 1: 60\n');
    await sleep(80);
    assert.deepStrictEqual(got.join(''), '1.000 I2C 0x76 W f7\n2.000 I2C 0x76 R 1: 60\n');
    tail.stop();
    fs.appendFileSync(file, 'after stop\n');
    await sleep(80);
    assert.ok(!got.join('').includes('after stop'));
});

test('a file that is emptied again is read from its start', async () => {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'tail-')), 't.txt');
    fs.writeFileSync(file, '');
    const got = [];
    const tail = new FileTail(file, (text) => got.push(text), 20);
    tail.start();
    fs.appendFileSync(file, 'first run\n');
    await sleep(80);
    fs.writeFileSync(file, 'new\n');
    await sleep(80);
    tail.stop();
    assert.deepStrictEqual(got.join(''), 'first run\nnew\n');
});

test('a file that does not exist yet is waited for', async () => {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'tail-')), 'later.txt');
    const got = [];
    const tail = new FileTail(file, (text) => got.push(text), 20);
    tail.start();
    await sleep(50);
    fs.writeFileSync(file, 'here\n');
    await sleep(80);
    tail.stop();
    assert.deepStrictEqual(got, ['here\n']);
});

test('a big burst arrives whole, read in bounded pieces', async () => {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'tail-')), 't.txt');
    fs.writeFileSync(file, '');
    const got = [];
    const tail = new FileTail(file, (text) => got.push(text), 10);
    tail.start();
    const line = 'x'.repeat(99) + '\n';
    fs.appendFileSync(file, line.repeat(3000));
    for (let i = 0; i < 100 && got.join('').length < 300000; i++) {
        await sleep(20);
    }
    tail.stop();
    assert.strictEqual(got.join(''), line.repeat(3000));
    assert.ok(got.length > 1, 'more than one piece');
    assert.ok(got.every((t) => t.length <= 64 * 1024));
});

test('a line without an end does not grow without bound', async () => {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'tail-')), 't.txt');
    fs.writeFileSync(file, '');
    const got = [];
    const tail = new FileTail(file, (text) => got.push(text), 10);
    tail.start();
    fs.appendFileSync(file, 'y'.repeat(1100 * 1024));
    for (let i = 0; i < 200 && got.join('').length < 1100 * 1024; i++) {
        await sleep(20);
    }
    tail.stop();
    assert.ok(got.join('').length >= 1024 * 1024, 'passed on once it is long');
});
