(() => {
  const ensureToast = () => {
    let el = document.getElementById('globalToast');
    if (el) return el;
    el = document.createElement('div');
    el.id = 'globalToast';
    el.style.position = 'fixed';
    el.style.left = '50%';
    el.style.top = '24px';
    el.style.transform = 'translateX(-50%)';
    el.style.zIndex = '9999';
    el.style.padding = '10px 14px';
    el.style.borderRadius = '10px';
    el.style.background = 'rgba(0,0,0,0.78)';
    el.style.color = '#fff';
    el.style.fontSize = '14px';
    el.style.maxWidth = '86vw';
    el.style.display = 'none';
    document.body.appendChild(el);
    return el;
  };

  window.showToast = (text, ms = 2000) => {
    const el = ensureToast();
    el.textContent = text;
    el.style.display = 'block';
    clearTimeout(el.__t);
    el.__t = setTimeout(() => {
      el.style.display = 'none';
    }, ms);
  };

  const safeJson = async (resp) => {
    try {
      return await resp.json();
    } catch {
      return null;
    }
  };

  const withAuth = (options) => {
    const token = localStorage.getItem('jwt_token') || '';
    if (!token) return options;
    const out = Object.assign({}, options || {});
    out.headers = Object.assign({}, (options && options.headers) || {});
    if (!out.headers.Authorization && !out.headers.authorization) {
      out.headers.Authorization = `Bearer ${token}`;
    }
    return out;
  };

  window.httpRequestJson = async (url, options) => {
    const resp = await fetch(url, withAuth(options));
    const requestId = resp.headers.get('X-Request-Id') || '';
    const body = await safeJson(resp);
    return { ok: resp.ok, status: resp.status, requestId, body };
  };

  window.handleHttpError = ({ status, requestId, body }) => {
    const code = (body && (body.code || (body.error && body.error.code))) || `HTTP_${status}`;
    const message = (body && (body.message || (body.error && body.error.message))) || '请求失败';
    const ts = body && body.timestamp;
    const suffix = requestId ? ` (request_id=${requestId})` : '';
    const tsSuffix = ts ? ` [${ts}]` : '';

    console.error('HTTP请求失败', { status, code, message, requestId, body });

    if (status === 401) {
      if (!location.pathname.endsWith('/login.html') && !location.pathname.endsWith('login.html')) {
        showToast('未授权，请先登录' + suffix, 2000);
        setTimeout(() => { window.location.href = 'login.html'; }, 400);
        return message;
      }
    }
    if (status === 403) {
      showToast('禁止访问' + suffix, 2000);
      setTimeout(() => { window.location.href = '403.html'; }, 400);
      return message;
    }
    if (status === 404) {
      showToast('资源不存在' + suffix, 2000);
      setTimeout(() => { window.location.href = '404.html'; }, 400);
      return message;
    }
    if (status >= 500) {
      showToast('服务器错误' + suffix, 2000);
      setTimeout(() => { window.location.href = 'error.html'; }, 400);
      return message;
    }

    showToast(`${message}${tsSuffix}${suffix}`, 2200);
    return message;
  };
})();
