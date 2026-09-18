// 配置文件模板（不含真实密码/密钥）
// 使用方法：复制成 config.js，再填你自己的值
//   cp config.example.js config.js
module.exports = {
  PORT: process.env.PORT || 3000,
  ADMIN_PASSWORD: process.env.ADMIN_PASSWORD || '改成你的管理员密码',
  ESP32_TOKEN: process.env.ESP32_TOKEN || '改成你的ESP32暗号(随机字符串)',
};
