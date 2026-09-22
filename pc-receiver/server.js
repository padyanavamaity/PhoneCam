import express from 'express';
import http from 'http';
import { WebSocketServer } from 'ws';
import os from 'os';

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });
let phone = null, viewer = null;

wss.on('connection', ws => {
  ws.on('message', raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.role === 'phone') { phone = ws; if (viewer) viewer.send(JSON.stringify({type:'viewer-ready'})); return; }
    if (m.role === 'viewer') { viewer = ws; if (phone) phone.send(JSON.stringify({type:'viewer-ready'})); return; }
    const target = ws === phone ? viewer : phone;
    if (target?.readyState === 1) target.send(JSON.stringify(m));
  });
  ws.on('close', () => { if (ws === phone) phone = null; if (ws === viewer) viewer = null; });
});
app.use(express.static('public'));
const port = 8765;
server.listen(port, '0.0.0.0', () => {
  const ips = Object.values(os.networkInterfaces()).flat().filter(x=>x && x.family==='IPv4' && !x.internal).map(x=>x.address);
  console.log(`Receiver: http://localhost:${port}`);
  console.log(`Phone signaling URL(s): ${ips.map(ip=>`ws://${ip}:${port}`).join(', ')}`);
});
