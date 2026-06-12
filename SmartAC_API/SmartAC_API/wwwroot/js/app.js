document.addEventListener('DOMContentLoaded', () => {
    // DOM Elements
    const megaBtns = document.querySelectorAll('.mega-btn');
    const customTimeInput = document.getElementById('custom-time');
    const btnCustomOn = document.getElementById('btn-custom-on');
    const btnCustomOff = document.getElementById('btn-custom-off');
    
    const scheduleList = document.getElementById('schedule-list');
    const refreshBtn = document.getElementById('refresh-btn');
    const loader = document.getElementById('fullscreen-loader');
    const toast = document.getElementById('toast');

    // 溫度控制邏輯
    const tempValue = document.getElementById('temp-value');
    const btnTempUp = document.getElementById('btn-temp-up');
    const btnTempDown = document.getElementById('btn-temp-down');
    let currentTemp = 27;

    if (btnTempUp && btnTempDown && tempValue) {
        btnTempUp.addEventListener('click', () => {
            if (currentTemp < 30) {
                currentTemp++;
                tempValue.innerText = currentTemp;
            }
        });
        btnTempDown.addEventListener('click', () => {
            if (currentTemp > 18) {
                currentTemp--;
                tempValue.innerText = currentTemp;
            }
        });
    }

    // 1. 綁定大按鈕點擊事件 (延遲開/關)
    megaBtns.forEach(btn => {
        btn.addEventListener('click', async () => {
            const action = btn.dataset.action;
            const delayMinutes = parseInt(btn.dataset.delay);
            
            await sendScheduleRequest({ Action: action, DelayMinutes: delayMinutes, Temperature: currentTemp });
        });
    });

    // 2. 綁定指定時間按鈕事件
    const handleCustomTime = async (action) => {
        const time = customTimeInput.value;
        if (!time) {
            showToast('請先選擇時間喔！', true);
            return;
        }
        await sendScheduleRequest({ Action: action, TargetTime: time, Temperature: currentTemp });
        customTimeInput.value = ''; // 清空輸入
    };

    btnCustomOn.addEventListener('click', () => handleCustomTime('turn_on'));
    btnCustomOff.addEventListener('click', () => handleCustomTime('turn_off'));

    // 發送排程請求到 API
    async function sendScheduleRequest(payload) {
        showLoader(true);
        try {
            const response = await fetch('/api/schedule', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });

            const result = await response.json();

            if (response.ok) {
                showToast('設定成功！', false);
                await loadSchedules(); // 重新載入列表
            } else {
                showToast('設定失敗，請稍後再試', true);
            }
        } catch (error) {
            showToast('連線異常，請檢查網路', true);
            console.error(error);
        } finally {
            showLoader(false);
        }
    }

    // 載入與繪製排程清單
    async function loadSchedules() {
        try {
            const response = await fetch('/api/schedules');
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
            const isTurnOn = item.action === 'turn_on';
            
            const card = document.createElement('div');
            card.className = 'schedule-card';
            card.innerHTML = `
                <div class="schedule-info">
                    <div class="badge ${isTurnOn ? 'badge-on' : 'badge-off'}">
                        ${isTurnOn ? '開冷氣' : '關冷氣'}
                    </div>
                    <div class="schedule-time">
                        執行時間：${formatDate(executeTime)}
                    </div>
                </div>
                <button class="delete-btn" data-id="${item.messageId}">取消</button>
            `;
            
            scheduleList.appendChild(card);
        });

        // 綁定取消按鈕事件
        document.querySelectorAll('.delete-btn').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                const id = e.target.dataset.id;
                e.target.innerText = '取消中...';
                e.target.disabled = true;
                
                try {
                    const res = await fetch(`/api/schedule/${id}`, { method: 'DELETE' });
                    if (res.ok) {
                        showToast('已取消！', false);
                        await loadSchedules();
                    } else {
                        throw new Error('Delete failed');
                    }
                } catch (error) {
                    showToast('取消失敗', true);
                    e.target.innerText = '取消';
                    e.target.disabled = false;
                }
            });
        });
    }

    // 輔助函式：顯示/隱藏全螢幕 Loader
    function showLoader(show) {
        if (show) {
            loader.classList.remove('hidden');
        } else {
            loader.classList.add('hidden');
        }
    }

    // 輔助函式：顯示 Toast 訊息
    let toastTimeout;
    function showToast(msg, isError) {
        toast.innerText = msg;
        if (isError) {
            toast.classList.add('error');
        } else {
            toast.classList.remove('error');
        }
        
        toast.classList.remove('hidden');
        
        clearTimeout(toastTimeout);
        toastTimeout = setTimeout(() => {
            toast.classList.add('hidden');
        }, 3000);
    }

    function formatDate(date) {
        const mm = String(date.getMonth() + 1).padStart(2, '0');
        const dd = String(date.getDate()).padStart(2, '0');
        const HH = String(date.getHours()).padStart(2, '0');
        const min = String(date.getMinutes()).padStart(2, '0');
        return `${mm}月${dd}日 ${HH}:${min}`;
    }

    // 綁定重新整理按鈕
    refreshBtn.addEventListener('click', loadSchedules);

    // 初始載入
    loadSchedules();
});
