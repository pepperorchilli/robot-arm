// 配置文件模板（不含真实密码/密钥）
// 使用方法：复制成 config.js，再填你自己的值
//   cp config.example.js config.js
//
// 注意：config.js 已被 .gitignore 排除，不会被提交。
// 生产环境建议直接用环境变量覆盖，不要把真实密码写进文件。
module.exports = {
  PORT: process.env.PORT || 3000,
  ADMIN_PASSWORD: process.env.ADMIN_PASSWORD || '改成你的管理员密码',
  ESP32_TOKEN: process.env.ESP32_TOKEN || '改成你的ESP32暗号(随机字符串)',

  // MySQL 连接信息（建库建表见 schema.sql）
  DB: {
    host: process.env.DB_HOST || '127.0.0.1',
    port: Number(process.env.DB_PORT) || 3306,
    user: process.env.DB_USER || 'arm_app',
    password: process.env.DB_PASSWORD || '改成你的数据库密码',
    database: process.env.DB_NAME || 'robot_arm',
  },
};
