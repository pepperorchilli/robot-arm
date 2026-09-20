// 星空背景（全站共用）
//
// 页面只要引一下本文件即可，canvas 会自动插到 body 最前面：
//   <script src="/starfield.js"></script>
//
// 纯 canvas 手绘，不引任何库。
// 三层星星做出纵深感：远的暗而小、近的亮而大，
// 各层以不同频率做正弦明暗变化，形成缓慢闪烁。

(function () {
  // 已经插过就不再插（防止重复引入脚本时叠两层）
  if (document.getElementById('sky')) return;

  const canvas = document.createElement('canvas');
  canvas.id = 'sky';

  // 插到 body 最前面，靠 CSS 的 position:fixed 垫在所有内容下面
  document.body.insertBefore(canvas, document.body.firstChild);

  const ctx = canvas.getContext('2d');
  let stars = [];
  let dpr = 1;

  const LAYERS = [
    { density: 0.00012, size: [0.4, 0.9], speed: 0.00025, alpha: [0.25, 0.55] }, // 远
    { density: 0.00006, size: [0.8, 1.5], speed: 0.00045, alpha: [0.45, 0.80] }, // 中
    { density: 0.00002, size: [1.4, 2.4], speed: 0.00080, alpha: [0.70, 1.00] }, // 近
  ];

  function build(w, h) {
    stars = [];
    for (const layer of LAYERS) {
      const n = Math.round(w * h * layer.density);
      for (let i = 0; i < n; i++) {
        stars.push({
          x: Math.random() * w,
          y: Math.random() * h,
          r: layer.size[0] + Math.random() * (layer.size[1] - layer.size[0]),
          baseAlpha: layer.alpha[0] + Math.random() * (layer.alpha[1] - layer.alpha[0]),
          speed: layer.speed * (0.6 + Math.random() * 0.8),
          phase: Math.random() * Math.PI * 2,
        });
      }
    }
  }

  function resize() {
    dpr = window.devicePixelRatio || 1;
    const w = window.innerWidth;
    const h = window.innerHeight;

    canvas.width = w * dpr;
    canvas.height = h * dpr;
    canvas.style.width = w + 'px';
    canvas.style.height = h + 'px';
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    build(w, h);
  }

  function draw(t) {
    const w = canvas.width / dpr;
    const h = canvas.height / dpr;

    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, w, h);

    ctx.fillStyle = '#fff';
    for (const s of stars) {
      const twinkle = 0.65 + 0.35 * Math.sin(t * s.speed + s.phase);
      ctx.globalAlpha = Math.min(1, s.baseAlpha * twinkle);
      ctx.beginPath();
      ctx.arc(s.x, s.y, s.r, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.globalAlpha = 1;

    requestAnimationFrame(draw);
  }

  // 系统开了「减少动态效果」就画一帧静态星空，不做动画
  const reduceMotion = window.matchMedia &&
    window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  window.addEventListener('resize', resize);
  resize();

  if (reduceMotion) {
    draw(0);
  } else {
    requestAnimationFrame(draw);
  }
})();
