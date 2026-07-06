#!/usr/bin/env node
"use strict";

// snowflake relay – zero-disk raw TCP tunnel
//
// Public port  (8080) – browser connects, relay reads raw HTTP request (until \r\n\r\n)
// Control port (9000) – snowflake client connects, stays connected
//
// Each browser request is serialized through the single tunnel.
// Response bytes are piped immediately without buffering the full payload.

const net = require("net");

const PUBLIC  = parseInt(process.env.PUBLIC_PORT  || "8080", 10);
const CONTROL = parseInt(process.env.CONTROL_PORT || "9000", 10);

// ─── state ─────────────────────────────────────────────────────

let clientSock = null;
let busy = false;
const queue = [];

// ─── helpers ───────────────────────────────────────────────────

// read exactly `n` bytes from `sock`, return as Buffer
function readN(sock, n) {
  return new Promise((resolve, reject) => {
    if (n === 0) return resolve(Buffer.alloc(0));
    let buf = Buffer.alloc(0);
    let settled = false;
    function onData(c) {
      buf = Buffer.concat([buf, c]);
      if (buf.length >= n) {
        cleanup();
        if (!settled) { settled = true; resolve(buf.slice(0, n)); }
      }
    }
    function onEnd()  { if (!settled) { settled = true; resolve(buf); } }
    function onErr(e) { if (!settled) { settled = true; reject(e); } }
    function cleanup() {
      sock.removeListener("data", onData);
      sock.removeListener("end",  onEnd);
      sock.removeListener("error", onErr);
    }
    sock.on("data", onData);
    sock.on("end",  onEnd);
    sock.on("error", onErr);
  });
}

// read until delimiter bytes found, return everything so far (including delimiter)
function readUntil(sock, delim) {
  return new Promise((resolve, reject) => {
    let buf = Buffer.alloc(0);
    let settled = false;
    function onData(c) {
      buf = Buffer.concat([buf, c]);
      const idx = buf.indexOf(delim);
      if (idx !== -1) {
        cleanup();
        if (!settled) { settled = true; resolve(buf.slice(0, idx + delim.length)); }
      }
    }
    function onEnd()  { if (!settled) { settled = true; resolve(buf); } }
    function onErr(e) { if (!settled) { settled = true; reject(e); } }
    function cleanup() {
      sock.removeListener("data", onData);
      sock.removeListener("end",  onEnd);
      sock.removeListener("error", onErr);
    }
    sock.on("data", onData);
    sock.on("end",  onEnd);
    sock.on("error", onErr);
  });
}

// ─── stream a client response to a browser socket ──────────────
// 1. read response headers from client (until \r\n\r\n)
// 2. parse Content-Length
// 3. forward headers to browser
// 4. forward body bytes as they arrive
// resolves when complete response has been forwarded

function streamResponse(clientSock, browserSock) {
  return new Promise((resolve, reject) => {
    let headerEnd = -1;
    let bodyLen   = -1;
    let bodyRead  = 0;
    let buffer    = Buffer.alloc(0);
    let settled   = false;
    let doneCalled = false;

    function tryDone() {
      if (doneCalled) return;
      if (headerEnd < 0) return;               // headers not parsed yet
      if (bodyLen < 0)   return;               // Content-Length missing – wait for client close
      if (bodyRead < bodyLen) return;           // not enough body yet

      doneCalled = true;
      settled    = true;
      cleanup();
      // We've forwarded all bytes. Don't end browserSock here –
      // pump() does that in the .finally().
      resolve();
    }

    function onData(chunk) {
      if (settled) return;

      if (headerEnd < 0) {
        // still reading headers
        buffer = Buffer.concat([buffer, chunk]);
        const idx = buffer.indexOf("\r\n\r\n");
        if (idx === -1) return; // wait

        headerEnd = idx + 4;
        const hdrStr = buffer.slice(0, idx).toString("ascii");
        const m = hdrStr.match(/Content-Length:\s*(\d+)/i);
        bodyLen = m ? parseInt(m[1], 10) : -1;

        // forward headers to browser
        browserSock.write(buffer.slice(0, headerEnd));

        // forward any body bytes already accumulated
        if (headerEnd < buffer.length) {
          const bodyChunk = buffer.slice(headerEnd);
          browserSock.write(bodyChunk);
          bodyRead += bodyChunk.length;
        }

        tryDone();
      } else {
        // forwarding body
        browserSock.write(chunk);
        bodyRead += chunk.length;
        tryDone();
      }
    }

    function onEnd() {
      // client half-close – if we never got Content-Length, resolve now
      if (!settled) {
        if (headerEnd < 0) {
          // never even got headers – broken
          cleanup();
          settled = true;
          reject(new Error("client closed before headers"));
        } else if (bodyLen < 0) {
          // no Content-Length – response ended by connection close
          cleanup();
          settled = true;
          resolve();
        } else {
          // premature close before Content-Length body was fully read
          cleanup();
          settled = true;
          reject(new Error("premature client close"));
        }
      }
    }

    function onErr(e) {
      if (!settled) { cleanup(); settled = true; reject(e); }
    }

    function cleanup() {
      clientSock.removeListener("data", onData);
      clientSock.removeListener("end",  onEnd);
      clientSock.removeListener("error", onErr);
    }

    clientSock.on("data", onData);
    clientSock.on("end",  onEnd);
    clientSock.on("error", onErr);
  });
}

// ─── control server – snowflake client connects here ───────────

const ctl = net.createServer((sock) => {
  if (clientSock && !clientSock.destroyed) {
    console.log("[ctl] replacing old client");
    clientSock.destroy();
  }
  clientSock = sock;
  console.log(`[ctl] client ${sock.remoteAddress}:${sock.remotePort}`);

  // reset busy when new client connects (clears stale state from old client)
  busy = false;

  sock.on("close", () => {
    console.log("[ctl] client gone");
    if (clientSock === sock) {
      clientSock = null;
      busy = false;
    }
    // defer flush – new client may connect within 3s
    setTimeout(() => {
      if (!clientSock && queue.length > 0) flushQueue();
    }, 3000).unref();
  });
  sock.on("error", (e) => { console.error("[ctl] err:", e.message); });

  // kick pending requests if this is the active client
  if (queue.length > 0 && clientSock === sock) pump();
});

// ─── public server – browsers connect here ─────────────────────

const pub = net.createServer((browserSock) => {
  if (!clientSock || clientSock.destroyed) {
    browserSock.end("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
    browserSock.destroy();
    return;
  }
  browserSock.setTimeout(15000); // don't hang forever
  browserSock.on("timeout", () => { try { browserSock.destroy(); } catch(_) {} });
  queue.push(browserSock);
  pump();
});

// ─── serialized request pump ───────────────────────────────────

function pump() {
  if (busy || queue.length === 0 || !clientSock || clientSock.destroyed) return;
  busy = true;

  const browserSock = queue.shift();

  // log and handle error at end
  let reqPath = "?";

  readUntil(browserSock, "\r\n\r\n")
    .then((rawReq) => {
      // extract path for logging
      const first = rawReq.slice(0, rawReq.indexOf("\r\n")).toString("ascii");
      reqPath = first.split(" ")[1] || "?";

      // forward to client
      clientSock.write(rawReq);

      // stream response back to browser
      return streamResponse(clientSock, browserSock);
    })
    .then(() => {
      console.log(`[pub] ${reqPath} → ok`);
    })
    .catch((e) => {
      console.error(`[pub] ${reqPath} → ${e.message}`);
      try {
        browserSock.write("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
      } catch (_) {}
    })
    .finally(() => {
      try { browserSock.end(); browserSock.destroy(); } catch (_) {}
      busy = false;
      pump();
    });
}

function flushQueue() {
  while (queue.length) {
    const s = queue.shift();
    try {
      s.write("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
      s.destroy();
    } catch (_) {}
  }
}

// ─── start ─────────────────────────────────────────────────────

ctl.listen(CONTROL, () => console.log(`[relay] control :${CONTROL}`));
pub.listen(PUBLIC,  () => console.log(`[relay] public  :${PUBLIC}`));
console.log("[relay] snowflake ready");

process.on("SIGINT",  shutdown);
process.on("SIGTERM", shutdown);

function shutdown() {
  console.log("\n[relay] exit");
  if (clientSock) clientSock.destroy();
  flushQueue();
  ctl.close();
  pub.close();
  process.exit(0);
}
