// node --test test/libinstall.test.js: CircuitPython libraries from PyPI into lib/.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const lib = require('../libinstall');

// A zip archive of {name: text}, deflated, as a wheel is.
function makeZip(files) {
    const locals = [];
    const centrals = [];
    let offset = 0;
    for (const [name, text] of Object.entries(files)) {
        const data = Buffer.from(text);
        const packed = zlib.deflateRawSync(data);
        const nameBuf = Buffer.from(name);
        const local = Buffer.alloc(30);
        local.writeUInt32LE(0x04034b50, 0);
        local.writeUInt16LE(20, 4);
        local.writeUInt16LE(8, 8);
        local.writeUInt32LE(packed.length, 18);
        local.writeUInt32LE(data.length, 22);
        local.writeUInt16LE(nameBuf.length, 26);
        locals.push(local, nameBuf, packed);
        const central = Buffer.alloc(46);
        central.writeUInt32LE(0x02014b50, 0);
        central.writeUInt16LE(20, 6);
        central.writeUInt16LE(8, 10);
        central.writeUInt32LE(packed.length, 20);
        central.writeUInt32LE(data.length, 24);
        central.writeUInt16LE(nameBuf.length, 28);
        central.writeUInt32LE(offset, 42);
        centrals.push(central, nameBuf);
        offset += 30 + nameBuf.length + packed.length;
    }
    const dir = Buffer.concat(centrals);
    const end = Buffer.alloc(22);
    end.writeUInt32LE(0x06054b50, 0);
    end.writeUInt16LE(Object.keys(files).length, 8);
    end.writeUInt16LE(Object.keys(files).length, 10);
    end.writeUInt32LE(dir.length, 12);
    end.writeUInt32LE(offset, 16);
    return Buffer.concat([...locals, dir, end]);
}

// A bundle index as the release has it, cut down.
const BUNDLE = {
    adafruit_bme280: { pypi_name: 'adafruit-circuitpython-bme280', dependencies: ['adafruit_bus_device'], external_dependencies: [] },
    adafruit_bus_device: { pypi_name: 'adafruit-circuitpython-busdevice', dependencies: [], external_dependencies: ['adafruit-circuitpython-typing'] },
    neopixel: { pypi_name: 'adafruit-circuitpython-neopixel', dependencies: ['adafruit_pixelbuf'], external_dependencies: [] },
    adafruit_pixelbuf: { pypi_name: 'adafruit-circuitpython-pixelbuf', dependencies: [], external_dependencies: [] },
};

test('a library name in any of its spellings is found in the bundle', () => {
    assert.strictEqual(lib.findInBundle(BUNDLE, 'bme280'), 'adafruit_bme280');
    assert.strictEqual(lib.findInBundle(BUNDLE, 'adafruit_bme280'), 'adafruit_bme280');
    assert.strictEqual(lib.findInBundle(BUNDLE, 'Adafruit-CircuitPython-BME280'), 'adafruit_bme280');
    assert.strictEqual(lib.findInBundle(BUNDLE, 'adafruit-circuitpython-busdevice'), 'adafruit_bus_device');
    assert.strictEqual(lib.findInBundle(BUNDLE, 'neopixel'), 'neopixel');
    assert.strictEqual(lib.findInBundle(BUNDLE, 'adafruit_squat'), null);
    assert.strictEqual(lib.packageName('bme280'), 'adafruit-circuitpython-bme280');
    assert.strictEqual(lib.packageName('circuitpython-nrf24l01'), 'circuitpython-nrf24l01');
});

test('a guessed name outside the bundle is not looked up anywhere', async () => {
    const drive = fs.mkdtempSync(path.join(os.tmpdir(), 'libinstall-'));
    const result = await lib.install(drive, ['adafruit_squat'], () => {}, { bundle: BUNDLE });
    assert.deepStrictEqual(result, { installed: [], missing: ['adafruit_squat'], refused: [] });
    assert.ok(!fs.existsSync(path.join(drive, 'lib')));
});

test('downloads come only from PyPI and the bundle\'s GitHub hosts over https', () => {
    assert.ok(lib.allowedUrl('https://pypi.org/pypi/x/json'));
    assert.ok(lib.allowedUrl('https://files.pythonhosted.org/packages/a/b/x-1.0-py3-none-any.whl'));
    assert.ok(lib.allowedUrl('https://api.github.com/repos/adafruit/Adafruit_CircuitPython_Bundle/releases/latest'));
    assert.ok(lib.allowedUrl('https://release-assets.githubusercontent.com/github-production-release-asset/1/2'));
    assert.ok(!lib.allowedUrl('http://files.pythonhosted.org/packages/x.whl'));
    assert.ok(!lib.allowedUrl('https://evil.example/x.whl'));
    assert.ok(!lib.allowedUrl('https://files.pythonhosted.org.evil.example/x.whl'));
});

test('installing: the library and its dependencies into lib/, all from the bundle (skipped offline)', async (t) => {
    const drive = fs.mkdtempSync(path.join(os.tmpdir(), 'libinstall-'));
    let result;
    try {
        result = await lib.install(drive, ['pcf8523']);
    } catch (e) {
        if (/ENOTFOUND|EAI_AGAIN|ECONNREFUSED|ETIMEDOUT|getaddrinfo|HTTP 403/.test(String(e))) {
            t.skip('offline or rate-limited: ' + e.message);
            return;
        }
        throw e;
    }
    const names = fs.readdirSync(path.join(drive, 'lib')).sort();
    assert.ok(names.includes('adafruit_pcf8523'), names.join(', '));
    assert.ok(names.includes('adafruit_register'), names.join(', '));
    assert.ok(names.includes('adafruit_bus_device'), names.join(', '));
    assert.ok(result.installed.some((p) => p.name === 'adafruit-circuitpython-pcf8523'));
    assert.deepStrictEqual(result.missing, []);
    const again = await lib.install(drive, ['adafruit-circuitpython-no-such-library-xyz']);
    assert.deepStrictEqual(again.missing, ['adafruit-circuitpython-no-such-library-xyz']);
});

test('the imports of a program, without the modules built into CircuitPython', () => {
    const source = [
        'import board, busio',
        'import time',
        'from adafruit_bme280 import basic as adafruit_bme280',
        'import adafruit_display_text.label',
        'import neopixel  # LEDs',
        '    from adafruit_bus_device.i2c_device import I2CDevice',
        'from . import nothing',
        '# import adafruit_commented',
        'x = "import adafruit_in_a_string"',
    ].join('\n');
    assert.deepStrictEqual(lib.importsOf(source).sort(), ['adafruit_bme280', 'adafruit_bus_device', 'adafruit_display_text', 'neopixel']);
});

test('dependencies: names without versions, extras, Blinka and the host-only packages', () => {
    assert.deepStrictEqual(lib.dependencies([
        'Adafruit-Blinka',
        'adafruit-circuitpython-busdevice>=5.0.0',
        'adafruit-circuitpython-register (>=1.9)',
        'adafruit-circuitpython-typing ; python_version >= "3.7"',
        'pyftdi>=0.40.0',
        'sphinx ; extra == "docs"',
        'Adafruit_CircuitPython_Ticks',
    ]), ['adafruit-circuitpython-busdevice', 'adafruit-circuitpython-register', 'adafruit-circuitpython-ticks']);
});

test('a wheel gives its Python files, packages included, nothing from dist-info', () => {
    const zip = makeZip({
        'adafruit_x.py': 'X = 1\n',
        'adafruit_pkg/__init__.py': '',
        'adafruit_pkg/part.py': 'P = 2\n',
        'adafruit_x-1.0.dist-info/METADATA': 'Name: x\n',
    });
    const files = lib.pythonFilesOf(zip);
    assert.deepStrictEqual(Object.keys(files).sort(), ['adafruit_pkg/__init__.py', 'adafruit_pkg/part.py', 'adafruit_x.py']);
    assert.strictEqual(files['adafruit_pkg/part.py'].toString(), 'P = 2\n');
});

test('a wheel entry that would land outside lib/ is refused', () => {
    assert.throws(() => lib.pythonFilesOf(makeZip({ '../evil.py': 'x' })), /outside/);
    assert.throws(() => lib.pythonFilesOf(makeZip({ '/abs.py': 'x' })), /outside/);
});


test('a wheel whose sha256 differs from PyPI\'s is refused', () => {
    const zip = makeZip({ 'adafruit_x.py': 'X = 1\n' });
    const good = require('crypto').createHash('sha256').update(zip).digest('hex');
    assert.doesNotThrow(() => lib.checkDigest(zip, { sha256: good }));
    assert.throws(() => lib.checkDigest(zip, { sha256: '00'.repeat(32) }), /sha256/);
    assert.throws(() => lib.checkDigest(zip, {}), /sha256/);
});

test('a damaged or oversized wheel is refused, not read past its end', () => {
    const zip = makeZip({ 'adafruit_x.py': 'X = 1\n' });
    assert.throws(() => lib.pythonFilesOf(zip.subarray(0, zip.length - 30)), /zip/);
    const bomb = makeZip({ 'adafruit_big.py': ' '.repeat(3 * 1024 * 1024) });
    assert.throws(() => lib.pythonFilesOf(bomb, 1024 * 1024), /large/);
});

test('a wheel with a file twice is refused', () => {
    const zip = makeZip({ 'adafruit_x.py': 'X = 1', 'adafruit_y.py': 'Y = 2' });
    const twice = Buffer.from(zip.toString('latin1').split('adafruit_y.py').join('adafruit_x.py'), 'latin1');
    assert.throws(() => lib.pythonFilesOf(twice), /twice/);
});

test('modules built into CircuitPython on some board are not looked up', () => {
    assert.deepStrictEqual(lib.importsOf('import picodvi, audiospeed, max3421e, dotclockframebuffer, rp2pio, hmac\nimport adafruit_pixelbuf\n'), ['adafruit_pixelbuf']);
});
