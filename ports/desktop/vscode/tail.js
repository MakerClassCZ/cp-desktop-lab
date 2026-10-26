// Following a file the program appends to (its --trace-bus file), as `tail -f` does: whole lines
// as they arrive, from the size it had at start; a file emptied again is read from its start.
const fs = require('fs');

const READ_MAX = 60 * 1024;         // read per poll, so a burst cannot take all memory at once
const PARTIAL_MAX = 1024 * 1024;    // a line without an end is passed on once it is this long

class FileTail {
    constructor(path, onChunk, intervalMs = 200) {
        this.path = path;
        this.onChunk = onChunk;
        this.intervalMs = intervalMs;
        this.offset = 0;
        this.partial = '';
        this.timer = null;
    }

    start() {
        try {
            this.offset = fs.statSync(this.path).size;
        } catch (e) {
            this.offset = 0;
        }
        this.timer = setInterval(() => this.poll(), this.intervalMs);
    }

    stop() {
        if (this.timer !== null) {
            clearInterval(this.timer);
            this.timer = null;
        }
    }

    poll() {
        let size;
        try {
            size = fs.statSync(this.path).size;
        } catch (e) {
            return;
        }
        if (size < this.offset) {
            this.offset = 0;
            this.partial = '';
        }
        if (size === this.offset) {
            return;
        }
        const buf = Buffer.alloc(Math.min(size - this.offset, READ_MAX));
        const fd = fs.openSync(this.path, 'r');
        let n;
        try {
            n = fs.readSync(fd, buf, 0, buf.length, this.offset);
        } finally {
            fs.closeSync(fd);
        }
        this.offset += n;
        const text = this.partial + buf.toString('utf8', 0, n);
        let end = text.lastIndexOf('\n') + 1;
        if (end === 0 && text.length >= PARTIAL_MAX) {
            end = text.length;
        }
        this.partial = text.slice(end);
        if (end > 0) {
            this.onChunk(text.slice(0, end));
        }
    }
}

module.exports = { FileTail };
