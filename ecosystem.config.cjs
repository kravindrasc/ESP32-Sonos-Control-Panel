const path = require('path');

// PM2 process config.
// Run `pm2 start ecosystem.config.cjs` from the repo root.
// node-sonos-http-api must be cloned as a sibling folder — see README.
module.exports = {
  apps: [
    {
      name: 'sonos-api',
      script: 'server.js',
      cwd: path.join(__dirname, 'node-sonos-http-api'),
      restart_delay: 2000,
      max_restarts: 50,
    },
    {
      name: 'sonos-bridge',
      script: 'server.js',
      cwd: path.join(__dirname, 'bridge'),
      restart_delay: 2000,
      max_restarts: 50,
    },
  ],
};
