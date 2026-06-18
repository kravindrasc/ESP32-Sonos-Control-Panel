import express from 'express';
import sharp from 'sharp';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ART_CACHE_DIR = path.join(__dirname, 'art-cache');
if (!fs.existsSync(ART_CACHE_DIR)) fs.mkdirSync(ART_CACHE_DIR, { recursive: true });

const SONOS_API = process.env.SONOS_API || 'http://localhost:5005';
const PORT = process.env.PORT || 3000;
const SONOS_PLAYER_IP = process.env.SONOS_PLAYER_IP || '192.168.1.100';

const app = express();
app.use(express.json());

async function sonosGet(path) {
  const res = await fetch(`${SONOS_API}${path}`);
  if (!res.ok) throw new Error(`Sonos GET failed: ${res.status}`);
  return res.json();
}

async function sonosText(path) {
  const res = await fetch(`${SONOS_API}${path}`);
  if (!res.ok) throw new Error(`Sonos cmd failed: ${res.status}`);
  return res.text();
}

function encodeRoom(room) {
  return encodeURIComponent(room);
}

function getAbsoluteArt(state) {
  return (
    state?.currentTrack?.absoluteAlbumArtUri ||
    state?.currentTrack?.absoluteAlbumArtURI ||
    ''
  );
}

function getRelativeArt(state) {
  return (
    state?.currentTrack?.albumArtUri ||
    state?.currentTrack?.albumArtURI ||
    ''
  );
}

function buildArtUrl(req, room, state) {
  const relativeArt = getRelativeArt(state);
  const absoluteArt = getAbsoluteArt(state);

  if (!(relativeArt || absoluteArt)) return '';

  const proto = req.protocol;
  const host = req.get('host');
  return `${proto}://${host}/api/sonos/art?room=${encodeURIComponent(room)}`;
}

async function fetchArtBuffer(state) {
  const relativeArt = getRelativeArt(state);
  const absoluteArt = getAbsoluteArt(state);

  let upstreamUrl = '';

  if (relativeArt) {
    upstreamUrl = `http://${SONOS_PLAYER_IP}:1400${relativeArt}`;
  } else if (absoluteArt) {
    upstreamUrl = absoluteArt;
  } else {
    return null;
  }

  const upstream = await fetch(upstreamUrl);
  if (!upstream.ok) {
    throw new Error(`Art fetch failed: ${upstream.status}`);
  }

  return Buffer.from(await upstream.arrayBuffer());
}

app.get('/api/sonos/now-playing', async (req, res) => {
  try {
    const room = req.query.room || 'Living Room';
    const state = await sonosGet(`/${encodeRoom(room)}/state`);

    res.json({
      room,
      title: state.currentTrack?.title || 'Unknown Title',
      artist: state.currentTrack?.artist || 'Unknown Artist',
      album: state.currentTrack?.album || '',
      artUrl: buildArtUrl(req, room, state),
      isPlaying: state.playerState === 'PLAYING',
      elapsedSec: state.elapsedTime || 0,
      durationSec: state.currentTrack?.duration || 0,
      volume: state.volume || 0,
      raw: state
    });
  } catch (err) {
    res.status(500).json({ error: String(err.message || err) });
  }
});

app.get('/api/sonos/favorites', async (req, res) => {
  try {
    const room = req.query.room || 'Living Room';
    const items = await sonosGet(`/${encodeRoom(room)}/favorites/detailed`);
    const proto = req.protocol;
    const host = req.get('host');
    const result = (items || []).map((item, idx) => ({
      index: idx,
      title: item.title || '',
      uri: item.uri || '',
      metadata: item.metadata || '',
      artUrl: item.albumArtUri
        ? `${proto}://${host}/api/sonos/favorite-art?index=${idx}&room=${encodeURIComponent(room)}`
        : '',
    }));
    res.json(result);
  } catch (err) {
    res.status(500).json({ error: String(err.message || err) });
  }
});

// Favorite art cache: memory-first, disk-backed so art survives bridge restarts.
// Once fetched successfully, the JPEG is stored permanently — this handles the case
// where Apple Music / Sonos CDN URLs are time-limited and expire between sessions.
const favArtMemCache = new Map();

// When a playlist's cover art URL is expired (pre-signed S3 blobstore URLs from
// Apple Music expire after 24h), fall back to the first track's album art, which
// uses static mzstatic.com URLs that don't expire.
async function fetchFirstTrackArt(favItem, room) {
  try {
    // Load the favorite into a temporary queue browse via the Sonos player SOAP.
    // The container URI looks like: x-rincon-cpcontainer:1006206clibraryplaylist%3Ap.xxx?sid=204&...
    // We ask the Sonos player to add it to the queue temporarily, read the first
    // queued item's art, then restore nothing (the queue isn't modified if we just browse).

    // Use the Sonos player's ContentDirectory to browse A:ALBUMARTIST or similar — but
    // Apple Music containers aren't accessible that way. Instead, try getaa with the
    // item's URI in a track-level format that the Sonos player can proxy.

    // The most reliable fallback: strip the container prefix and try getaa
    const uri = favItem.uri || '';
    // Try the Sonos getaa proxy with the raw URI — works for some service items
    const getaaUrl = `http://${SONOS_PLAYER_IP}:1400/getaa?s=1&u=${encodeURIComponent(uri)}`;
    const r = await fetch(getaaUrl);
    if (r.ok && r.headers.get('content-type')?.startsWith('image/')) {
      console.log(`[fav-art] getaa fallback succeeded for "${favItem.title}"`);
      return Buffer.from(await r.arrayBuffer());
    }
  } catch (_) {}
  return null;
}

function artCachePath(room, title, size) {
  const safeRoom = room.replace(/[^a-zA-Z0-9]/g, '_');
  const safeTitle = title.replace(/[^a-zA-Z0-9]/g, '_');
  return path.join(ART_CACHE_DIR, `${safeRoom}_${safeTitle}_${size}.jpg`);
}

function readArtFromDisk(room, title, size) {
  try {
    const p = artCachePath(room, title, size);
    if (fs.existsSync(p)) return fs.readFileSync(p);
  } catch (_) {}
  return null;
}

function writeArtToDisk(room, title, size, jpeg) {
  try { fs.writeFileSync(artCachePath(room, title, size), jpeg); } catch (_) {}
}

app.get('/api/sonos/favorite-art', async (req, res) => {
  try {
    const room = req.query.room || 'Living Room';
    const idx = Number(req.query.index ?? -1);
    const size = Math.min(Math.max(Number(req.query.size ?? 64), 32), 240);

    // Fetch item list first so we can key everything by title, not index
    const items = await sonosGet(`/${encodeRoom(room)}/favorites/detailed`);
    if (!items || idx < 0 || idx >= items.length) return res.status(404).send('Not found');

    const title = items[idx].title || String(idx);
    const cacheKey = `${room}:${title}:${size}`;

    // 1. Memory cache
    if (favArtMemCache.has(cacheKey)) {
      res.setHeader('Content-Type', 'image/jpeg');
      res.setHeader('Cache-Control', 'max-age=86400');
      return res.send(favArtMemCache.get(cacheKey));
    }

    // 2. Disk cache — keyed by title so reordering favorites doesn't break art
    const cached = readArtFromDisk(room, title, size);
    if (cached) {
      favArtMemCache.set(cacheKey, cached);
      res.setHeader('Content-Type', 'image/jpeg');
      res.setHeader('Cache-Control', 'max-age=86400');
      return res.send(cached);
    }

    // 3. Fetch fresh — try the primary albumArtUri, then fall back to getaa proxy
    const artUri = items[idx].albumArtUri;
    if (!artUri) return res.status(404).send('No art');

    const resolvedArtUri = (artUri.startsWith('http://') || artUri.startsWith('https://'))
      ? artUri
      : `http://${SONOS_PLAYER_IP}:1400${artUri}`;

    let raw = null;
    const upstream = await fetch(resolvedArtUri);
    if (upstream.ok) {
      raw = Buffer.from(await upstream.arrayBuffer());
    } else {
      console.warn(`[fav-art] "${title}" — primary art ${upstream.status}, trying track fallback`);
      raw = await fetchFirstTrackArt(items[idx], room);
      if (!raw) return res.status(502).send('Art unavailable');
    }

    const jpeg = await sharp(raw)
      .resize(size, size, { fit: 'cover' })
      .jpeg({ quality: 80, force: true })
      .toBuffer();

    favArtMemCache.set(cacheKey, jpeg);
    writeArtToDisk(room, title, size, jpeg);
    res.setHeader('Content-Type', 'image/jpeg');
    res.setHeader('Cache-Control', 'max-age=86400');
    res.send(jpeg);
  } catch (err) {
    res.status(500).send(String(err.message || err));
  }
});

app.get('/api/sonos/room-volumes', async (req, res) => {
  try {
    const zones = await sonosGet('/zones');
    const result = {};
    zones.forEach(z => z.members.forEach(m => {
      result[m.roomName] = m.state?.volume ?? 0;
    }));
    res.json(result);
  } catch (err) {
    res.status(500).json({ error: String(err.message || err) });
  }
});

app.get('/api/sonos/art', async (req, res) => {
  try {
    const room = req.query.room || 'Living Room';
    const state = await sonosGet(`/${encodeRoom(room)}/state`);
    const artBuffer = await fetchArtBuffer(state);

    if (!artBuffer) {
      return res.status(404).send('No album art');
    }

    const jpegBuffer = await sharp(artBuffer)
      .resize(240, 240, { fit: 'cover' })
      .jpeg({ quality: 85, force: true })
      .toBuffer();

    res.setHeader('Content-Type', 'image/jpeg');
    res.setHeader('Cache-Control', 'no-store');
    res.send(jpegBuffer);
  } catch (err) {
    res.status(500).send(String(err.message || err));
  }
});

app.post('/api/sonos/command', async (req, res) => {
  try {
    const { room = 'Living Room', action, value } = req.body || {};
    if (!action) return res.status(400).json({ error: 'Missing action' });

    let path;
    switch (action) {
      case 'play':
      case 'pause':
      case 'playpause':
      case 'next':
      case 'previous':
        path = `/${encodeRoom(room)}/${action}`;
        break;
      case 'volume': {
        const vol = Math.max(0, Math.min(100, Number(value || 0)));
        path = `/${encodeRoom(room)}/volume/${vol}`;
        break;
      }
      case 'mute':
      case 'unmute':
        path = `/${encodeRoom(room)}/${action}`;
        break;
      case 'shuffle':
        path = `/${encodeRoom(room)}/shuffle/toggle`;
        break;
      case 'repeat':
        path = `/${encodeRoom(room)}/repeat/toggle`;
        break;
      case 'join': {
        const coordinator = String(req.body.coordinator || '');
        if (!coordinator) return res.status(400).json({ error: 'Missing coordinator for join' });
        path = `/${encodeRoom(room)}/join/${encodeRoom(coordinator)}`;
        break;
      }
      case 'leave':
        path = `/${encodeRoom(room)}/leave`;
        break;
      case 'linein': {
        // TV/HDMI ARC input uses x-sonos-htastream:<coordinator-uuid>:spdif
        const zones = await sonosGet('/zones');
        const zone = zones.find(z =>
          z.members.some(m => m.roomName.toLowerCase() === room.toLowerCase())
        );
        if (!zone) return res.status(404).json({ error: `Room "${room}" not found` });
        const uuid = zone.coordinator.uuid;
        const tvUri = encodeURIComponent(`x-sonos-htastream:${uuid}:spdif`);
        path = `/${encodeRoom(room)}/setavtransporturi/${tvUri}`;
        break;
      }
      case 'favorite': {
        const favIdx = Number(value ?? 0);
        const favs = await sonosGet(`/${encodeRoom(room)}/favorites/detailed`);
        if (!favs || favIdx < 0 || favIdx >= favs.length) {
          return res.status(404).json({ error: 'Favorite not found' });
        }
        path = `/${encodeRoom(room)}/favorite/${encodeURIComponent(favs[favIdx].title)}`;
        break;
      }
      default:
        return res.status(400).json({ error: 'Unsupported action' });
    }

    const result = await sonosText(path);
    res.json({ ok: true, result });
  } catch (err) {
    res.status(500).json({ error: String(err.message || err) });
  }
});

app.get('/api/sonos/zones', async (req, res) => {
  try {
    const zones = await sonosGet('/zones');
    const rooms = [];
    zones.forEach(z => {
      const coord = z.coordinator.roomName;
      z.members.forEach(m => rooms.push({
        name: m.roomName,
        coordinator: coord,
        volume: m.state?.volume ?? 0
      }));
    });
    rooms.sort((a, b) => a.name.localeCompare(b.name));
    res.json(rooms);
  } catch (err) {
    res.status(500).json({ error: String(err.message || err) });
  }
});

app.get('/api/health', (_req, res) => {
  res.json({
    ok: true,
    sonosApi: SONOS_API,
    sonosPlayerIp: SONOS_PLAYER_IP
  });
});

app.listen(PORT, () => {
  console.log(`Sonos frame bridge listening on http://localhost:${PORT}`);
});