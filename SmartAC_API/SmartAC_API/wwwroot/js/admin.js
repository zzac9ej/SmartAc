// admin.js — ADMIN 主控台邏輯
// URL 格式：/a/{token}

document.addEventListener('DOMContentLoaded', async () => {
    // ── 1. 從 URL 路徑讀取 token ─────────────────────────────────────────────
    const pathParts = window.location.pathname.split('/');
    const token = pathParts[pathParts.length - 1]; // /a/m9q2z7wk → m9q2z7wk

    const errorPage        = document.getElementById('error-page');
    const mainPage         = document.getElementById('main-page');
    const roomTabsEl       = document.getElementById('room-tabs');
    const currentRoomLabel = document.getElementById('current-room-label');

    // ── 2. 驗證身份 ──────────────────────────────────────────────────────────
    let rooms    = [];
    let activeRoomId = '';

    try {
        const res = await fetch(`/api/auth/me?token=${encodeURIComponent(token)}`);
        if (!res.ok) throw new Error('Unauthorized');
        const data = await res.json();

        if (data.role !== 'admin') throw new Error('Not an admin token');

        rooms        = data.rooms; // [{id, name}, ...]
        activeRoomId = rooms[0]?.id ?? '';
    } catch {
        errorPage.classList.remove('hidden');
        return;
    }

    // 驗證成功，顯示主頁面
    mainPage.classList.remove('hidden');

    // ── 3. 建立 Room Tabs ────────────────────────────────────────────────────
    const ROOM_ICONS = { home: '🏠', office: '🏢' };

    function renderTabs() {
        roomTabsEl.innerHTML = '';
        rooms.forEach(room => {
            const icon = ROOM_ICONS[room.id] ?? '📍';
            const btn  = document.createElement('button');
            btn.className   = 'room-tab' + (room.id === activeRoomId ? ' active' : '');
            btn.dataset.id  = room.id;
            btn.innerHTML   = `${icon} ${room.name}`;
            btn.addEventListener('click', () => switchRoom(room.id));
            roomTabsEl.appendChild(btn);
        });

        // 更新當前地點標示
        const current  = rooms.find(r => r.id === activeRoomId);
        const icon     = ROOM_ICONS[activeRoomId] ?? '📍';
        currentRoomLabel.textContent = `${icon} 正在控制：${current?.name ?? ''}`;
        document.title = `${current?.name ?? ''} | 冷氣主控台`;
    }

    function switchRoom(roomId) {
        activeRoomId = roomId;
        renderTabs();
        loadSchedules(); // 切換房間時重新載入排程
    }

    renderTabs();

    // ── 4. DOM 元素 ───────────────────────────────────────────────────────────
    const megaBtns        = document.querySelectorAll('.mega-btn');
    const customTimeInput = document.getElementById('custom-time');
    const btnCustomOn     = document.getElementById('btn-custom-on');
    const btnCustomOff    = document.getElementById('btn-custom-off');
    const scheduleList    = document.getElementById('schedule-list');
    const refreshBtn      = document.getElementById('refresh-btn');
    const loader          = document.getElementById('fullscreen-loader');
    const toast           = document.getElementById('toast');
    const tempValue       = document.getElementById('temp-value');
    const btnTempUp       = document.getElementById('btn-temp-up');
    const btnTempDown     = document.getElementById('btn-temp-down');

    let currentTemp = 27;

    // ── 5. 溫度控制 ──────────────────────────────────────────────────────────
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

    // ── 6. 大按鈕事件 ────────────────────────────────────────────────────────
    megaBtns.forEach(btn => {
        btn.addEventListener('click', async () => {
            const action       = btn.dataset.action;
            const delayMinutes = parseInt(btn.dataset.delay);
            await sendScheduleRequest({ Action: action, DelayMinutes: delayMinutes, Temperature: currentTemp });
        });
    });

    // ── 7. 指定時間事件 ──────────────────────────────────────────────────────
    const handleCustomTime = async (action) => {
        const time = customTimeInput.value;
        if (!time) { showToast('請先選擇時間喔！', true); return; }
        await sendScheduleRequest({ Action: action, TargetTime: time, Temperature: currentTemp });
        customTimeInput.value = '';
    };

    btnCustomOn.addEventListener('click', () => handleCustomTime('turn_on'));
    btnCustomOff.addEventListener('click', () => handleCustomTime('turn_off'));

    // ── 8. 核心：發送排程請求 ─────────────────────────────────────────────────
    async function sendScheduleRequest(payload, silent = false) {
        if (!silent) showLoader(true);
        // ADMIN 帶入當前選取的 roomId
        payload.RoomId = activeRoomId;
        try {
            const response = await fetch(`/api/schedule?token=${encodeURIComponent(token)}`, {
                method:  'POST',
                headers: { 'Content-Type': 'application/json' },
                body:    JSON.stringify(payload)
            });

            if (response.ok) {
                showToast('設定成功！', false);
                await loadSchedules();
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

    // ── 9. 載入排程清單 ───────────────────────────────────────────────────────
    async function loadSchedules() {
        try {
            // ADMIN 傳 roomId 只看當前地點的排程
            const response = await fetch(
                `/api/schedules?token=${encodeURIComponent(token)}&roomId=${encodeURIComponent(activeRoomId)}`
            );
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
