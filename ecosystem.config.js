module.exports = {
  apps: [
    {
      // The C++ AI engine. Runs from this dir so the relative
      // "config/config.json" and "sql/" paths resolve.
      name: 'test_gstreamer',
      script: './build/test_gstreamer',
      cwd: './',
      autorestart: true,
      watch: false,
      // --- Resilience ---
      // A C++ uncaught exception (e.g. RTSP server port already in use on a
      // fast restart, model file missing) aborts the whole process. Without
      // backoff PM2 would restart it instantly, hit the still-bound port
      // again, and after ~16 rapid failures give up and leave the engine
      // DOWN. Exponential backoff gives the old socket time to leave
      // TIME_WAIT and keeps retrying instead of surrendering.
      exp_backoff_restart_delay: 2000,
      min_uptime: 10000,
      max_restarts: 50,
      // Decoder/RGA buffer or GstSample leaks would otherwise grow until the
      // board OOMs; restart well before that.
      max_memory_restart: '2500M',
      kill_timeout: 8000,
    },
  ],
};

