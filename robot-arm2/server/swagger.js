// Swagger / OpenAPI 文档配置
//
// 接口注解直接写在 server.js 的路由上方（JSDoc 格式），
// swagger-jsdoc 会自动扫描并生成 OpenAPI 规范。
// 好处：文档和代码在一起，改接口时不容易忘记同步文档。
const path = require('path');
const swaggerJsdoc = require('swagger-jsdoc');

const options = {
  definition: {
    openapi: '3.0.3',
    info: {
      title: '机械臂云控 API',
      version: '1.0.0',
      description: [
        'ESP32-S3 + PCA9685 五自由度机械臂的云端控制接口。',
        '',
        '**系统架构**',
        '',
        '```',
        'Vue 网页遥控 ──┐',
        '               ├──HTTP──> 本服务器 ──WebSocket──> ESP32 ──I2C──> PCA9685 ──> 5 个舵机',
        'Python 手势 ───┘',
        '```',
        '',
        '**几个约定**',
        '- 舵机编号 `servo` 取值 0~5，依次是：底座、肩部、肘部、腕俯仰、腕旋转、夹爪',
        '- 角度 `angle` 取值 0~180（度）',
        '- 管理类接口需要管理员密码，通过请求体传递（本地开发默认 `123456`）',
        '- ESP32 未连接时，遥控接口返回 503',
      ].join('\n'),
    },
    servers: [
      { url: 'http://localhost:3000', description: '本地开发' },
    ],
    tags: [
      { name: '遥控', description: '机械臂舵机控制（经 WebSocket 转发给 ESP32）' },
      { name: '留言板', description: '官网留言与博主回复' },
    ],
    components: {
      schemas: {
        Reply: {
          type: 'object',
          description: '博主的回复',
          properties: {
            content: { type: 'string', example: '谢谢支持！' },
            time: {
              type: 'integer',
              description: '毫秒时间戳',
              example: 1789696196789,
            },
          },
        },
        Message: {
          type: 'object',
          description: '一条留言',
          properties: {
            id: { type: 'integer', example: 1 },
            nickname: { type: 'string', example: '张三' },
            content: { type: 'string', example: '机械臂很酷' },
            time: { type: 'integer', description: '毫秒时间戳', example: 1789696196644 },
            replies: {
              type: 'array',
              items: { $ref: '#/components/schemas/Reply' },
            },
          },
        },
        Error: {
          type: 'object',
          properties: {
            error: { type: 'string', example: '密码错误' },
          },
        },
      },
    },
  },
  // 注解写在这里列出的文件里
  apis: [path.join(__dirname, 'server.js')],
};

module.exports = swaggerJsdoc(options);
