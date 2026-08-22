// guest.js — GUEST 遙控器邏輯
// URL 格式：/g/{token}

document.addEventListener('DOMContentLoaded', async () => {
    // ── 1. 從 URL 路徑讀取 token ─────────────────────────────────────────────
    const pathParts = window.location.pathname.split('/');
    const token = pathParts[pathParts.length - 1]; // /g/abc123 → abc123

    const errorPage = document.getElementById('error-page');
    const mainPage  = document.getElementById('main-page');
    const roomBadge = document.getElementById('room-badge');

    // ── 2. 驗證身份 ──────────────────────────────────────────────────────────
    let roomId = '';
    try {
        const res = await fetch(`/api/auth/me?token=${encodeURIComponent(token)}`);
        if (!res.ok) throw new Error('Unauthorized');
        const data = await res.json();

        if (data.role !== 'guest') throw new Error('Not a guest token');

        roomId = data.roomId;
        roomBadge.textContent = `🏢 ${data.roomName}`;
        document.title = `${data.roomName} 冷氣遙控器`;
        // 套用辦公室主題（橘色系）
        document.body.classList.add('theme-office');
    } catch {
        errorPage.classList.remove('hidden');
        return; // 停止後續初始化
    }

    // 驗證成功，顯示主頁面
    mainPage.classList.remove('hidden');

    // ── 3. DOM 元素 ───────────────────────────────────────────────────────────
    const megaBtns       = document.querySelectorAll('.mega-btn');
    const customTimeInput = document.getElementById('custom-time');
    const btnCustomOn    = document.getElementById('btn-custom-on');
    const btnCustomOff   = document.getElementById('btn-custom-off');
    const scheduleList   = document.getElementById('schedule-list');
    const refreshBtn     = document.getElementById('refresh-btn');
    const loader         = document.getElementById('fullscreen-loader');
    const toast          = document.getElementById('toast');
    const tempValue      = document.getElementById('temp-value');
    const btnTempUp      = document.getElementById('btn-temp-up');
    const btnTempDown    = document.getElementById('btn-temp-down');

    let currentTemp = 25; // 辦公室預設 25°C

    // ── 4. 溫度控制 ──────────────────────────────────────────────────────────
    let tempTimeout;

    btnTempUp.addEventListener('click', () => {
        if (currentTemp < 30) {
            currentTemp++;
            tempValue.innerText = currentTemp;
            clearTimeout(tempTimeout);
            tempTimeout = setTimeout(() => {
                sendScheduleRequest({ Action: 'turn_on', DelayMinutes: 0, Temperature: currentTemp }, true);
            }, 500);
        }
    });

    btnTempDown.addEventListener('click', () => {
        if (currentTemp > 18) {
            currentTemp--;
            tempValue.innerText = currentTemp;
            clearTimeout(tempTimeout);
            tempTimeout = setTimeout(() => {
                sendScheduleRequest({ Action: 'turn_on', DelayMinutes: 0, Temperature: currentTemp }, true);
            }, 500);
        }
    });

    // ── 5. 大按鈕事件 ────────────────────────────────────────────────────────
    megaBtns.forEach(btn => {
        btn.addEventListener('click', async () => {
            const action       = btn.dataset.action;
            const delayMinutes = parseInt(btn.dataset.delay);
            await sendScheduleRequest({ Action: action, DelayMinutes: delayMinutes, Temperature: currentTemp });
        });
    });

    // ── 6. 指定時間事件 ──────────────────────────────────────────────────────
    const handleCustomTime = async (action) => {
        const time = customTimeInput.value;
        if (!time) { showToast('請先選擇時間喔！', true); return; }
        await sendScheduleRequest({ Action: action, TargetTime: time, Temperature: currentTemp });
        customTimeInput.value = '';
    };

    btnCustomOn.addEventListener('click', () => handleCustomTime('turn_on'));
    btnCustomOff.addEventListener('click', () => handleCustomTime('turn_off'));

    // ── 7. 核心：發送排程請求 ──────────────────────────────────────────────────
    async function sendScheduleRequest(payload, silent = false) {
        if (!silent) showLoader(true);
        // 加入 roomId（GUEST 的 roomId 由伺服器決定，帶過去讓伺服器驗證）
        payload.RoomId = roomId;
        try {
            const response = await fetch(`/api/schedule?token=${encodeURIComponent(token)}`, {
                method:  'POST',
                headers: { 'Content-Type': 'application/json' },
                body:    JSON.stringify(payload)
            });

            if (response.ok) {
                showToast('設定成功！', false);
                await loadSchedules();
            } else if (response.status === 403) {
                showToast('沒有權限執行此操作', true);
            } else {
                showToast('設定失敗，請稍後再試', true);
            }
        } catch (error) {
            showToast('連線異常，請檢查網路', true);
            console.error(error);
        } finally {
            if (!silent) showLoader(false);
        }
    }

    // ── 8. 載入排程清單 ───────────────────────────────────────────────────────
    async function loadSchedules() {
        try {
            const response = await fetch(`/api/schedules?token=${encodeURIComponent(token)}`);
            if (!response.ok) throw new Error('Failed to load');
            const list = await response.json();
            renderSchedules(list);
        } catch (error) {
            console.error('Error:', error);
            scheduleList.innerHTML = '<div class="empty-state" style="color:var(--color-off)">無法讀取資料</div>';
        }
    }

    function renderSchedules(list) {
        if (!list || list.length === 0) {
            scheduleList.innerHTML = '<div class="empty-state">目前沒有任何排程</div>';
            return;
        }

        scheduleList.innerHTML = '';
        list.forEach(item => {
            const executeTime = new Date(item.executeAt);
            const isTurnOn    = item.action === 'turn_on';
            const card        = document.createElement('div');
            card.className    = 'schedule-card';
            card.innerHTML    = `
                <div class="schedule-info">
                    <div class="badge ${isTurnOn ? 'badge-on' : 'badge-off'}">
                        ${isTurnOn ? '開冷氣' : '關冷氣'}
                    </div>
                    <div class="schedule-time">執行時間：${formatDate(executeTime)}</div>
                </div>
                <button class="delete-btn" data-id="${item.messageId}">取消</button>
            `;
            scheduleList.appendChild(card);
        });

        document.querySelectorAll('.delete-btn').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const id = e.target.dataset.id;
                e.target.innerText = '取消中...';
                e.target.disabled  = true;
                try {
                    const res = await fetch(`/api/schedule/${id}?token=${encodeURIComponent(token)}`, { method: 'DELETE' });
                    if (res.ok) {
                        showToast('已取消！', false);
                        await loadSchedules();
                    } else throw new Error('Delete failed');
                } catch {
                    showToast('取消失敗', true);
                    e.target.innerText = '取消';
                    e.target.disabled  = false;
                }
            });
        });
    }

    // ── 輔助函式 ─────────────────────────────────────────────────────────────
    function showLoader(show) {
        if (show) loader.classList.remove('hidden');
        else      loader.classList.add('hidden');
    }

    let toastTimeout;
    function showToast(msg, isError) {
        toast.innerText = msg;
        if (isError) toast.classList.add('error');
        else         toast.classList.remove('error');
        toast.classList.remove('hidden');
        clearTimeout(toastTimeout);
        toastTimeout = setTimeout(() => toast.classList.add('hidden'), 3000);
    }

    function formatDate(date) {
        const mm  = String(date.getMonth() + 1).padStart(2, '0');
        const dd  = String(date.getDate()).padStart(2, '0');
        const HH  = String(date.getHours()).padStart(2, '0');
        const min = String(date.getMinutes()).padStart(2, '0');
        return `${mm}月${dd}日 ${HH}:${min}`;
    }

    refreshBtn.addEventListener('click', loadSchedules);
    loadSchedules();
});
