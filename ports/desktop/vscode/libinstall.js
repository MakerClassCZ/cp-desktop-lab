// CircuitPython libraries into the drive's lib/, with their dependencies: the wheel's .py files,
// as source (the port runs source, and the debugger shows locals only for source).
//
// What may be installed comes from the Adafruit CircuitPython Bundle's index, the file circup
// reads, from the bundle's latest release on GitHub (checked against the sha256 GitHub lists):
// it maps import names to the PyPI projects Adafruit publishes and names their dependencies. A
// name taken from an import, and every dependency, has to be in it; anyone can publish a PyPI
// project called adafruit-circuitpython-anything. A name the user types may be any PyPI project.
// The files come from PyPI, each wheel checked against the sha256 PyPI lists for it.
//
// Also a command line: node libinstall.js DRIVE [--index FILE] [--from-code] [NAME...] prints the
// result as JSON; --index reads the bundle's index from a file instead of GitHub.
const crypto = require('crypto');
const fs = require('fs');
const https = require('https');
const path = require('path');
const zlib = require('zlib');

const MAX_DOWNLOAD = 20 * 1024 * 1024;
const MAX_ENTRY = 4 * 1024 * 1024;          // one file of a wheel, unpacked
const MAX_TOTAL = 16 * 1024 * 1024;         // all of a wheel, unpacked
const MAX_FILES = 2000;
const BUNDLE_RELEASE = 'https://api.github.com/repos/adafruit/Adafruit_CircuitPython_Bundle/releases/latest';

// What Blinka needs on a host computer and CircuitPython does not, and type hints.
const SKIP = new Set([
    'adafruit-blinka', 'adafruit-blinka-displayio', 'adafruit-blinka-bleio', 'adafruit-platformdetect',
    'adafruit-pureio', 'pyftdi', 'pyserial', 'pyusb', 'typing-extensions', 'sysv-ipc', 'binho-host-adapter',
    'rpi.gpio', 'rpi-gpio', 'adafruit-circuitpython-typing', 'pillow', 'numpy', 'bleak', 'pyudev',
]);

// Modules built into CircuitPython on some board (shared-bindings and the ports' bindings) and
// MicroPython's own: never looked up. adafruit_pixelbuf is a library where the core lacks it.
const BUILT_IN = new Set(('__future__ _bleio _eve _pew _pixelmap _stage aesio alarm analogbufio analogio ' +
    'atexit audiobusio audiocore audiodelays audiofilewriter audiofilters audiofreeverb audioi2sin audioio ' +
    'audiomixer audiomp3 audiopwmio audiospeed aurora_epaper bitbangio bitmapfilter bitmaptools bitops board ' +
    'busdisplay busio camera canio codeop countio digitalio displayio dotclockframebuffer dualbank emmcio ' +
    'epaperdisplay floppyio fontio fourwire framebufferio frequencyio getpass gifio gnss hardwarekey hashlib ' +
    'hmac i2cdisplaybus i2cioexpander i2ctarget imagecapture ipaddress is31fl3741 jpegio keypad keypad_demux ' +
    'locale lvfontio math max3421e mcp4822 mdns memorymap memorymonitor microcontroller mii mipidsi msgpack ' +
    'neopixel_write nvm onewireio os paralleldisplaybus picogame ps2io pulseio pwmio qrio qspibus rainbowio ' +
    'random rclcpy rgbmatrix rotaryio rtc sdcardio sdioio sharpdisplay socketpool spitarget ssl storage ' +
    'struct supervisor synthio terminalio tilepalettemapper time touchio traceback uheap usb usb_audio ' +
    'usb_cdc usb_hid usb_host usb_host_audio usb_host_bulk usb_midi usb_video ustack vectorio warnings ' +
    'watchdog wifi zlib cyw43 espcamera espidf espnow espulp picodvi rp2pio samd wiznet zephyr_display ' +
    'zephyr_kernel hostnetwork sdl array binascii builtins collections errno gc io json micropython re ' +
    'select sys ulab typing').split(/\s+/));

function normal(name) {
    return name.trim().toLowerCase().replace(/[_.]+/g, '-');
}

// "bme280", "adafruit_bme280" or "adafruit-circuitpython-bme280" -> "adafruit-circuitpython-bme280".
function packageName(name) {
    const n = normal(name);
    if (n.startsWith('adafruit-circuitpython-') || (n.includes('-') && !n.startsWith('adafruit-'))) {
        return n;
    }
    return 'adafruit-circuitpython-' + n.replace(/^adafruit-/, '');
}

// The bundle's import name for a name in any spelling, or null when the bundle has no such library.
function findInBundle(bundle, name) {
    const raw = name.trim();
    for (const key of [raw, raw.toLowerCase(), 'adafruit_' + raw.toLowerCase()]) {
        if (Object.prototype.hasOwnProperty.call(bundle, key)) {
            return key;
        }
    }
    const pypi = packageName(raw);
    return Object.keys(bundle).find((key) => normal(bundle[key].pypi_name || '') === pypi) || null;
}

// The top-level modules a program imports, other than CircuitPython's own and relative ones.
function importsOf(source) {
    const found = new Set();
    for (const raw of String(source).split(/\r?\n/)) {
        const line = raw.replace(/#.*$/, '');
        let m = line.match(/^\s*import\s+(.+)$/);
        if (m) {
            for (const part of m[1].split(',')) {
                const name = part.trim().split(/\s+/)[0].split('.')[0];
                if (/^\w+$/.test(name)) {
                    found.add(name);
                }
            }
            continue;
        }
        m = line.match(/^\s*from\s+([\w.]+)\s+import\b/);
        if (m && !m[1].startsWith('.')) {
            found.add(m[1].split('.')[0]);
        }
    }
    return [...found].filter((n) => !BUILT_IN.has(n));
}

// Requires-Dist lines -> the package names CircuitPython needs: no extras, nothing in SKIP.
function dependencies(requires) {
    const out = [];
    for (const req of requires || []) {
        const [spec, marker] = req.split(';');
        if (marker && /extra\s*==/.test(marker)) {
            continue;
        }
        const name = normal(spec.split(/[\s<>=!~([]/)[0]);
        if (name && !SKIP.has(name) && !out.includes(name)) {
            out.push(name);
        }
    }
    return out;
}

const HOSTS = ['pypi.org', 'files.pythonhosted.org', 'api.github.com', 'github.com',
    'release-assets.githubusercontent.com', 'objects.githubusercontent.com'];

function allowedUrl(url) {
    try {
        const u = new URL(url);
        return u.protocol === 'https:' && HOSTS.includes(u.hostname);
    } catch (e) {
        return false;
    }
}

// The download is the one its index lists: the sha256 has to match.
function checkDigest(data, digests) {
    const want = digests && digests.sha256;
    const got = crypto.createHash('sha256').update(data).digest('hex');
    if (!want || want.toLowerCase() !== got) {
        throw new Error('a download does not match the sha256 its index gives');
    }
}

// The .py files of a wheel (a zip archive), by their path inside it; dist-info is left out. An
// entry is at most maxEntry bytes unpacked, the whole at most MAX_TOTAL.
function pythonFilesOf(zip, maxEntry = MAX_ENTRY) {
    const fits = (start, len) => start >= 0 && start + len <= zip.length;
    let end = zip.length - 22;
    while (end >= 0 && zip.readUInt32LE(end) !== 0x06054b50) {
        end--;
    }
    if (end < 0) {
        throw new Error('not a zip archive');
    }
    const count = zip.readUInt16LE(end + 10);
    if (count > MAX_FILES) {
        throw new Error('a wheel with too many files');
    }
    let at = zip.readUInt32LE(end + 16);
    const files = {};
    let total = 0;
    for (let i = 0; i < count; i++) {
        if (!fits(at, 46) || zip.readUInt32LE(at) !== 0x02014b50) {
            throw new Error('a damaged zip archive');
        }
        const method = zip.readUInt16LE(at + 10);
        const packedSize = zip.readUInt32LE(at + 20);
        const nameLen = zip.readUInt16LE(at + 28);
        const extraLen = zip.readUInt16LE(at + 30);
        const commentLen = zip.readUInt16LE(at + 32);
        const local = zip.readUInt32LE(at + 42);
        if (!fits(at + 46, nameLen)) {
            throw new Error('a damaged zip archive');
        }
        const name = zip.toString('utf8', at + 46, at + 46 + nameLen);
        at += 46 + nameLen + extraLen + commentLen;
        if (!name.endsWith('.py') || name.split('/')[0].endsWith('.dist-info')) {
            continue;
        }
        if (name.startsWith('/') || name.split(/[/\\]/).includes('..') || /^[A-Za-z]:/.test(name)) {
            throw new Error('a wheel entry outside lib/: ' + name);
        }
        if (Object.prototype.hasOwnProperty.call(files, name)) {
            throw new Error('a wheel with a file twice: ' + name);
        }
        if (!fits(local, 30)) {
            throw new Error('a damaged zip archive');
        }
        const dataAt = local + 30 + zip.readUInt16LE(local + 26) + zip.readUInt16LE(local + 28);
        if (!fits(dataAt, packedSize)) {
            throw new Error('a damaged zip archive');
        }
        const packed = zip.subarray(dataAt, dataAt + packedSize);
        const room = Math.min(maxEntry, MAX_TOTAL - total);
        if (method !== 8 && packed.length > room) {
            throw new Error('a wheel entry too large: ' + name);
        }
        try {
            files[name] = method === 8 ? zlib.inflateRawSync(packed, { maxOutputLength: Math.max(room, 1) }) : Buffer.from(packed);
        } catch (e) {
            throw new Error(e.code === 'ERR_BUFFER_TOO_LARGE' ? 'a wheel entry too large: ' + name : 'a damaged zip archive');
        }
        total += files[name].length;
    }
    return files;
}

// A GET from the allowed hosts only, redirects included, of at most MAX_DOWNLOAD bytes.
function get(url, redirects = 5) {
    return new Promise((resolve, reject) => {
        if (!allowedUrl(url)) {
            reject(new Error('not an address of PyPI or of the bundle: ' + url));
            return;
        }
        const headers = { 'User-Agent': 'circuitpython-host-debug', Accept: '*/*' };
        const req = https.get(url, { headers, timeout: 30000 }, (res) => {
            if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location && redirects > 0) {
                res.resume();
                resolve(get(new URL(res.headers.location, url).toString(), redirects - 1));
                return;
            }
            const chunks = [];
            let size = 0;
            res.on('data', (c) => {
                size += c.length;
                if (size > MAX_DOWNLOAD) {
                    req.destroy(new Error('a download larger than ' + MAX_DOWNLOAD + ' bytes: ' + url));
                    return;
                }
                chunks.push(c);
            });
            res.on('end', () => resolve({ status: res.statusCode, body: Buffer.concat(chunks) }));
            res.on('error', reject);
        });
        req.on('timeout', () => req.destroy(new Error('ETIMEDOUT ' + url)));
        req.on('error', reject);
    });
}

let bundleCache = null;

// The bundle's index from its latest release, checked against GitHub's sha256; once per process.
async function loadBundle() {
    if (bundleCache !== null) {
        return bundleCache;
    }
    const release = await get(BUNDLE_RELEASE);
    if (release.status !== 200) {
        throw new Error('the Adafruit bundle release could not be read (HTTP ' + release.status + ')');
    }
    const assets = JSON.parse(release.body.toString('utf8')).assets || [];
    const asset = assets.find((a) => /^adafruit-circuitpython-bundle-\d+\.json$/.test(a.name));
    if (!asset || !/^sha256:[0-9a-f]{64}$/i.test(asset.digest || '')) {
        throw new Error('the Adafruit bundle release has no index with a sha256');
    }
    const index = await get(asset.browser_download_url);
    if (index.status !== 200) {
        throw new Error('the Adafruit bundle index could not be read (HTTP ' + index.status + ')');
    }
    checkDigest(index.body, { sha256: asset.digest.slice(7) });
    bundleCache = JSON.parse(index.body.toString('utf8'));
    return bundleCache;
}

// One PyPI project's pure-Python wheel into libDir; null when PyPI has none.
async function installWheel(libDir, project) {
    const meta = await get('https://pypi.org/pypi/' + encodeURIComponent(project) + '/json');
    if (meta.status !== 200) {
        return null;
    }
    const info = JSON.parse(meta.body.toString('utf8'));
    const wheel = (info.urls || []).find((u) => u.packagetype === 'bdist_wheel' && /py3-none-any\.whl$/.test(u.filename));
    if (!wheel) {
        return null;
    }
    const archive = await get(wheel.url);
    if (archive.status !== 200) {
        return null;
    }
    checkDigest(archive.body, wheel.digests);
    const files = pythonFilesOf(archive.body);
    for (const [rel, data] of Object.entries(files)) {
        const target = path.join(libDir, rel);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.writeFileSync(target, data);
    }
    return { name: project, version: info.info.version, files: Object.keys(files), requires: info.info.requires_dist };
}

// Install names (any spelling) and what they depend on into DRIVE/lib. missing: neither in the
// bundle nor, for a typed name, on PyPI. refused: a typed library's dependency outside the bundle.
// options.trustNames: the names are the user's own; options.bundle: the index to use.
async function install(drive, names, onProgress = () => {}, options = {}) {
    const bundle = options.bundle || await loadBundle();
    const libDir = path.join(drive, 'lib');
    const installed = [];
    const missing = [];
    const refused = [];
    const done = new Set();
    const queue = [];                   // bundle import names
    const typed = [];                   // PyPI projects outside the bundle, as the user named them
    for (const name of names) {
        const key = findInBundle(bundle, name);
        if (key !== null) {
            queue.push(key);
        } else if (options.trustNames) {
            typed.push(packageName(name));
        } else {
            missing.push(name);
        }
    }
    const fromRequires = (requires) => {
        for (const dep of dependencies(requires)) {
            const key = findInBundle(bundle, dep);
            if (key !== null) {
                queue.push(key);
            } else if (!refused.includes(dep)) {
                refused.push(dep);
            }
        }
    };
    for (const project of typed) {
        onProgress(project);
        const got = await installWheel(libDir, project);
        if (got === null) {
            missing.push(project);
            continue;
        }
        installed.push({ name: got.name, version: got.version, files: got.files });
        fromRequires(got.requires);
    }
    while (queue.length > 0) {
        const key = queue.shift();
        if (done.has(key)) {
            continue;
        }
        done.add(key);
        const entry = bundle[key];
        onProgress(entry.pypi_name || key);
        const got = entry.pypi_name ? await installWheel(libDir, entry.pypi_name) : null;
        if (got === null) {
            missing.push(entry.pypi_name || key);
            continue;
        }
        installed.push({ name: got.name, version: got.version, files: got.files });
        queue.push(...(entry.dependencies || []).filter((d) => Object.prototype.hasOwnProperty.call(bundle, d)));
        for (const ext of entry.external_dependencies || []) {
            const dep = findInBundle(bundle, ext);
            if (dep !== null) {
                queue.push(dep);
            }
        }
    }
    return { installed, missing, refused };
}

// The libraries a program on the drive imports.
function importsOfDrive(drive) {
    for (const main of ['code.py', 'main.py']) {
        const file = path.join(drive, main);
        if (fs.existsSync(file)) {
            return importsOf(fs.readFileSync(file, 'utf8'));
        }
    }
    return [];
}

module.exports = {
    packageName, findInBundle, importsOf, importsOfDrive, dependencies, pythonFilesOf, install, loadBundle,
    allowedUrl, checkDigest, BUILT_IN,
};

if (require.main === module) {
    const args = process.argv.slice(2);
    const drive = args.shift();
    const options = {};
    const names = [];
    let fromCode = false;
    while (args.length > 0) {
        const a = args.shift();
        if (a === '--from-code') {
            fromCode = true;
        } else if (a === '--index') {
            options.bundle = JSON.parse(fs.readFileSync(args.shift(), 'utf8'));
        } else {
            names.push(a);
        }
    }
    if (!drive) {
        console.error('usage: node libinstall.js DRIVE [--index FILE] [--from-code] [NAME...]');
        process.exit(2);
    }
    if (fromCode) {
        names.push(...importsOfDrive(drive));
    } else {
        options.trustNames = true;
    }
    install(drive, names, () => {}, options).then((r) => console.log(JSON.stringify(r)), (e) => {
        console.error(String(e));
        process.exit(1);
    });
}
