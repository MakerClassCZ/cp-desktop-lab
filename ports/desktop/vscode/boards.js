// The parts of the extension that do not need VS Code: the boards, the program's command line,
// and when to reconnect. Tested with node (test/boards.test.js).

const BOARDS = ['virtual', 'ft232h', 'mcp2221', 'gpiochip'];

const DESCRIPTIONS = {
    virtual: 'the window, the keyboard and game controllers as buttons',
    ft232h: 'real pins through an FT232H: digitalio, busio.SPI, busio.I2C',
    mcp2221: 'real pins through an MCP2221A: digitalio, busio.I2C, busio.UART, analogio',
    gpiochip: "this computer's own GPIO, I2C, SPI and UART (a Raspberry Pi and the like)",
};

// `circuitpython --list-boards` prints one line per board: NAME<tab>always|connected|not connected.
function parseListBoards(text) {
    const boards = [];
    for (const line of String(text).split(/\r?\n/)) {
        const [name, state] = line.split('\t');
        if (BOARDS.includes(name) && state !== undefined) {
            boards.push({ name, present: state.trim() !== 'not connected', always: state.trim() === 'always' });
        }
    }
    return boards;
}

// "auto": the one adapter that is plugged in; null when none or several are, so the user picks.
function chooseAuto(boards) {
    const plugged = boards.filter((b) => b.present && !b.always);
    return plugged.length === 1 ? plugged[0].name : null;
}

// What the launch configuration's board means before anything is asked: a name, or null when the
// user (or the remembered choice, or auto-detection) has to decide.
function configuredBoard(config) {
    const board = config.board;
    if (board === undefined || board === 'ask' || board === 'auto') {
        return null;
    }
    if (!BOARDS.includes(board)) {
        throw new Error('unknown board "' + board + '"; known: ' + BOARDS.join(', ') + ', ask, auto');
    }
    return board;
}

function hasBoardArg(args) {
    return (args || []).some((a) => a === '--board');
}

// The program's command line. A --board, --trace-bus or --board-file in args wins over the property.
function buildArgs(config, board, drive, port, tracePath) {
    const args = ['--drive', drive, '--debug', String(port)];
    const extra = config.args || [];
    if (board && !hasBoardArg(extra)) {
        args.push('--board', board);
    }
    if (tracePath && !extra.includes('--trace-bus')) {
        args.push('--trace-bus', tracePath);
    }
    if (config.boardFile && !extra.includes('--board-file')) {
        args.push('--board-file', config.boardFile);
    }
    return args.concat(extra);
}

// The bus trace panel: as the configuration says, else on for a board with real pins.
function traceWanted(config, board) {
    return typeof config.traceBus === 'boolean' ? config.traceBus : board !== undefined && board !== 'virtual';
}

// A session the program ended (code.py over, a reload after a save) may be followed by the
// program waiting for the editor again (after the next save); a session the user stopped is not reconnected.
class SessionWatch {
    constructor() {
        this.programEnded = false;
        this.userStopped = false;
    }
    fromAdapter(message) {
        if (message && message.type === 'event' && (message.event === 'terminated' || message.event === 'exited')) {
            this.programEnded = true;
        }
    }
    toAdapter(message) {
        if (message && message.type === 'request' && (message.command === 'disconnect' || message.command === 'terminate')
            && !this.programEnded) {
            this.userStopped = true;
        }
    }
    shouldReconnect(enabled) {
        return Boolean(enabled) && !this.userStopped;
    }
}

module.exports = { BOARDS, DESCRIPTIONS, parseListBoards, chooseAuto, configuredBoard, buildArgs, traceWanted, SessionWatch };
