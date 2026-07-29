const fs = require('fs');
const path = require('path');

// Nạp .env (cùng thư mục với file này) — file RIÊNG của từng máy, KHÔNG copy
// sang máy khác. Đây là chỗ duy nhất chứa tham số khác nhau giữa các máy
// (đường dẫn weights, chuỗi DB, IP RTSP công khai...). Nhờ vậy config.json và
// file này giữ nguyên khi copy project, chỉ .env là mỗi máy tự viết.
// Xem .env.example để biết các biến hỗ trợ.
// Không có file cũng không sao: engine chạy đúng theo config/config.json.
function loadLocalEnv() {
  const file = path.join(__dirname, '.env');
  if (!fs.existsSync(file)) return {};
  const out = {};
  for (const raw of fs.readFileSync(file, 'utf8').split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const eq = line.indexOf('=');
    if (eq <= 0) continue;
    const key = line.slice(0, eq).trim();
    let val = line.slice(eq + 1).trim();
    // Bỏ nháy bao ngoài để giá trị có khoảng trắng vẫn viết được.
    if (val.length >= 2 && (val[0] === '"' || val[0] === "'") && val[val.length - 1] === val[0]) {
      val = val.slice(1, -1);
    }
    out[key] = val;
  }
  return out;
}

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
      // GSETTINGS_BACKEND=memory: TẮT backend GSettings "dconf".
      //
      // GStreamer nạp module GIO libdconfsettings, module này đẻ một thread
      // "dconf worker" mở một GMainContext nói chuyện với D-Bus PHIÊN
      // (DBUS_SESSION_BUS_ADDRESS mà pm2 thừa kế từ lúc đăng nhập). Khi bus
      // phiên đó trục trặc/biến mất (đăng xuất, xrdp restart, phiên VS Code
      // đổi...), worker phát tín hiệu chết -> raise(SIGTERM) NGAY TRONG tiến
      // trình (bt: dconf worker -> g_main_context_dispatch -> g_signal_emit ->
      // raise). Process tự tắt sạch (exit 0) và pm2 restart -> lặp vô hạn ~10s.
      // Engine không cần GSettings, nên ép backend "memory" để không còn worker
      // dconf và không phụ thuộc D-Bus phiên. (Chẩn đoán 2026-07-27 bằng gdb
      // catch tgkill: xác nhận kẻ gửi SIGTERM là chính process, từ dconf.)
      env: { ...loadLocalEnv(), GSETTINGS_BACKEND: 'memory' },
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
