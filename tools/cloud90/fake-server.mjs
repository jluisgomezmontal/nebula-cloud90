// Fake Cloud90 for testing the game's link without the real API: accepts
// the Bearer token "c90gl_test_token_0123456789", answers hello with welcome and one effect,
// prints the game's messages. Needs `ws` (run from the backend's folder or
// set NODE_PATH). Usage: node fake-server.mjs [port]
import { createServer } from 'http';
import { WebSocketServer } from 'ws';

const port = Number(process.argv[2] ?? 3999);
const server = createServer();
const wss = new WebSocketServer({ noServer: true });
server.on('upgrade', (req, socket, head) => {
  if (req.url !== '/api/game-link/ws') return socket.destroy();
  if (req.headers.authorization !== 'Bearer c90gl_test_token_0123456789') {
    socket.end('HTTP/1.1 401 Unauthorized\r\n\r\n');
    return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    ws.on('message', (d) => {
      const m = JSON.parse(String(d));
      console.log('game â†’', JSON.stringify(m));
      if (m.type === 'hello') {
        ws.send(JSON.stringify({ type: 'welcome', game: 'smg' }));
        ws.send(JSON.stringify({ type: 'effect', id: 'e1', effect: 'invert_controls', ms: 5000, by: 'Ana' }));
      }
      if (m.type === 'ack') {
        console.log(m.ok ? 'ACK OK' : 'ACK FAIL ' + m.reason);
        setTimeout(() => process.exit(m.ok ? 0 : 1), 100);
      }
    });
  });
});
server.listen(port, '127.0.0.1', () => console.log('fake Cloud90 on', port));
setTimeout(() => { console.log('timeout'); process.exit(2); }, 20000);
