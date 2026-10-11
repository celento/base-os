"use strict";

const statusEl = document.getElementById("status");
const screenEl = document.getElementById("screen");
const frameEl = document.getElementById("frame");
const overlayEl = document.getElementById("overlay");
const restartBtn = document.getElementById("restart");
const fullscreenBtn = document.getElementById("fullscreen");

function setStatus(text) { statusEl.textContent = text; }

async function fetchGzip(url) {
    const response = await fetch(url);
    if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
    const stream = response.body.pipeThrough(new DecompressionStream("gzip"));
    return new Response(stream).arrayBuffer();
}

async function start() {
    if (typeof DecompressionStream === "undefined" || typeof WebAssembly === "undefined") {
        setStatus("This browser lacks WebAssembly or DecompressionStream support.");
        return;
    }
    const info = await (await fetch("build.json", { cache: "no-store" })).json();
    const v = encodeURIComponent(info.revision + "-" + info.images["baseos.img"].sha256);
    setStatus("Downloading disk images…");
    const [floppy, data] = await Promise.all([
        fetchGzip(`images/baseos.img.gz?v=${v}`),
        fetchGzip(`images/baseos-data.img.gz?v=${v}`),
    ]);

    setStatus("Booting…");
    const emulator = new V86({
        wasm_path: `v86/v86.wasm?v=${v}`,
        memory_size: 64 * 1024 * 1024,
        vga_memory_size: 16 * 1024 * 1024,
        screen_container: screenEl,
        bios: { url: "bios/seabios.bin" },
        vga_bios: { url: "bios/vgabios.bin" },
        fda: { buffer: floppy },
        hda: { buffer: data },
        boot_order: 0x321, // floppy, then hard disk, as with `make run`
        autostart: true,
    });
    window.emulator = emulator;

    let serial = "";
    emulator.add_listener("serial0-output-byte", (byte) => {
        serial += String.fromCharCode(byte);
        if (serial.length > 65536) serial = serial.slice(-32768);
        window.baseosSerial = serial;
    });
    emulator.add_listener("emulator-ready", () => {
        setStatus(`Running build ${info.revision}`);
        restartBtn.disabled = false;
        fullscreenBtn.disabled = false;
    });

    restartBtn.onclick = () => emulator.restart();
    fullscreenBtn.onclick = () => {
        if (document.fullscreenElement) document.exitFullscreen();
        else frameEl.requestFullscreen?.();
    };
    screenEl.addEventListener("click", () => {
        // Browsers keep audio suspended until the page receives a user gesture.
        const audio = emulator.speaker_adapter && emulator.speaker_adapter.audio_context;
        if (audio && audio.state === "suspended") audio.resume();
        if (!document.pointerLockElement) emulator.lock_mouse();
    });
    document.addEventListener("pointerlockchange", () => {
        overlayEl.hidden = !!document.pointerLockElement;
    });
    overlayEl.hidden = false;
}

start().catch((error) => {
    console.error(error);
    setStatus("Could not start BaseOS: " + error.message);
});
